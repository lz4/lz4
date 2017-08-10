/*
LZ4 auto-framing library
Copyright (C) 2011-2016, Yann Collet.

BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

* Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the
distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

You can contact the author at :
- LZ4 homepage : http://www.lz4.org
- LZ4 source repository : https://github.com/lz4/lz4
*/

/* LZ4F is a stand-alone API to create LZ4-compressed Frames
*  in full conformance with specification v1.5.0
*  All related operations, including memory management, are handled by the library.
* */


/*-************************************
*  Compiler Options
**************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  pragma warning(disable : 4127)        /* disable: C4127: conditional expression is constant */
#endif


/*-************************************
*  Memory routines
**************************************/
#include <stdlib.h>   /* malloc, calloc, free */
#define ALLOCATOR(s)   calloc(1,s)
#define FREEMEM        free
#include <string.h>   /* memset, memcpy, memmove */
#define MEM_INIT       memset


/*-************************************
*  Includes
**************************************/
#include "lz4frame_static.h"
#include "lz4.h"
#define LZ4_HC_STATIC_LINKING_ONLY
#include "lz4hc.h"
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"


/*-************************************
*  Common Utils
**************************************/
#define LZ4_STATIC_ASSERT(c)    { enum { LZ4_static_assert = 1/(int)(!!(c)) }; }   /* use only *after* variable declarations */


/*-************************************
*  Basic Types
**************************************/
#if !defined (__VMS) && (defined (__cplusplus) || (defined (__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L) /* C99 */) )
# include <stdint.h>
  typedef  uint8_t BYTE;
  typedef uint16_t U16;
  typedef uint32_t U32;
  typedef  int32_t S32;
  typedef uint64_t U64;
#else
  typedef unsigned char       BYTE;
  typedef unsigned short      U16;
  typedef unsigned int        U32;
  typedef   signed int        S32;
  typedef unsigned long long  U64;
#endif


/* unoptimized version; solves endianess & alignment issues */
static U32 LZ4F_readLE32 (const void* src)
{
    const BYTE* const srcPtr = (const BYTE*)src;
    U32 value32 = srcPtr[0];
    value32 += (srcPtr[1]<<8);
    value32 += (srcPtr[2]<<16);
    value32 += ((U32)srcPtr[3])<<24;
    return value32;
}

static void LZ4F_writeLE32 (void* dst, U32 value32)
{
    BYTE* const dstPtr = (BYTE*)dst;
    dstPtr[0] = (BYTE)value32;
    dstPtr[1] = (BYTE)(value32 >> 8);
    dstPtr[2] = (BYTE)(value32 >> 16);
    dstPtr[3] = (BYTE)(value32 >> 24);
}

static U64 LZ4F_readLE64 (const void* src)
{
    const BYTE* const srcPtr = (const BYTE*)src;
    U64 value64 = srcPtr[0];
    value64 += ((U64)srcPtr[1]<<8);
    value64 += ((U64)srcPtr[2]<<16);
    value64 += ((U64)srcPtr[3]<<24);
    value64 += ((U64)srcPtr[4]<<32);
    value64 += ((U64)srcPtr[5]<<40);
    value64 += ((U64)srcPtr[6]<<48);
    value64 += ((U64)srcPtr[7]<<56);
    return value64;
}

static void LZ4F_writeLE64 (void* dst, U64 value64)
{
    BYTE* const dstPtr = (BYTE*)dst;
    dstPtr[0] = (BYTE)value64;
    dstPtr[1] = (BYTE)(value64 >> 8);
    dstPtr[2] = (BYTE)(value64 >> 16);
    dstPtr[3] = (BYTE)(value64 >> 24);
    dstPtr[4] = (BYTE)(value64 >> 32);
    dstPtr[5] = (BYTE)(value64 >> 40);
    dstPtr[6] = (BYTE)(value64 >> 48);
    dstPtr[7] = (BYTE)(value64 >> 56);
}


/*-************************************
*  Constants
**************************************/
#define KB *(1<<10)
#define MB *(1<<20)
#define GB *(1<<30)

#define _1BIT  0x01
#define _2BITS 0x03
#define _3BITS 0x07
#define _4BITS 0x0F
#define _8BITS 0xFF

#define LZ4F_MAGIC_SKIPPABLE_START 0x184D2A50U
#define LZ4F_MAGICNUMBER 0x184D2204U
#define LZ4F_BLOCKUNCOMPRESSED_FLAG 0x80000000U
#define LZ4F_BLOCKSIZEID_DEFAULT LZ4F_max64KB

static const size_t minFHSize = 7;
static const size_t maxFHSize = LZ4F_HEADER_SIZE_MAX;   /* 19 */
static const size_t BHSize = 4;


/*-************************************
*  Structures and local types
**************************************/
typedef struct LZ4F_cctx_s
{
    LZ4F_preferences_t prefs;
    U32    version;
    U32    cStage;
    const LZ4F_CDict* cdict;
    size_t maxBlockSize;
    size_t maxBufferSize;
    BYTE*  tmpBuff;
    BYTE*  tmpIn;
    size_t tmpInSize;
    U64    totalInSize;
    XXH32_state_t xxh;
    void*  lz4CtxPtr;
    U32    lz4CtxLevel;   /* 0: unallocated;  1: LZ4_stream_t;  3: LZ4_streamHC_t */
} LZ4F_cctx_t;


/*-************************************
*  Error management
**************************************/
#define LZ4F_GENERATE_STRING(STRING) #STRING,
static const char* LZ4F_errorStrings[] = { LZ4F_LIST_ERRORS(LZ4F_GENERATE_STRING) };


unsigned LZ4F_isError(LZ4F_errorCode_t code)
{
    return (code > (LZ4F_errorCode_t)(-LZ4F_ERROR_maxCode));
}

const char* LZ4F_getErrorName(LZ4F_errorCode_t code)
{
    static const char* codeError = "Unspecified error code";
    if (LZ4F_isError(code)) return LZ4F_errorStrings[-(int)(code)];
    return codeError;
}

LZ4F_errorCodes LZ4F_getErrorCode(size_t functionResult)
{
    if (!LZ4F_isError(functionResult)) return LZ4F_OK_NoError;
    return (LZ4F_errorCodes)(-(ptrdiff_t)functionResult);
}

static LZ4F_errorCode_t err0r(LZ4F_errorCodes code)
{
    /* A compilation error here means sizeof(ptrdiff_t) is not large enough */
    LZ4_STATIC_ASSERT(sizeof(ptrdiff_t) >= sizeof(size_t));
    return (LZ4F_errorCode_t)-(ptrdiff_t)code;
}

unsigned LZ4F_getVersion(void) { return LZ4F_VERSION; }

int LZ4F_compressionLevel_max(void) { return LZ4HC_CLEVEL_MAX; }


/*-************************************
*  Private functions
**************************************/
#define MIN(a,b)   ( (a) < (b) ? (a) : (b) )

static size_t LZ4F_getBlockSize(unsigned blockSizeID)
{
    static const size_t blockSizes[4] = { 64 KB, 256 KB, 1 MB, 4 MB };

    if (blockSizeID == 0) blockSizeID = LZ4F_BLOCKSIZEID_DEFAULT;
    blockSizeID -= 4;
    if (blockSizeID > 3) return err0r(LZ4F_ERROR_maxBlockSize_invalid);
    return blockSizes[blockSizeID];
}

static BYTE LZ4F_headerChecksum (const void* header, size_t length)
{
    U32 const xxh = XXH32(header, length, 0);
    return (BYTE)(xxh >> 8);
}


/*-************************************
*  Simple-pass compression functions
**************************************/
static LZ4F_blockSizeID_t LZ4F_optimalBSID(const LZ4F_blockSizeID_t requestedBSID,
                                           const size_t srcSize)
{
    LZ4F_blockSizeID_t proposedBSID = LZ4F_max64KB;
    size_t maxBlockSize = 64 KB;
    while (requestedBSID > proposedBSID) {
        if (srcSize <= maxBlockSize)
            return proposedBSID;
        proposedBSID = (LZ4F_blockSizeID_t)((int)proposedBSID + 1);
        maxBlockSize <<= 2;
    }
    return requestedBSID;
}

/* LZ4F_compressBound_internal() :
 * Provides dstCapacity given a srcSize to guarantee operation success in worst case situations.
 * prefsPtr is optional : if NULL is provided, preferences will be set to cover worst case scenario.
 * Result is always the same for a srcSize and prefsPtr, so it can be relied upon to size reusable buffers.
 * When srcSize==0, LZ4F_compressBound() provides an upper bound for LZ4F_flush() and LZ4F_compressEnd() operations.
 */
static size_t LZ4F_compressBound_internal(size_t srcSize,
                                    const LZ4F_preferences_t* preferencesPtr,
                                          size_t alreadyBuffered)
{
    LZ4F_preferences_t prefsNull;
    memset(&prefsNull, 0, sizeof(prefsNull));
    prefsNull.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;   /* worst case */
    {   const LZ4F_preferences_t* const prefsPtr = (preferencesPtr==NULL) ? &prefsNull : preferencesPtr;
        U32 const flush = prefsPtr->autoFlush | (srcSize==0);
        LZ4F_blockSizeID_t const bid = prefsPtr->frameInfo.blockSizeID;
        size_t const blockSize = LZ4F_getBlockSize(bid);
        size_t const maxBuffered = blockSize - 1;
        size_t const bufferedSize = MIN(alreadyBuffered, maxBuffered);
        size_t const maxSrcSize = srcSize + bufferedSize;
        unsigned const nbFullBlocks = (unsigned)(maxSrcSize / blockSize);
        size_t const partialBlockSize = (srcSize - (srcSize==0)) & (blockSize-1);   /* 0 => -1 == MAX => blockSize-1 */
        size_t const lastBlockSize = flush ? partialBlockSize : 0;
        unsigned const nbBlocks = nbFullBlocks + (lastBlockSize>0);

        size_t const blockHeaderSize = 4;   /* default, without block CRC option (which cannot be generated with current API) */
        size_t const frameEnd = 4 + (prefsPtr->frameInfo.contentChecksumFlag*4);

        return (blockHeaderSize * nbBlocks) + (blockSize * nbFullBlocks) + lastBlockSize + frameEnd;;
    }
}

size_t LZ4F_compressFrameBound(size_t srcSize, const LZ4F_preferences_t* preferencesPtr)
{
    LZ4F_preferences_t prefs;
    size_t const headerSize = maxFHSize;      /* max header size, including optional fields */

    if (preferencesPtr!=NULL) prefs = *preferencesPtr;
    else memset(&prefs, 0, sizeof(prefs));
    prefs.autoFlush = 1;

    return headerSize + LZ4F_compressBound_internal(srcSize, &prefs, 0);;
}


/*! LZ4F_compressFrame_usingCDict() :
 *  Compress srcBuffer using a dictionary, in a single step.
 *  cdict can be NULL, in which case, no dictionary is used.
 *  dstBuffer MUST be >= LZ4F_compressFrameBound(srcSize, preferencesPtr).
 *  The LZ4F_preferences_t structure is optional : you may provide NULL as argument,
 *  however, it's the only way to provide a dictID, so it's not recommended.
 * @return : number of bytes written into dstBuffer,
 *           or an error code if it fails (can be tested using LZ4F_isError())
 */
size_t LZ4F_compressFrame_usingCDict(void* dstBuffer, size_t dstCapacity,
                               const void* srcBuffer, size_t srcSize,
                               const LZ4F_CDict* cdict,
                               const LZ4F_preferences_t* preferencesPtr)
{
    LZ4F_cctx_t cctxI;
    LZ4_stream_t lz4ctx;
    LZ4F_preferences_t prefs;
    LZ4F_compressOptions_t options;
    BYTE* const dstStart = (BYTE*) dstBuffer;
    BYTE* dstPtr = dstStart;
    BYTE* const dstEnd = dstStart + dstCapacity;

    memset(&cctxI, 0, sizeof(cctxI));
    cctxI.version = LZ4F_VERSION;
    cctxI.maxBufferSize = 5 MB;   /* mess with real buffer size to prevent dynamic allocation; works only because autoflush==1 & stableSrc==1 */

    if (preferencesPtr!=NULL)
        prefs = *preferencesPtr;
    else
        memset(&prefs, 0, sizeof(prefs));
    if (prefs.frameInfo.contentSize != 0)
        prefs.frameInfo.contentSize = (U64)srcSize;   /* auto-correct content size if selected (!=0) */

    prefs.frameInfo.blockSizeID = LZ4F_optimalBSID(prefs.frameInfo.blockSizeID, srcSize);
    prefs.autoFlush = 1;
    if (srcSize <= LZ4F_getBlockSize(prefs.frameInfo.blockSizeID))
        prefs.frameInfo.blockMode = LZ4F_blockIndependent;   /* only one block => no need for inter-block link */

    if (prefs.compressionLevel < LZ4HC_CLEVEL_MIN) {
        cctxI.lz4CtxPtr = &lz4ctx;
        cctxI.lz4CtxLevel = 1;
    }  /* fast compression context pre-created on stack */

    memset(&options, 0, sizeof(options));
    options.stableSrc = 1;

    if (dstCapacity < LZ4F_compressFrameBound(srcSize, &prefs))  /* condition to guarantee success */
        return err0r(LZ4F_ERROR_dstMaxSize_tooSmall);

    { size_t const headerSize = LZ4F_compressBegin_usingCDict(&cctxI, dstBuffer, dstCapacity, cdict, &prefs);  /* write header */
      if (LZ4F_isError(headerSize)) return headerSize;
      dstPtr += headerSize;   /* header size */ }

    { size_t const cSize = LZ4F_compressUpdate(&cctxI, dstPtr, dstEnd-dstPtr, srcBuffer, srcSize, &options);
      if (LZ4F_isError(cSize)) return cSize;
      dstPtr += cSize; }

    { size_t const tailSize = LZ4F_compressEnd(&cctxI, dstPtr, dstEnd-dstPtr, &options);   /* flush last block, and generate suffix */
      if (LZ4F_isError(tailSize)) return tailSize;
      dstPtr += tailSize; }

    if (prefs.compressionLevel >= LZ4HC_CLEVEL_MIN)  /* Ctx allocation only for lz4hc */
        FREEMEM(cctxI.lz4CtxPtr);

    return (dstPtr - dstStart);
}


/*! LZ4F_compressFrame() :
 *  Compress an entire srcBuffer into a valid LZ4 frame, in a single step.
 *  dstBuffer MUST be >= LZ4F_compressFrameBound(srcSize, preferencesPtr).
 *  The LZ4F_preferences_t structure is optional : you can provide NULL as argument. All preferences will be set to default.
 * @return : number of bytes written into dstBuffer.
 *           or an error code if it fails (can be tested using LZ4F_isError())
 */
size_t LZ4F_compressFrame(void* dstBuffer, size_t dstCapacity,
                    const void* srcBuffer, size_t srcSize,
                    const LZ4F_preferences_t* preferencesPtr)
{
    return LZ4F_compressFrame_usingCDict(dstBuffer, dstCapacity,
                                         srcBuffer, srcSize,
                                         NULL, preferencesPtr);
}


/*-***************************************************
*   Dictionary compression
*****************************************************/

struct LZ4F_CDict_s {
    void* dictContent;
    LZ4_stream_t* fastCtx;
    LZ4_streamHC_t* HCCtx;
}; /* typedef'd to LZ4F_CDict within lz4frame_static.h */

/*! LZ4_createCDict() :
 *  When compressing multiple messages / blocks with the same dictionary, it's recommended to load it just once.
 *  LZ4_createCDict() will create a digested dictionary, ready to start future compression operations without startup delay.
 *  LZ4_CDict can be created once and shared by multiple threads concurrently, since its usage is read-only.
 * `dictBuffer` can be released after LZ4_CDict creation, since its content is copied within CDict
 * @return : digested dictionary for compression, or NULL if failed */
LZ4F_CDict* LZ4F_createCDict(const void* dictBuffer, size_t dictSize)
{
    const char* dictStart = (const char*)dictBuffer;
    LZ4F_CDict* cdict = (LZ4F_CDict*) malloc(sizeof(*cdict));
    if (!cdict) return NULL;
    if (dictSize > 64 KB) {
        dictStart += dictSize - 64 KB;
        dictSize = 64 KB;
    }
    cdict->dictContent = ALLOCATOR(dictSize);
    cdict->fastCtx = LZ4_createStream();
    cdict->HCCtx = LZ4_createStreamHC();
    if (!cdict->dictContent || !cdict->fastCtx || !cdict->HCCtx) {
        LZ4F_freeCDict(cdict);
        return NULL;
    }
    memcpy(cdict->dictContent, dictStart, dictSize);
    LZ4_resetStream(cdict->fastCtx);
    LZ4_loadDict (cdict->fastCtx, (const char*)cdict->dictContent, (int)dictSize);
    LZ4_resetStreamHC(cdict->HCCtx, LZ4HC_CLEVEL_DEFAULT);
    LZ4_loadDictHC(cdict->HCCtx, (const char*)cdict->dictContent, (int)dictSize);
    return cdict;
}

void LZ4F_freeCDict(LZ4F_CDict* cdict)
{
    if (cdict==NULL) return;  /* support free on NULL */
    FREEMEM(cdict->dictContent);
    LZ4_freeStream(cdict->fastCtx);
    LZ4_freeStreamHC(cdict->HCCtx);
    FREEMEM(cdict);
}


/*-*********************************
*  Advanced compression functions
***********************************/

/*! LZ4F_createCompressionContext() :
 *  The first thing to do is to create a compressionContext object, which will be used in all compression operations.
 *  This is achieved using LZ4F_createCompressionContext(), which takes as argument a version and an LZ4F_preferences_t structure.
 *  The version provided MUST be LZ4F_VERSION. It is intended to track potential incompatible differences between different binaries.
 *  The function will provide a pointer to an allocated LZ4F_compressionContext_t object.
 *  If the result LZ4F_errorCode_t is not OK_NoError, there was an error during context creation.
 *  Object can release its memory using LZ4F_freeCompressionContext();
 */
LZ4F_errorCode_t LZ4F_createCompressionContext(LZ4F_compressionContext_t* LZ4F_compressionContextPtr, unsigned version)
{
    LZ4F_cctx_t* const cctxPtr = (LZ4F_cctx_t*)ALLOCATOR(sizeof(LZ4F_cctx_t));
    if (cctxPtr==NULL) return err0r(LZ4F_ERROR_allocation_failed);

    cctxPtr->version = version;
    cctxPtr->cStage = 0;   /* Next stage : init stream */

    *LZ4F_compressionContextPtr = (LZ4F_compressionContext_t)cctxPtr;

    return LZ4F_OK_NoError;
}


LZ4F_errorCode_t LZ4F_freeCompressionContext(LZ4F_compressionContext_t LZ4F_compressionContext)
{
    LZ4F_cctx_t* const cctxPtr = (LZ4F_cctx_t*)LZ4F_compressionContext;

    if (cctxPtr != NULL) {  /* support free on NULL */
       FREEMEM(cctxPtr->lz4CtxPtr);  /* works because LZ4_streamHC_t and LZ4_stream_t are simple POD types */
       FREEMEM(cctxPtr->tmpBuff);
       FREEMEM(LZ4F_compressionContext);
    }

    return LZ4F_OK_NoError;
}


/*! LZ4F_compressBegin_usingCDict() :
 *  init streaming compression and writes frame header into dstBuffer.
 *  dstBuffer must be >= LZ4F_HEADER_SIZE_MAX bytes.
 * @return : number of bytes written into dstBuffer for the header
 *           or an error code (can be tested using LZ4F_isError())
 */
size_t LZ4F_compressBegin_usingCDict(LZ4F_cctx* cctxPtr,
                          void* dstBuffer, size_t dstCapacity,
                          const LZ4F_CDict* cdict,
                          const LZ4F_preferences_t* preferencesPtr)
{
    LZ4F_preferences_t prefNull;
    BYTE* const dstStart = (BYTE*)dstBuffer;
    BYTE* dstPtr = dstStart;
    BYTE* headerStart;

    if (dstCapacity < maxFHSize) return err0r(LZ4F_ERROR_dstMaxSize_tooSmall);
    memset(&prefNull, 0, sizeof(prefNull));
    if (preferencesPtr == NULL) preferencesPtr = &prefNull;
    cctxPtr->prefs = *preferencesPtr;

    /* Ctx Management */
    {   U32 const tableID = (cctxPtr->prefs.compressionLevel < LZ4HC_CLEVEL_MIN) ? 1 : 2;  /* 0:nothing ; 1:LZ4 table ; 2:HC tables */
        if (cctxPtr->lz4CtxLevel < tableID) {
            FREEMEM(cctxPtr->lz4CtxPtr);
            if (cctxPtr->prefs.compressionLevel < LZ4HC_CLEVEL_MIN)
                cctxPtr->lz4CtxPtr = (void*)LZ4_createStream();
            else
                cctxPtr->lz4CtxPtr = (void*)LZ4_createStreamHC();
            if (cctxPtr->lz4CtxPtr == NULL) return err0r(LZ4F_ERROR_allocation_failed);
            cctxPtr->lz4CtxLevel = tableID;
    }   }

    /* Buffer Management */
    if (cctxPtr->prefs.frameInfo.blockSizeID == 0)
        cctxPtr->prefs.frameInfo.blockSizeID = LZ4F_BLOCKSIZEID_DEFAULT;
    cctxPtr->maxBlockSize = LZ4F_getBlockSize(cctxPtr->prefs.frameInfo.blockSizeID);

    {   size_t const requiredBuffSize = preferencesPtr->autoFlush ?
                (cctxPtr->prefs.frameInfo.blockMode == LZ4F_blockLinked) * 64 KB :  /* only needs windows size */
                cctxPtr->maxBlockSize + ((cctxPtr->prefs.frameInfo.blockMode == LZ4F_blockLinked) * 128 KB);

        if (cctxPtr->maxBufferSize < requiredBuffSize) {
            cctxPtr->maxBufferSize = 0;
            FREEMEM(cctxPtr->tmpBuff);
            cctxPtr->tmpBuff = (BYTE*)ALLOCATOR(requiredBuffSize);
            if (cctxPtr->tmpBuff == NULL) return err0r(LZ4F_ERROR_allocation_failed);
            cctxPtr->maxBufferSize = requiredBuffSize;
    }   }
    cctxPtr->tmpIn = cctxPtr->tmpBuff;
    cctxPtr->tmpInSize = 0;
    XXH32_reset(&(cctxPtr->xxh), 0);

    /* context init */
    cctxPtr->cdict = cdict;
    if (cctxPtr->prefs.frameInfo.blockMode == LZ4F_blockLinked) {
        /* frame init only for blockLinked : blockIndependent will be init at each block */
        if (cdict) {
            if (cctxPtr->prefs.compressionLevel < LZ4HC_CLEVEL_MIN) {
                memcpy(cctxPtr->lz4CtxPtr, cdict->fastCtx, sizeof(*cdict->fastCtx));
            } else {
                memcpy(cctxPtr->lz4CtxPtr, cdict->HCCtx, sizeof(*cdict->HCCtx));
                LZ4_setCompressionLevel((LZ4_streamHC_t*)cctxPtr->lz4CtxPtr, cctxPtr->prefs.compressionLevel);
            }
        } else {
            if (cctxPtr->prefs.compressionLevel < LZ4HC_CLEVEL_MIN)
                LZ4_resetStream((LZ4_stream_t*)(cctxPtr->lz4CtxPtr));
            else
                LZ4_resetStreamHC((LZ4_streamHC_t*)(cctxPtr->lz4CtxPtr), cctxPtr->prefs.compressionLevel);
        }
    }

    /* Magic Number */
    LZ4F_writeLE32(dstPtr, LZ4F_MAGICNUMBER);
    dstPtr += 4;
    headerStart = dstPtr;

    /* FLG Byte */
    *dstPtr++ = (BYTE)(((1 & _2BITS) << 6)    /* Version('01') */
        + ((cctxPtr->prefs.frameInfo.blockMode & _1BIT ) << 5)  /* Block mode */
        + ((cctxPtr->prefs.frameInfo.contentSize > 0) << 3)     /* Frame content size */
        + ((cctxPtr->prefs.frameInfo.contentChecksumFlag & _1BIT ) << 2)   /* Frame checksum */
        +  (cctxPtr->prefs.frameInfo.dictID > 0) );   /* Dictionary ID */
    /* BD Byte */
    *dstPtr++ = (BYTE)((cctxPtr->prefs.frameInfo.blockSizeID & _3BITS) << 4);
    /* Optional Frame content size field */
    if (cctxPtr->prefs.frameInfo.contentSize) {
        LZ4F_writeLE64(dstPtr, cctxPtr->prefs.frameInfo.contentSize);
        dstPtr += 8;
        cctxPtr->totalInSize = 0;
    }
    /* Optional dictionary ID field */
    if (cctxPtr->prefs.frameInfo.dictID) {
        LZ4F_writeLE32(dstPtr, cctxPtr->prefs.frameInfo.dictID);
        dstPtr += 4;
    }
    /* Header CRC Byte */
    *dstPtr = LZ4F_headerChecksum(headerStart, dstPtr - headerStart);
    dstPtr++;

    cctxPtr->cStage = 1;   /* header written, now request input data block */
    return (dstPtr - dstStart);
}


/*! LZ4F_compressBegin() :
 *  init streaming compression and writes frame header into dstBuffer.
 *  dstBuffer must be >= LZ4F_HEADER_SIZE_MAX bytes.
 *  preferencesPtr can be NULL, in which case default parameters are selected.
 * @return : number of bytes written into dstBuffer for the header
 *           or an error code (can be tested using LZ4F_isError())
 */
size_t LZ4F_compressBegin(LZ4F_cctx* cctxPtr,
                          void* dstBuffer, size_t dstCapacity,
                          const LZ4F_preferences_t* preferencesPtr)
{
    return LZ4F_compressBegin_usingCDict(cctxPtr, dstBuffer, dstCapacity,
                                         NULL, preferencesPtr);
}


/* LZ4F_compressBound() :
 *      @ return size of Dst buffer given a srcSize to handle worst case situations.
 *      The LZ4F_frameInfo_t structure is optional : if NULL, preferences will be set to cover worst case situations.
 *      This function cannot fail.
 */
size_t LZ4F_compressBound(size_t srcSize, const LZ4F_preferences_t* preferencesPtr)
{
    return LZ4F_compressBound_internal(srcSize, preferencesPtr, (size_t)-1);
}


typedef int (*compressFunc_t)(void* ctx, const char* src, char* dst, int srcSize, int dstSize, int level, const LZ4F_CDict* cdict);

static size_t LZ4F_makeBlock(void* dst, const void* src, size_t srcSize, compressFunc_t compress, void* lz4ctx, int level, const LZ4F_CDict* cdict)
{
    /* compress a single block */
    BYTE* const cSizePtr = (BYTE*)dst;
    U32 cSize = (U32)compress(lz4ctx, (const char*)src, (char*)(cSizePtr+4),
                                      (int)(srcSize), (int)(srcSize-1),
                                      level, cdict);
    LZ4F_writeLE32(cSizePtr, cSize);
    if (cSize == 0) {  /* compression failed */
        cSize = (U32)srcSize;
        LZ4F_writeLE32(cSizePtr, cSize | LZ4F_BLOCKUNCOMPRESSED_FLAG);
        memcpy(cSizePtr+4, src, srcSize);
    }
    return cSize + 4;
}


static int LZ4F_compressBlock(void* ctx, const char* src, char* dst, int srcSize, int dstCapacity, int level, const LZ4F_CDict* cdict)
{
    int const acceleration = (level < -1) ? -level : 1;
    if (cdict) {
        memcpy(ctx, cdict->fastCtx, sizeof(*cdict->fastCtx));
        return LZ4_compress_fast_continue((LZ4_stream_t*)ctx, src, dst, srcSize, dstCapacity, acceleration);
    }
    return LZ4_compress_fast_extState(ctx, src, dst, srcSize, dstCapacity, acceleration);
}

static int LZ4F_compressBlock_continue(void* ctx, const char* src, char* dst, int srcSize, int dstCapacity, int level, const LZ4F_CDict* cdict)
{
    int const acceleration = (level < -1) ? -level : 1;
    (void)cdict; /* init once at beginning of frame */
    return LZ4_compress_fast_continue((LZ4_stream_t*)ctx, src, dst, srcSize, dstCapacity, acceleration);
}

static int LZ4F_compressBlockHC(void* ctx, const char* src, char* dst, int srcSize, int dstCapacity, int level, const LZ4F_CDict* cdict)
{
    if (cdict) {
        memcpy(ctx, cdict->HCCtx, sizeof(*cdict->HCCtx));
        LZ4_setCompressionLevel((LZ4_streamHC_t*)ctx, level);
        return LZ4_compress_HC_continue((LZ4_streamHC_t*)ctx, src, dst, srcSize, dstCapacity);
    }
    return LZ4_compress_HC_extStateHC(ctx, src, dst, srcSize, dstCapacity, level);
}

static int LZ4F_compressBlockHC_continue(void* ctx, const char* src, char* dst, int srcSize, int dstCapacity, int level, const LZ4F_CDict* cdict)
{
    (void)level; (void)cdict; /* init once at beginning of frame */
    return LZ4_compress_HC_continue((LZ4_streamHC_t*)ctx, src, dst, srcSize, dstCapacity);
}

static compressFunc_t LZ4F_selectCompression(LZ4F_blockMode_t blockMode, int level)
{
    if (level < LZ4HC_CLEVEL_MIN) {
        if (blockMode == LZ4F_blockIndependent) return LZ4F_compressBlock;
        return LZ4F_compressBlock_continue;
    }
    if (blockMode == LZ4F_blockIndependent) return LZ4F_compressBlockHC;
    return LZ4F_compressBlockHC_continue;
}

static int LZ4F_localSaveDict(LZ4F_cctx_t* cctxPtr)
{
    if (cctxPtr->prefs.compressionLevel < LZ4HC_CLEVEL_MIN)
        return LZ4_saveDict ((LZ4_stream_t*)(cctxPtr->lz4CtxPtr), (char*)(cctxPtr->tmpBuff), 64 KB);
    return LZ4_saveDictHC ((LZ4_streamHC_t*)(cctxPtr->lz4CtxPtr), (char*)(cctxPtr->tmpBuff), 64 KB);
}

typedef enum { notDone, fromTmpBuffer, fromSrcBuffer } LZ4F_lastBlockStatus;

/*! LZ4F_compressUpdate() :
 *  LZ4F_compressUpdate() can be called repetitively to compress as much data as necessary.
 *  dstBuffer MUST be >= LZ4F_compressBound(srcSize, preferencesPtr).
 *  LZ4F_compressOptions_t structure is optional : you can provide NULL as argument.
 * @return : the number of bytes written into dstBuffer. It can be zero, meaning input data was just buffered.
 *           or an error code if it fails (which can be tested using LZ4F_isError())
 */
size_t LZ4F_compressUpdate(LZ4F_cctx* cctxPtr, void* dstBuffer, size_t dstCapacity, const void* srcBuffer, size_t srcSize, const LZ4F_compressOptions_t* compressOptionsPtr)
{
    LZ4F_compressOptions_t cOptionsNull;
    size_t const blockSize = cctxPtr->maxBlockSize;
    const BYTE* srcPtr = (const BYTE*)srcBuffer;
    const BYTE* const srcEnd = srcPtr + srcSize;
    BYTE* const dstStart = (BYTE*)dstBuffer;
    BYTE* dstPtr = dstStart;
    LZ4F_lastBlockStatus lastBlockCompressed = notDone;
    compressFunc_t const compress = LZ4F_selectCompression(cctxPtr->prefs.frameInfo.blockMode, cctxPtr->prefs.compressionLevel);


    if (cctxPtr->cStage != 1) return err0r(LZ4F_ERROR_GENERIC);
    if (dstCapacity < LZ4F_compressBound_internal(srcSize, &(cctxPtr->prefs), cctxPtr->tmpInSize)) return err0r(LZ4F_ERROR_dstMaxSize_tooSmall);
    memset(&cOptionsNull, 0, sizeof(cOptionsNull));
    if (compressOptionsPtr == NULL) compressOptionsPtr = &cOptionsNull;

    /* complete tmp buffer */
    if (cctxPtr->tmpInSize > 0) {   /* some data already within tmp buffer */
        size_t const sizeToCopy = blockSize - cctxPtr->tmpInSize;
        if (sizeToCopy > srcSize) {
            /* add src to tmpIn buffer */
            memcpy(cctxPtr->tmpIn + cctxPtr->tmpInSize, srcBuffer, srcSize);
            srcPtr = srcEnd;
            cctxPtr->tmpInSize += srcSize;
            /* still needs some CRC */
        } else {
            /* complete tmpIn block and then compress it */
            lastBlockCompressed = fromTmpBuffer;
            memcpy(cctxPtr->tmpIn + cctxPtr->tmpInSize, srcBuffer, sizeToCopy);
            srcPtr += sizeToCopy;

            dstPtr += LZ4F_makeBlock(dstPtr, cctxPtr->tmpIn, blockSize, compress, cctxPtr->lz4CtxPtr, cctxPtr->prefs.compressionLevel, cctxPtr->cdict);

            if (cctxPtr->prefs.frameInfo.blockMode==LZ4F_blockLinked) cctxPtr->tmpIn += blockSize;
            cctxPtr->tmpInSize = 0;
        }
    }

    while ((size_t)(srcEnd - srcPtr) >= blockSize) {
        /* compress full blocks */
        lastBlockCompressed = fromSrcBuffer;
        dstPtr += LZ4F_makeBlock(dstPtr, srcPtr, blockSize, compress, cctxPtr->lz4CtxPtr, cctxPtr->prefs.compressionLevel, cctxPtr->cdict);
        srcPtr += blockSize;
    }

    if ((cctxPtr->prefs.autoFlush) && (srcPtr < srcEnd)) {
        /* compress remaining input < blockSize */
        lastBlockCompressed = fromSrcBuffer;
        dstPtr += LZ4F_makeBlock(dstPtr, srcPtr, srcEnd - srcPtr, compress, cctxPtr->lz4CtxPtr, cctxPtr->prefs.compressionLevel, cctxPtr->cdict);
        srcPtr  = srcEnd;
    }

    /* preserve dictionary if necessary */
    if ((cctxPtr->prefs.frameInfo.blockMode==LZ4F_blockLinked) && (lastBlockCompressed==fromSrcBuffer)) {
        if (compressOptionsPtr->stableSrc) {
            cctxPtr->tmpIn = cctxPtr->tmpBuff;
        } else {
            int const realDictSize = LZ4F_localSaveDict(cctxPtr);
            if (realDictSize==0) return err0r(LZ4F_ERROR_GENERIC);
            cctxPtr->tmpIn = cctxPtr->tmpBuff + realDictSize;
        }
    }

    /* keep tmpIn within limits */
    if ((cctxPtr->tmpIn + blockSize) > (cctxPtr->tmpBuff + cctxPtr->maxBufferSize)   /* necessarily LZ4F_blockLinked && lastBlockCompressed==fromTmpBuffer */
        && !(cctxPtr->prefs.autoFlush))
    {
        int const realDictSize = LZ4F_localSaveDict(cctxPtr);
        cctxPtr->tmpIn = cctxPtr->tmpBuff + realDictSize;
    }

    /* some input data left, necessarily < blockSize */
    if (srcPtr < srcEnd) {
        /* fill tmp buffer */
        size_t const sizeToCopy = srcEnd - srcPtr;
        memcpy(cctxPtr->tmpIn, srcPtr, sizeToCopy);
        cctxPtr->tmpInSize = sizeToCopy;
    }

    if (cctxPtr->prefs.frameInfo.contentChecksumFlag == LZ4F_contentChecksumEnabled)
        XXH32_update(&(cctxPtr->xxh), srcBuffer, srcSize);

    cctxPtr->totalInSize += srcSize;
    return dstPtr - dstStart;
}


/*! LZ4F_flush() :
 *  Should you need to create compressed data immediately, without waiting for a block to be filled,
 *  you can call LZ4_flush(), which will immediately compress any remaining data stored within compressionContext.
 *  The result of the function is the number of bytes written into dstBuffer
 *  (it can be zero, this means there was no data left within compressionContext)
 *  The function outputs an error code if it fails (can be tested using LZ4F_isError())
 *  The LZ4F_compressOptions_t structure is optional : you can provide NULL as argument.
 */
size_t LZ4F_flush(LZ4F_cctx* cctxPtr, void* dstBuffer, size_t dstCapacity, const LZ4F_compressOptions_t* compressOptionsPtr)
{
    BYTE* const dstStart = (BYTE*)dstBuffer;
    BYTE* dstPtr = dstStart;
    compressFunc_t compress;

    if (cctxPtr->tmpInSize == 0) return 0;   /* nothing to flush */
    if (cctxPtr->cStage != 1) return err0r(LZ4F_ERROR_GENERIC);
    if (dstCapacity < (cctxPtr->tmpInSize + 4)) return err0r(LZ4F_ERROR_dstMaxSize_tooSmall);   /* +4 : block header(4)  */
    (void)compressOptionsPtr;   /* not yet useful */

    /* select compression function */
    compress = LZ4F_selectCompression(cctxPtr->prefs.frameInfo.blockMode, cctxPtr->prefs.compressionLevel);

    /* compress tmp buffer */
    dstPtr += LZ4F_makeBlock(dstPtr, cctxPtr->tmpIn, cctxPtr->tmpInSize, compress, cctxPtr->lz4CtxPtr, cctxPtr->prefs.compressionLevel, cctxPtr->cdict);
    if (cctxPtr->prefs.frameInfo.blockMode==LZ4F_blockLinked) cctxPtr->tmpIn += cctxPtr->tmpInSize;
    cctxPtr->tmpInSize = 0;

    /* keep tmpIn within limits */
    if ((cctxPtr->tmpIn + cctxPtr->maxBlockSize) > (cctxPtr->tmpBuff + cctxPtr->maxBufferSize)) {  /* necessarily LZ4F_blockLinked */
        int realDictSize = LZ4F_localSaveDict(cctxPtr);
        cctxPtr->tmpIn = cctxPtr->tmpBuff + realDictSize;
    }

    return dstPtr - dstStart;
}


/*! LZ4F_compressEnd() :
 * When you want to properly finish the compressed frame, just call LZ4F_compressEnd().
 * It will flush whatever data remained within compressionContext (like LZ4_flush())
 * but also properly finalize the frame, with an endMark and a checksum.
 * The result of the function is the number of bytes written into dstBuffer (necessarily >= 4 (endMark size))
 * The function outputs an error code if it fails (can be tested using LZ4F_isError())
 * The LZ4F_compressOptions_t structure is optional : you can provide NULL as argument.
 * compressionContext can then be used again, starting with LZ4F_compressBegin(). The preferences will remain the same.
 */
size_t LZ4F_compressEnd(LZ4F_cctx* cctxPtr, void* dstBuffer, size_t dstMaxSize, const LZ4F_compressOptions_t* compressOptionsPtr)
{
    BYTE* const dstStart = (BYTE*)dstBuffer;
    BYTE* dstPtr = dstStart;

    size_t const flushSize = LZ4F_flush(cctxPtr, dstBuffer, dstMaxSize, compressOptionsPtr);
    if (LZ4F_isError(flushSize)) return flushSize;
    dstPtr += flushSize;

    LZ4F_writeLE32(dstPtr, 0);
    dstPtr+=4;   /* endMark */

    if (cctxPtr->prefs.frameInfo.contentChecksumFlag == LZ4F_contentChecksumEnabled) {
        U32 const xxh = XXH32_digest(&(cctxPtr->xxh));
        LZ4F_writeLE32(dstPtr, xxh);
        dstPtr+=4;   /* content Checksum */
    }

    cctxPtr->cStage = 0;   /* state is now re-usable (with identical preferences) */
    cctxPtr->maxBufferSize = 0;  /* reuse HC context */

    if (cctxPtr->prefs.frameInfo.contentSize) {
        if (cctxPtr->prefs.frameInfo.contentSize != cctxPtr->totalInSize)
            return err0r(LZ4F_ERROR_frameSize_wrong);
    }

    return dstPtr - dstStart;
}


/*-***************************************************
*   Frame Decompression
*****************************************************/

struct LZ4F_dctx_s {
    LZ4F_frameInfo_t frameInfo;
    U32    version;
    U32    dStage;
    U64    frameRemainingSize;
    size_t maxBlockSize;
    size_t maxBufferSize;
    BYTE*  tmpIn;
    size_t tmpInSize;
    size_t tmpInTarget;
    BYTE*  tmpOutBuffer;
    const BYTE*  dict;
    size_t dictSize;
    BYTE*  tmpOut;
    size_t tmpOutSize;
    size_t tmpOutStart;
    XXH32_state_t xxh;
    BYTE   header[16];
};  /* typedef'd to LZ4F_dctx in lz4frame.h */


/*! LZ4F_createDecompressionContext() :
 *   Create a decompressionContext object, which will track all decompression operations.
 *   Provides a pointer to a fully allocated and initialized LZ4F_decompressionContext object.
 *   Object can later be released using LZ4F_freeDecompressionContext().
 *   @return : if != 0, there was an error during context creation.
 */
LZ4F_errorCode_t LZ4F_createDecompressionContext(LZ4F_dctx** LZ4F_decompressionContextPtr, unsigned versionNumber)
{
    LZ4F_dctx* const dctxPtr = (LZ4F_dctx*)ALLOCATOR(sizeof(LZ4F_dctx));
    if (dctxPtr==NULL) return err0r(LZ4F_ERROR_GENERIC);

    dctxPtr->version = versionNumber;
    *LZ4F_decompressionContextPtr = dctxPtr;
    return LZ4F_OK_NoError;
}

LZ4F_errorCode_t LZ4F_freeDecompressionContext(LZ4F_dctx* dctxPtr)
{
    LZ4F_errorCode_t result = LZ4F_OK_NoError;
    if (dctxPtr != NULL) {   /* can accept NULL input, like free() */
      result = (LZ4F_errorCode_t)dctxPtr->dStage;
      FREEMEM(dctxPtr->tmpIn);
      FREEMEM(dctxPtr->tmpOutBuffer);
      FREEMEM(dctxPtr);
    }
    return result;
}


/*==---   Streaming Decompression operations   ---==*/

typedef enum {
    dstage_getHeader=0, dstage_storeHeader,
    dstage_init,
    dstage_getCBlockSize, dstage_storeCBlockSize,
    dstage_copyDirect,
    dstage_getCBlock, dstage_storeCBlock,
    dstage_decodeCBlock, dstage_decodeCBlock_intoDst,
    dstage_decodeCBlock_intoTmp, dstage_flushOut,
    dstage_getSuffix, dstage_storeSuffix,
    dstage_getSFrameSize, dstage_storeSFrameSize,
    dstage_skipSkippable
} dStage_t;

void LZ4F_resetDecompressionContext(LZ4F_dctx* dctx)
{
    dctx->dStage = dstage_getHeader;
}


/*! LZ4F_headerSize() :
 *   @return : size of frame header
 *             or an error code, which can be tested using LZ4F_isError()
 */
static size_t LZ4F_headerSize(const void* src, size_t srcSize)
{
    /* minimal srcSize to determine header size */
    if (srcSize < 5) return err0r(LZ4F_ERROR_frameHeader_incomplete);

    /* special case : skippable frames */
    if ((LZ4F_readLE32(src) & 0xFFFFFFF0U) == LZ4F_MAGIC_SKIPPABLE_START) return 8;

    /* control magic number */
    if (LZ4F_readLE32(src) != LZ4F_MAGICNUMBER)
        return err0r(LZ4F_ERROR_frameType_unknown);

    /* Frame Header Size */
    {   BYTE const FLG = ((const BYTE*)src)[4];
        U32 const contentSizeFlag = (FLG>>3) & _1BIT;
        U32 const dictIDFlag = FLG & _1BIT;
        return minFHSize + (contentSizeFlag*8) + (dictIDFlag*4);
    }
}


/*! LZ4F_decodeHeader() :
 *  input   : `src` points at the **beginning of the frame**
 *  output  : set internal values of dctx, such as
 *            dctxPtr->frameInfo and dctxPtr->dStage.
 *            Also allocates internal buffers.
 *  @return : nb Bytes read from src (necessarily <= srcSize)
 *            or an error code (testable with LZ4F_isError())
 */
static size_t LZ4F_decodeHeader(LZ4F_dctx* dctxPtr, const void* src, size_t srcSize)
{
    unsigned blockMode, contentSizeFlag, contentChecksumFlag, dictIDFlag, blockSizeID;
    size_t frameHeaderSize;
    const BYTE* srcPtr = (const BYTE*)src;

    /* need to decode header to get frameInfo */
    if (srcSize < minFHSize) return err0r(LZ4F_ERROR_frameHeader_incomplete);   /* minimal frame header size */
    memset(&(dctxPtr->frameInfo), 0, sizeof(dctxPtr->frameInfo));

    /* special case : skippable frames */
    if ((LZ4F_readLE32(srcPtr) & 0xFFFFFFF0U) == LZ4F_MAGIC_SKIPPABLE_START) {
        dctxPtr->frameInfo.frameType = LZ4F_skippableFrame;
        if (src == (void*)(dctxPtr->header)) {
            dctxPtr->tmpInSize = srcSize;
            dctxPtr->tmpInTarget = 8;
            dctxPtr->dStage = dstage_storeSFrameSize;
            return srcSize;
        } else {
            dctxPtr->dStage = dstage_getSFrameSize;
            return 4;
        }
    }

    /* control magic number */
    if (LZ4F_readLE32(srcPtr) != LZ4F_MAGICNUMBER)
        return err0r(LZ4F_ERROR_frameType_unknown);
    dctxPtr->frameInfo.frameType = LZ4F_frame;

    /* Flags */
    {   U32 const FLG = srcPtr[4];
        U32 const version = (FLG>>6) & _2BITS;
        U32 const blockChecksumFlag = (FLG>>4) & _1BIT;
        blockMode = (FLG>>5) & _1BIT;
        contentSizeFlag = (FLG>>3) & _1BIT;
        contentChecksumFlag = (FLG>>2) & _1BIT;
        dictIDFlag = FLG & _1BIT;
        /* validate */
        if (((FLG>>1)&_1BIT) != 0) return err0r(LZ4F_ERROR_reservedFlag_set); /* Reserved bits */
        if (version != 1) return err0r(LZ4F_ERROR_headerVersion_wrong);        /* Version Number, only supported value */
        if (blockChecksumFlag != 0) return err0r(LZ4F_ERROR_blockChecksum_unsupported); /* Not supported for the time being */
    }

    /* Frame Header Size */
    frameHeaderSize = minFHSize + (contentSizeFlag*8) + (dictIDFlag*4);

    if (srcSize < frameHeaderSize) {
        /* not enough input to fully decode frame header */
        if (srcPtr != dctxPtr->header)
            memcpy(dctxPtr->header, srcPtr, srcSize);
        dctxPtr->tmpInSize = srcSize;
        dctxPtr->tmpInTarget = frameHeaderSize;
        dctxPtr->dStage = dstage_storeHeader;
        return srcSize;
    }

    {   U32 const BD = srcPtr[5];
        blockSizeID = (BD>>4) & _3BITS;
        /* validate */
        if (((BD>>7)&_1BIT) != 0) return err0r(LZ4F_ERROR_reservedFlag_set);   /* Reserved bit */
        if (blockSizeID < 4) return err0r(LZ4F_ERROR_maxBlockSize_invalid);    /* 4-7 only supported values for the time being */
        if (((BD>>0)&_4BITS) != 0) return err0r(LZ4F_ERROR_reservedFlag_set);  /* Reserved bits */
    }

    /* check header */
    {   BYTE const HC = LZ4F_headerChecksum(srcPtr+4, frameHeaderSize-5);
        if (HC != srcPtr[frameHeaderSize-1])
            return err0r(LZ4F_ERROR_headerChecksum_invalid);
    }

    /* save */
    dctxPtr->frameInfo.blockMode = (LZ4F_blockMode_t)blockMode;
    dctxPtr->frameInfo.contentChecksumFlag = (LZ4F_contentChecksum_t)contentChecksumFlag;
    dctxPtr->frameInfo.blockSizeID = (LZ4F_blockSizeID_t)blockSizeID;
    dctxPtr->maxBlockSize = LZ4F_getBlockSize(blockSizeID);
    if (contentSizeFlag)
        dctxPtr->frameRemainingSize =
            dctxPtr->frameInfo.contentSize = LZ4F_readLE64(srcPtr+6);
    if (dictIDFlag)
        dctxPtr->frameInfo.dictID = LZ4F_readLE32(srcPtr + frameHeaderSize - 5);

    dctxPtr->dStage = dstage_init;

    return frameHeaderSize;
}


/*! LZ4F_getFrameInfo() :
 * This function extracts frame parameters (max blockSize, frame checksum, etc.).
 * Usage is optional. Objective is to provide relevant information for allocation purposes.
 * This function works in 2 situations :
 *   - At the beginning of a new frame, in which case it will decode this information from `srcBuffer`, and start the decoding process.
 *     Amount of input data provided must be large enough to successfully decode the frame header.
 *     A header size is variable, but is guaranteed to be <= LZ4F_HEADER_SIZE_MAX bytes. It's possible to provide more input data than this minimum.
 *   - After decoding has been started. In which case, no input is read, frame parameters are extracted from dctx.
 * The number of bytes consumed from srcBuffer will be updated within *srcSizePtr (necessarily <= original value).
 * Decompression must resume from (srcBuffer + *srcSizePtr).
 * @return : an hint about how many srcSize bytes LZ4F_decompress() expects for next call,
 *           or an error code which can be tested using LZ4F_isError()
 * note 1 : in case of error, dctx is not modified. Decoding operations can resume from where they stopped.
 * note 2 : frame parameters are *copied into* an already allocated LZ4F_frameInfo_t structure.
 */
LZ4F_errorCode_t LZ4F_getFrameInfo(LZ4F_dctx* dctxPtr, LZ4F_frameInfo_t* frameInfoPtr,
                                   const void* srcBuffer, size_t* srcSizePtr)
{
    if (dctxPtr->dStage > dstage_storeHeader) {  /* assumption :  dstage_* header enum at beginning of range */
        /* frameInfo already decoded */
        size_t o=0, i=0;
        *srcSizePtr = 0;
        *frameInfoPtr = dctxPtr->frameInfo;
        /* returns : recommended nb of bytes for LZ4F_decompress() */
        return LZ4F_decompress(dctxPtr, NULL, &o, NULL, &i, NULL);
    } else {
        if (dctxPtr->dStage == dstage_storeHeader) {
            /* frame decoding already started, in the middle of header => automatic fail */
            *srcSizePtr = 0;
            return err0r(LZ4F_ERROR_frameDecoding_alreadyStarted);
        } else {
            size_t decodeResult;
            size_t const hSize = LZ4F_headerSize(srcBuffer, *srcSizePtr);
            if (LZ4F_isError(hSize)) { *srcSizePtr=0; return hSize; }
            if (*srcSizePtr < hSize) {
                *srcSizePtr=0;
                return err0r(LZ4F_ERROR_frameHeader_incomplete);
            }

            decodeResult = LZ4F_decodeHeader(dctxPtr, srcBuffer, hSize);
            if (LZ4F_isError(decodeResult)) {
                *srcSizePtr = 0;
            } else {
                *srcSizePtr = decodeResult;
                decodeResult = BHSize;   /* block header size */
            }
            *frameInfoPtr = dctxPtr->frameInfo;
            return decodeResult;
    }   }
}


/* trivial redirector, for common prototype */
static int LZ4F_decompress_safe (const char* src,
                                 char* dst,
                                 int compressedSize,
                                 int dstCapacity,
                                 const char* dictStart,
                                 int dictSize)
{
    (void)dictStart; (void)dictSize;
    return LZ4_decompress_safe (src, dst, compressedSize, dstCapacity);
}


static void LZ4F_updateDict(LZ4F_dctx* dctxPtr, const BYTE* dstPtr, size_t dstSize, const BYTE* dstPtr0, unsigned withinTmp)
{
    if (dctxPtr->dictSize==0)
        dctxPtr->dict = (const BYTE*)dstPtr;   /* priority to dictionary continuity */

    if (dctxPtr->dict + dctxPtr->dictSize == dstPtr) {  /* dictionary continuity */
        dctxPtr->dictSize += dstSize;
        return;
    }

    if (dstPtr - dstPtr0 + dstSize >= 64 KB) {  /* dstBuffer large enough to become dictionary */
        dctxPtr->dict = (const BYTE*)dstPtr0;
        dctxPtr->dictSize = dstPtr - dstPtr0 + dstSize;
        return;
    }

    if ((withinTmp) && (dctxPtr->dict == dctxPtr->tmpOutBuffer)) {
        /* assumption : dctxPtr->dict + dctxPtr->dictSize == dctxPtr->tmpOut + dctxPtr->tmpOutStart */
        dctxPtr->dictSize += dstSize;
        return;
    }

    if (withinTmp) { /* copy relevant dict portion in front of tmpOut within tmpOutBuffer */
        size_t const preserveSize = dctxPtr->tmpOut - dctxPtr->tmpOutBuffer;
        size_t copySize = 64 KB - dctxPtr->tmpOutSize;
        const BYTE* const oldDictEnd = dctxPtr->dict + dctxPtr->dictSize - dctxPtr->tmpOutStart;
        if (dctxPtr->tmpOutSize > 64 KB) copySize = 0;
        if (copySize > preserveSize) copySize = preserveSize;

        memcpy(dctxPtr->tmpOutBuffer + preserveSize - copySize, oldDictEnd - copySize, copySize);

        dctxPtr->dict = dctxPtr->tmpOutBuffer;
        dctxPtr->dictSize = preserveSize + dctxPtr->tmpOutStart + dstSize;
        return;
    }

    if (dctxPtr->dict == dctxPtr->tmpOutBuffer) {    /* copy dst into tmp to complete dict */
        if (dctxPtr->dictSize + dstSize > dctxPtr->maxBufferSize) {  /* tmp buffer not large enough */
            size_t const preserveSize = 64 KB - dstSize;   /* note : dstSize < 64 KB */
            memcpy(dctxPtr->tmpOutBuffer, dctxPtr->dict + dctxPtr->dictSize - preserveSize, preserveSize);
            dctxPtr->dictSize = preserveSize;
        }
        memcpy(dctxPtr->tmpOutBuffer + dctxPtr->dictSize, dstPtr, dstSize);
        dctxPtr->dictSize += dstSize;
        return;
    }

    /* join dict & dest into tmp */
    {   size_t preserveSize = 64 KB - dstSize;   /* note : dstSize < 64 KB */
        if (preserveSize > dctxPtr->dictSize) preserveSize = dctxPtr->dictSize;
        memcpy(dctxPtr->tmpOutBuffer, dctxPtr->dict + dctxPtr->dictSize - preserveSize, preserveSize);
        memcpy(dctxPtr->tmpOutBuffer + preserveSize, dstPtr, dstSize);
        dctxPtr->dict = dctxPtr->tmpOutBuffer;
        dctxPtr->dictSize = preserveSize + dstSize;
    }
}



/*! LZ4F_decompress() :
 * Call this function repetitively to regenerate data compressed within srcBuffer.
 * The function will attempt to decode up to *srcSizePtr bytes from srcBuffer, into dstBuffer of capacity *dstSizePtr.
 *
 * The number of bytes regenerated into dstBuffer will be provided within *dstSizePtr (necessarily <= original value).
 *
 * The number of bytes effectively read from srcBuffer will be provided within *srcSizePtr (necessarily <= original value).
 * If the number of bytes read is < number of bytes provided, then the decompression operation is not complete.
 * Remaining data will have to be presented again in a subsequent invocation.
 *
 * The function result is an hint of the better srcSize to use for next call to LZ4F_decompress.
 * Basically, it's the size of the current (or remaining) compressed block + header of next block.
 * Respecting the hint provides some boost to performance, since it allows less buffer shuffling.
 * Note that this is just a hint, it's always possible to any srcSize value.
 * When a frame is fully decoded, @return will be 0.
 * If decompression failed, @return is an error code which can be tested using LZ4F_isError().
 */
size_t LZ4F_decompress(LZ4F_dctx* dctxPtr,
                       void* dstBuffer, size_t* dstSizePtr,
                       const void* srcBuffer, size_t* srcSizePtr,
                       const LZ4F_decompressOptions_t* decompressOptionsPtr)
{
    LZ4F_decompressOptions_t optionsNull;
    const BYTE* const srcStart = (const BYTE*)srcBuffer;
    const BYTE* const srcEnd = srcStart + *srcSizePtr;
    const BYTE* srcPtr = srcStart;
    BYTE* const dstStart = (BYTE*)dstBuffer;
    BYTE* const dstEnd = dstStart + *dstSizePtr;
    BYTE* dstPtr = dstStart;
    const BYTE* selectedIn = NULL;
    unsigned doAnotherStage = 1;
    size_t nextSrcSizeHint = 1;


    memset(&optionsNull, 0, sizeof(optionsNull));
    if (decompressOptionsPtr==NULL) decompressOptionsPtr = &optionsNull;
    *srcSizePtr = 0;
    *dstSizePtr = 0;

    /* behaves like a state machine */

    while (doAnotherStage) {

        switch(dctxPtr->dStage)
        {

        case dstage_getHeader:
            if ((size_t)(srcEnd-srcPtr) >= maxFHSize) {  /* enough to decode - shortcut */
                LZ4F_errorCode_t const hSize = LZ4F_decodeHeader(dctxPtr, srcPtr, srcEnd-srcPtr);  /* will change dStage appropriately */
                if (LZ4F_isError(hSize)) return hSize;
                srcPtr += hSize;
                break;
            }
            dctxPtr->tmpInSize = 0;
            if (srcEnd-srcPtr == 0) return minFHSize;   /* 0-size input */
            dctxPtr->tmpInTarget = minFHSize;   /* minimum to attempt decode */
            dctxPtr->dStage = dstage_storeHeader;
            /* fall-through */

        case dstage_storeHeader:
            {   size_t sizeToCopy = dctxPtr->tmpInTarget - dctxPtr->tmpInSize;
                if (sizeToCopy > (size_t)(srcEnd - srcPtr)) sizeToCopy =  srcEnd - srcPtr;
                memcpy(dctxPtr->header + dctxPtr->tmpInSize, srcPtr, sizeToCopy);
                dctxPtr->tmpInSize += sizeToCopy;
                srcPtr += sizeToCopy;
                if (dctxPtr->tmpInSize < dctxPtr->tmpInTarget) {
                    nextSrcSizeHint = (dctxPtr->tmpInTarget - dctxPtr->tmpInSize) + BHSize;   /* rest of header + nextBlockHeader */
                    doAnotherStage = 0;   /* not enough src data, ask for some more */
                    break;
                }
                {   LZ4F_errorCode_t const hSize = LZ4F_decodeHeader(dctxPtr, dctxPtr->header, dctxPtr->tmpInTarget);  /* will change dStage appropriately */
                    if (LZ4F_isError(hSize)) return hSize;
                }
                break;
            }

        case dstage_init:
            if (dctxPtr->frameInfo.contentChecksumFlag) XXH32_reset(&(dctxPtr->xxh), 0);
            /* internal buffers allocation */
            {   size_t const bufferNeeded = dctxPtr->maxBlockSize + ((dctxPtr->frameInfo.blockMode==LZ4F_blockLinked) * 128 KB);
                if (bufferNeeded > dctxPtr->maxBufferSize) {   /* tmp buffers too small */
                    dctxPtr->maxBufferSize = 0;   /* ensure allocation will be re-attempted on next entry*/
                    FREEMEM(dctxPtr->tmpIn);
                    dctxPtr->tmpIn = (BYTE*)ALLOCATOR(dctxPtr->maxBlockSize);
                    if (dctxPtr->tmpIn == NULL) return err0r(LZ4F_ERROR_allocation_failed);
                    FREEMEM(dctxPtr->tmpOutBuffer);
                    dctxPtr->tmpOutBuffer= (BYTE*)ALLOCATOR(bufferNeeded);
                    if (dctxPtr->tmpOutBuffer== NULL) return err0r(LZ4F_ERROR_allocation_failed);
                    dctxPtr->maxBufferSize = bufferNeeded;
            }   }
            dctxPtr->tmpInSize = 0;
            dctxPtr->tmpInTarget = 0;
            dctxPtr->dict = dctxPtr->tmpOutBuffer;
            dctxPtr->dictSize = 0;
            dctxPtr->tmpOut = dctxPtr->tmpOutBuffer;
            dctxPtr->tmpOutStart = 0;
            dctxPtr->tmpOutSize = 0;

            dctxPtr->dStage = dstage_getCBlockSize;
            /* fall-through */

        case dstage_getCBlockSize:
            if ((size_t)(srcEnd - srcPtr) >= BHSize) {
                selectedIn = srcPtr;
                srcPtr += BHSize;
            } else {
                /* not enough input to read cBlockSize field */
                dctxPtr->tmpInSize = 0;
                dctxPtr->dStage = dstage_storeCBlockSize;
            }

            if (dctxPtr->dStage == dstage_storeCBlockSize)   /* can be skipped */
        case dstage_storeCBlockSize:
            {   size_t sizeToCopy = BHSize - dctxPtr->tmpInSize;
                if (sizeToCopy > (size_t)(srcEnd - srcPtr)) sizeToCopy = srcEnd - srcPtr;
                memcpy(dctxPtr->tmpIn + dctxPtr->tmpInSize, srcPtr, sizeToCopy);
                srcPtr += sizeToCopy;
                dctxPtr->tmpInSize += sizeToCopy;
                if (dctxPtr->tmpInSize < BHSize) {   /* not enough input for cBlockSize */
                    nextSrcSizeHint = BHSize - dctxPtr->tmpInSize;
                    doAnotherStage  = 0;
                    break;
                }
                selectedIn = dctxPtr->tmpIn;
            }

        /* case dstage_decodeCBlockSize: */   /* no more direct access, to remove scan-build warning */
            {   size_t const nextCBlockSize = LZ4F_readLE32(selectedIn) & 0x7FFFFFFFU;
                if (nextCBlockSize==0) {  /* frameEnd signal, no more CBlock */
                    dctxPtr->dStage = dstage_getSuffix;
                    break;
                }
                if (nextCBlockSize > dctxPtr->maxBlockSize)
                    return err0r(LZ4F_ERROR_maxBlockSize_invalid);
                dctxPtr->tmpInTarget = nextCBlockSize;
                if (LZ4F_readLE32(selectedIn) & LZ4F_BLOCKUNCOMPRESSED_FLAG) {
                    dctxPtr->dStage = dstage_copyDirect;
                    break;
                }
                dctxPtr->dStage = dstage_getCBlock;
                if (dstPtr==dstEnd) {
                    nextSrcSizeHint = nextCBlockSize + BHSize;
                    doAnotherStage = 0;
                }
                break;
            }

        case dstage_copyDirect:   /* uncompressed block */
            {   size_t sizeToCopy = dctxPtr->tmpInTarget;
                if ((size_t)(srcEnd-srcPtr) < sizeToCopy) sizeToCopy = srcEnd - srcPtr;
                if ((size_t)(dstEnd-dstPtr) < sizeToCopy) sizeToCopy = dstEnd - dstPtr;
                memcpy(dstPtr, srcPtr, sizeToCopy);
                if (dctxPtr->frameInfo.contentChecksumFlag)
                    XXH32_update(&(dctxPtr->xxh), srcPtr, sizeToCopy);
                if (dctxPtr->frameInfo.contentSize)
                    dctxPtr->frameRemainingSize -= sizeToCopy;

                /* dictionary management */
                if (dctxPtr->frameInfo.blockMode==LZ4F_blockLinked)
                    LZ4F_updateDict(dctxPtr, dstPtr, sizeToCopy, dstStart, 0);

                srcPtr += sizeToCopy;
                dstPtr += sizeToCopy;
                if (sizeToCopy == dctxPtr->tmpInTarget) {  /* all copied */
                    dctxPtr->dStage = dstage_getCBlockSize;
                    break;
                }
                dctxPtr->tmpInTarget -= sizeToCopy;   /* still need to copy more */
                nextSrcSizeHint = dctxPtr->tmpInTarget + BHSize;
                doAnotherStage = 0;
                break;
            }

        case dstage_getCBlock:   /* entry from dstage_decodeCBlockSize */
            if ((size_t)(srcEnd-srcPtr) < dctxPtr->tmpInTarget) {
                dctxPtr->tmpInSize = 0;
                dctxPtr->dStage = dstage_storeCBlock;
                break;
            }
            selectedIn = srcPtr;
            srcPtr += dctxPtr->tmpInTarget;
            dctxPtr->dStage = dstage_decodeCBlock;
            break;

        case dstage_storeCBlock:
            {   size_t sizeToCopy = dctxPtr->tmpInTarget - dctxPtr->tmpInSize;
                if (sizeToCopy > (size_t)(srcEnd-srcPtr)) sizeToCopy = srcEnd-srcPtr;
                memcpy(dctxPtr->tmpIn + dctxPtr->tmpInSize, srcPtr, sizeToCopy);
                dctxPtr->tmpInSize += sizeToCopy;
                srcPtr += sizeToCopy;
                if (dctxPtr->tmpInSize < dctxPtr->tmpInTarget) { /* need more input */
                    nextSrcSizeHint = (dctxPtr->tmpInTarget - dctxPtr->tmpInSize) + BHSize;
                    doAnotherStage=0;
                    break;
                }
                selectedIn = dctxPtr->tmpIn;
                dctxPtr->dStage = dstage_decodeCBlock;
            }
            /* fall-through */

        case dstage_decodeCBlock:
            if ((size_t)(dstEnd-dstPtr) < dctxPtr->maxBlockSize)   /* not enough place into dst : decode into tmpOut */
                dctxPtr->dStage = dstage_decodeCBlock_intoTmp;
            else
                dctxPtr->dStage = dstage_decodeCBlock_intoDst;
            break;

        case dstage_decodeCBlock_intoDst:
            {   int (*decoder)(const char*, char*, int, int, const char*, int);
                int decodedSize;

                if (dctxPtr->frameInfo.blockMode == LZ4F_blockLinked)
                    decoder = LZ4_decompress_safe_usingDict;
                else
                    decoder = LZ4F_decompress_safe;

                decodedSize = decoder((const char*)selectedIn, (char*)dstPtr,
                        (int)dctxPtr->tmpInTarget, (int)dctxPtr->maxBlockSize,
                        (const char*)dctxPtr->dict, (int)dctxPtr->dictSize);
                if (decodedSize < 0) return err0r(LZ4F_ERROR_GENERIC);   /* decompression failed */
                if (dctxPtr->frameInfo.contentChecksumFlag)
                    XXH32_update(&(dctxPtr->xxh), dstPtr, decodedSize);
                if (dctxPtr->frameInfo.contentSize)
                    dctxPtr->frameRemainingSize -= decodedSize;

                /* dictionary management */
                if (dctxPtr->frameInfo.blockMode==LZ4F_blockLinked)
                    LZ4F_updateDict(dctxPtr, dstPtr, decodedSize, dstStart, 0);

                dstPtr += decodedSize;
                dctxPtr->dStage = dstage_getCBlockSize;
                break;
            }

        case dstage_decodeCBlock_intoTmp:
            /* not enough place into dst : decode into tmpOut */
            {   int (*decoder)(const char*, char*, int, int, const char*, int);
                int decodedSize;

                if (dctxPtr->frameInfo.blockMode == LZ4F_blockLinked)
                    decoder = LZ4_decompress_safe_usingDict;
                else
                    decoder = LZ4F_decompress_safe;

                /* ensure enough place for tmpOut */
                if (dctxPtr->frameInfo.blockMode == LZ4F_blockLinked) {
                    if (dctxPtr->dict == dctxPtr->tmpOutBuffer) {
                        if (dctxPtr->dictSize > 128 KB) {
                            memcpy(dctxPtr->tmpOutBuffer, dctxPtr->dict + dctxPtr->dictSize - 64 KB, 64 KB);
                            dctxPtr->dictSize = 64 KB;
                        }
                        dctxPtr->tmpOut = dctxPtr->tmpOutBuffer + dctxPtr->dictSize;
                    } else {  /* dict not within tmp */
                        size_t reservedDictSpace = dctxPtr->dictSize;
                        if (reservedDictSpace > 64 KB) reservedDictSpace = 64 KB;
                        dctxPtr->tmpOut = dctxPtr->tmpOutBuffer + reservedDictSpace;
                    }
                }

                /* Decode */
                decodedSize = decoder((const char*)selectedIn, (char*)dctxPtr->tmpOut,
                        (int)dctxPtr->tmpInTarget, (int)dctxPtr->maxBlockSize,
                        (const char*)dctxPtr->dict, (int)dctxPtr->dictSize);
                if (decodedSize < 0)
                    return err0r(LZ4F_ERROR_decompressionFailed);   /* decompression failed */
                if (dctxPtr->frameInfo.contentChecksumFlag)
                    XXH32_update(&(dctxPtr->xxh), dctxPtr->tmpOut, decodedSize);
                if (dctxPtr->frameInfo.contentSize)
                    dctxPtr->frameRemainingSize -= decodedSize;
                dctxPtr->tmpOutSize = decodedSize;
                dctxPtr->tmpOutStart = 0;
                dctxPtr->dStage = dstage_flushOut;
                break;
            }

        case dstage_flushOut:  /* flush decoded data from tmpOut to dstBuffer */
            {   size_t sizeToCopy = dctxPtr->tmpOutSize - dctxPtr->tmpOutStart;
                if (sizeToCopy > (size_t)(dstEnd-dstPtr)) sizeToCopy = dstEnd-dstPtr;
                memcpy(dstPtr, dctxPtr->tmpOut + dctxPtr->tmpOutStart, sizeToCopy);

                /* dictionary management */
                if (dctxPtr->frameInfo.blockMode==LZ4F_blockLinked)
                    LZ4F_updateDict(dctxPtr, dstPtr, sizeToCopy, dstStart, 1);

                dctxPtr->tmpOutStart += sizeToCopy;
                dstPtr += sizeToCopy;

                /* end of flush ? */
                if (dctxPtr->tmpOutStart == dctxPtr->tmpOutSize) {
                    dctxPtr->dStage = dstage_getCBlockSize;
                    break;
                }
                nextSrcSizeHint = BHSize;
                doAnotherStage = 0;   /* still some data to flush */
                break;
            }

        case dstage_getSuffix:
            {   size_t const suffixSize = dctxPtr->frameInfo.contentChecksumFlag * 4;
                if (dctxPtr->frameRemainingSize)
                    return err0r(LZ4F_ERROR_frameSize_wrong);   /* incorrect frame size decoded */
                if (suffixSize == 0) {  /* frame completed */
                    nextSrcSizeHint = 0;
                    dctxPtr->dStage = dstage_getHeader;
                    doAnotherStage = 0;
                    break;
                }
                if ((srcEnd - srcPtr) < 4) {  /* not enough size for entire CRC */
                    dctxPtr->tmpInSize = 0;
                    dctxPtr->dStage = dstage_storeSuffix;
                } else {
                    selectedIn = srcPtr;
                    srcPtr += 4;
                }
            }

            if (dctxPtr->dStage == dstage_storeSuffix)   /* can be skipped */
        case dstage_storeSuffix:
            {
                size_t sizeToCopy = 4 - dctxPtr->tmpInSize;
                if (sizeToCopy > (size_t)(srcEnd - srcPtr)) sizeToCopy = srcEnd - srcPtr;
                memcpy(dctxPtr->tmpIn + dctxPtr->tmpInSize, srcPtr, sizeToCopy);
                srcPtr += sizeToCopy;
                dctxPtr->tmpInSize += sizeToCopy;
                if (dctxPtr->tmpInSize < 4) { /* not enough input to read complete suffix */
                    nextSrcSizeHint = 4 - dctxPtr->tmpInSize;
                    doAnotherStage=0;
                    break;
                }
                selectedIn = dctxPtr->tmpIn;
            }

        /* case dstage_checkSuffix: */   /* no direct call, to avoid scan-build warning */
            {   U32 const readCRC = LZ4F_readLE32(selectedIn);
                U32 const resultCRC = XXH32_digest(&(dctxPtr->xxh));
                if (readCRC != resultCRC) return err0r(LZ4F_ERROR_contentChecksum_invalid);
                nextSrcSizeHint = 0;
                dctxPtr->dStage = dstage_getHeader;
                doAnotherStage = 0;
                break;
            }

        case dstage_getSFrameSize:
            if ((srcEnd - srcPtr) >= 4) {
                selectedIn = srcPtr;
                srcPtr += 4;
            } else {
                /* not enough input to read cBlockSize field */
                dctxPtr->tmpInSize = 4;
                dctxPtr->tmpInTarget = 8;
                dctxPtr->dStage = dstage_storeSFrameSize;
            }

            if (dctxPtr->dStage == dstage_storeSFrameSize)
        case dstage_storeSFrameSize:
            {
                size_t sizeToCopy = dctxPtr->tmpInTarget - dctxPtr->tmpInSize;
                if (sizeToCopy > (size_t)(srcEnd - srcPtr)) sizeToCopy = srcEnd - srcPtr;
                memcpy(dctxPtr->header + dctxPtr->tmpInSize, srcPtr, sizeToCopy);
                srcPtr += sizeToCopy;
                dctxPtr->tmpInSize += sizeToCopy;
                if (dctxPtr->tmpInSize < dctxPtr->tmpInTarget) {
                    /* not enough input to get full sBlockSize; wait for more */
                    nextSrcSizeHint = dctxPtr->tmpInTarget - dctxPtr->tmpInSize;
                    doAnotherStage = 0;
                    break;
                }
                selectedIn = dctxPtr->header + 4;
            }

        /* case dstage_decodeSFrameSize: */   /* no direct access */
            {   size_t const SFrameSize = LZ4F_readLE32(selectedIn);
                dctxPtr->frameInfo.contentSize = SFrameSize;
                dctxPtr->tmpInTarget = SFrameSize;
                dctxPtr->dStage = dstage_skipSkippable;
                break;
            }

        case dstage_skipSkippable:
            {   size_t skipSize = dctxPtr->tmpInTarget;
                if (skipSize > (size_t)(srcEnd-srcPtr)) skipSize = srcEnd-srcPtr;
                srcPtr += skipSize;
                dctxPtr->tmpInTarget -= skipSize;
                doAnotherStage = 0;
                nextSrcSizeHint = dctxPtr->tmpInTarget;
                if (nextSrcSizeHint) break;
                dctxPtr->dStage = dstage_getHeader;
                break;
            }
        }
    }

    /* preserve dictionary within tmp if necessary */
    if ( (dctxPtr->frameInfo.blockMode==LZ4F_blockLinked)
        &&(dctxPtr->dict != dctxPtr->tmpOutBuffer)
        &&(!decompressOptionsPtr->stableDst)
        &&((unsigned)(dctxPtr->dStage-1) < (unsigned)(dstage_getSuffix-1))
        )
    {
        if (dctxPtr->dStage == dstage_flushOut) {
            size_t preserveSize = dctxPtr->tmpOut - dctxPtr->tmpOutBuffer;
            size_t copySize = 64 KB - dctxPtr->tmpOutSize;
            const BYTE* oldDictEnd = dctxPtr->dict + dctxPtr->dictSize - dctxPtr->tmpOutStart;
            if (dctxPtr->tmpOutSize > 64 KB) copySize = 0;
            if (copySize > preserveSize) copySize = preserveSize;

            memcpy(dctxPtr->tmpOutBuffer + preserveSize - copySize, oldDictEnd - copySize, copySize);

            dctxPtr->dict = dctxPtr->tmpOutBuffer;
            dctxPtr->dictSize = preserveSize + dctxPtr->tmpOutStart;
        } else {
            size_t newDictSize = dctxPtr->dictSize;
            const BYTE* oldDictEnd = dctxPtr->dict + dctxPtr->dictSize;
            if ((newDictSize) > 64 KB) newDictSize = 64 KB;

            memcpy(dctxPtr->tmpOutBuffer, oldDictEnd - newDictSize, newDictSize);

            dctxPtr->dict = dctxPtr->tmpOutBuffer;
            dctxPtr->dictSize = newDictSize;
            dctxPtr->tmpOut = dctxPtr->tmpOutBuffer + newDictSize;
        }
    }

    *srcSizePtr = (srcPtr - srcStart);
    *dstSizePtr = (dstPtr - dstStart);
    return nextSrcSizeHint;
}
