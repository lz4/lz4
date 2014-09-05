/*
   LZ4 auto-framing library
   Copyright (C) 2011-2014, Yann Collet.
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
   - LZ4 source repository : http://code.google.com/p/lz4/
   - LZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
*/

/* LZ4F is a stand-alone API to create LZ4-compressed frames
 * fully conformant to specification v1.4.1.
 * All related operations, including memory management, are handled by the library.
 * You don't need lz4.h when using lz4frame.h.
 * */


/**************************************
 Compiler Options
**************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  pragma warning(disable : 4127)        /* disable: C4127: conditional expression is constant */
#endif

#define GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)
#ifdef __GNUC__
#  pragma GCC diagnostic ignored "-Wmissing-braces"   /* GCC bug 53119 : doesn't accept { 0 } as initializer (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53119) */
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"   /* GCC bug 53119 : doesn't accept { 0 } as initializer (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53119) */
#endif


/**************************************
   Memory routines
**************************************/
#include <stdlib.h>   /* malloc, calloc, free */
#define ALLOCATOR(s)   calloc(1,s)
#define FREEMEM        free
#include <string.h>   /* memset, memcpy */
#define MEM_INIT       memset


/**************************************
   Includes
**************************************/
#include "lz4frame.h"
#include "lz4.h"
#include "lz4hc.h"
#include "xxhash.h"


/**************************************
   Basic Types
**************************************/
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)   /* C99 */
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


/**************************************
   Constants
**************************************/
#define KB *(1<<10)
#define MB *(1<<20)
#define GB *(1<<30)

#define _1BIT  0x01
#define _2BITS 0x03
#define _3BITS 0x07
#define _4BITS 0x0F
#define _8BITS 0xFF

#define LZ4F_MAGICNUMBER 0x184D2204U
#define LZ4F_BLOCKUNCOMPRESSED_FLAG 0x80000000U
#define LZ4F_MAXHEADERFRAME_SIZE 19
#define LZ4F_BLOCKSIZEID_DEFAULT 4


/**************************************
   Structures and local types
**************************************/
typedef struct {
	LZ4F_preferences_t prefs;
	unsigned version;
	unsigned cStage;
	size_t maxBlockSize;
	XXH32_stateSpace_t xxh;
	BYTE* tmpIn;
	size_t tmpInSize;
} LZ4F_cctx_internal_t;

typedef struct {
    LZ4F_frameInfo_t frameInfo;
	unsigned version;
	unsigned dStage;
	size_t maxBlockSize;
	XXH32_stateSpace_t xxh;
	size_t sizeToDecode;
	const BYTE* srcExpect;
	BYTE*  tmpIn;
	size_t tmpInSize;
	size_t tmpInTarget;
	BYTE*  tmpOut;
	size_t tmpOutSize;
	size_t tmpOutStart;
	BYTE   header[7];
} LZ4F_dctx_internal_t;


/**************************************
   Macros
**************************************/


/**************************************
   Private functions
**************************************/
static size_t LZ4F_getBlockSize(unsigned blockSizeID)
{
	static const size_t blockSizes[4] = { 64 KB, 256 KB, 1 MB, 4 MB };

	if (blockSizeID == 0) blockSizeID = LZ4F_BLOCKSIZEID_DEFAULT;
	blockSizeID -= 4;
	if (blockSizeID > 3) return -ERROR_maxBlockSize_invalid;
	return blockSizes[blockSizeID];
}


/* unoptimized version; solves endianess & alignment issues */
static void LZ4F_writeLE32 (BYTE* dstPtr, U32 value32)
{
	dstPtr[0] = (BYTE)value32;
	dstPtr[1] = (BYTE)(value32 >> 8);
	dstPtr[2] = (BYTE)(value32 >> 16);
	dstPtr[3] = (BYTE)(value32 >> 24);
}

static U32 LZ4F_readLE32 (const BYTE* srcPtr)
{
    U32 value32 = srcPtr[0];
    value32 += (srcPtr[1]<<8);
    value32 += (srcPtr[2]<<16);
    value32 += (srcPtr[3]<<24);
    return value32;
}


static BYTE LZ4F_headerChecksum (const BYTE* header, size_t length)
{
	U32 xxh = XXH32(header, length, 0);
	return (BYTE)(xxh >> 8);
}



/**************************************
   Error management
**************************************/
int LZ4F_isError(LZ4F_errorCode_t code)
{
	return (code > (LZ4F_errorCode_t)(-ERROR_maxCode));
}


/**************************************
   Simple compression functions
**************************************/
size_t LZ4F_compressFrameBound(size_t srcSize, const LZ4F_frameInfo_t* frameInfoPtr)
{
	const LZ4F_frameInfo_t frameInfoNull = { 0 };
	size_t headerSize;
	size_t blockInfoSize;
	size_t blockSize;
	unsigned nbBlocks;
	size_t frameSuffixSize;
	size_t totalBound;

	if (frameInfoPtr==NULL) frameInfoPtr = &frameInfoNull;   /* all parameters set to default */

	headerSize = 7;      /* basic header size (no option) including magic number */
	blockInfoSize = 4;   /* basic blockInfo size (no option) for one block */

	blockSize = LZ4F_getBlockSize(frameInfoPtr->blockSizeID);
	nbBlocks = (srcSize + (blockSize-1)) / blockSize;
	blockInfoSize *= nbBlocks;   /* total block info size */

	frameSuffixSize = 4;  /* basic frameSuffixSize (no option) */
	if (frameInfoPtr->contentChecksumFlag == contentChecksumEnabled) frameSuffixSize += 4;

	totalBound = headerSize + srcSize + blockInfoSize + frameSuffixSize;
	if (totalBound < srcSize) return -ERROR_srcSize_tooLarge;   /* overflow error */

	return totalBound;
}


/* LZ4F_compressFrame()
 * Compress an entire srcBuffer into a valid LZ4 frame, as defined by specification v1.4.1, in a single step.
 * The most important rule is that dstBuffer MUST be large enough (dstMaxSize) to ensure compression completion even in worst case.
 * You can get the minimum value of dstMaxSize by using LZ4F_compressFrameBound()
 * If this condition is not respected, LZ4F_compressFrame() will fail (result is an errorCode)
 * The LZ4F_preferences_t structure is optional : you can provide NULL as argument. All preferences will be set to default.
 * The result of the function is the number of bytes written into dstBuffer.
 * The function outputs an error code if it fails (can be tested using LZ4F_isError())
 */
size_t LZ4F_compressFrame(void* dstBuffer, size_t dstMaxSize, const void* srcBuffer, size_t srcSize, const LZ4F_preferences_t* preferencesPtr)
{
	const LZ4F_frameInfo_t frameInfoNull = { 0 };
	const LZ4F_frameInfo_t* const frameInfoPtr = (preferencesPtr==NULL) ? &frameInfoNull : &(preferencesPtr->frameInfo);
	LZ4F_compressionContext_t cctx = NULL;
	LZ4F_errorCode_t errorCode;
	BYTE* const dstStart = (BYTE*) dstBuffer;
	BYTE* dstPtr = dstStart;
	size_t blockSize = LZ4F_getBlockSize(frameInfoPtr->blockSizeID);
	unsigned nbBlocks = (srcSize + (blockSize-1)) / blockSize;
	unsigned blockNb;
	const BYTE* srcPtr = (const BYTE*) srcBuffer;
	const size_t dstBlockSize = LZ4F_compressBound(blockSize, frameInfoPtr);


	if (dstMaxSize < LZ4F_compressFrameBound(srcSize, frameInfoPtr))
		return -ERROR_dstMaxSize_tooSmall;

	errorCode = LZ4F_createCompressionContext(&cctx, LZ4F_VERSION, preferencesPtr);
	if (LZ4F_isError(errorCode)) return errorCode;

	errorCode = LZ4F_compressBegin(cctx, dstBuffer, dstMaxSize);  /* write header */
	if (LZ4F_isError(errorCode)) return errorCode;
	dstPtr += errorCode;   /* header size */

	for (blockNb=1; blockNb<nbBlocks; blockNb++)
	{
		errorCode = LZ4F_compress(cctx, dstPtr, dstBlockSize, srcPtr, blockSize, NULL);
		if (LZ4F_isError(errorCode)) return errorCode;
		srcPtr += blockSize;
		dstPtr += errorCode;
	}

	/* last block */
	{
        size_t lastBlockSize = srcSize % blockSize;
        if (lastBlockSize==0) lastBlockSize = blockSize;
        errorCode = LZ4F_compress(cctx, dstPtr, dstBlockSize, srcPtr, lastBlockSize, NULL);
        if (LZ4F_isError(errorCode)) return errorCode;
        dstPtr += errorCode;
	}

	errorCode = LZ4F_compressEnd(cctx, dstPtr, dstBlockSize, NULL);   /* flush last block, and generate suffix */
	if (LZ4F_isError(errorCode)) return errorCode;
	dstPtr += errorCode;

	errorCode = LZ4F_freeCompressionContext(cctx);
	if (LZ4F_isError(errorCode)) return errorCode;

	return (dstPtr - dstStart);
}


/***********************************
 * Advanced compression functions
 * *********************************/

/* LZ4F_createCompressionContext() :
 * The first thing to do is to create a compressionContext object, which will be used in all compression operations.
 * This is achieved using LZ4F_createCompressionContext(), which takes as argument a version and an LZ4F_preferences_t structure.
 * The version provided MUST be LZ4F_VERSION. It is intended to track potential version differences between different binaries.
 * The LZ4F_preferences_t structure is optional : you can provide NULL as argument, all preferences will then be set to default.
 * The function will provide a pointer to a fully allocated LZ4F_compressionContext_t object.
 * If the result LZ4F_errorCode_t is not zero, there was an error during context creation.
 * Object can release its memory using LZ4F_freeCompressionContext();
 */
LZ4F_errorCode_t LZ4F_createCompressionContext(LZ4F_compressionContext_t* LZ4F_compressionContextPtr, int version, const LZ4F_preferences_t* preferencesPtr)
{
	const LZ4F_preferences_t prefNull = { 0 };
	LZ4F_cctx_internal_t* cctxPtr;

	if (preferencesPtr == NULL) preferencesPtr = &prefNull;

	cctxPtr = ALLOCATOR(sizeof(LZ4F_cctx_internal_t));
	if (cctxPtr==NULL) return -ERROR_allocation_failed;

	cctxPtr->prefs = *preferencesPtr;   /* equivalent to memcpy() */
	cctxPtr->version = version;
	cctxPtr->cStage = 0;   /* Next stage : write header */
	if (cctxPtr->prefs.frameInfo.blockSizeID == 0) cctxPtr->prefs.frameInfo.blockSizeID = LZ4F_BLOCKSIZEID_DEFAULT;
	cctxPtr->maxBlockSize = LZ4F_getBlockSize(cctxPtr->prefs.frameInfo.blockSizeID);
	cctxPtr->tmpIn = ALLOCATOR(cctxPtr->maxBlockSize);
	if (cctxPtr->tmpIn == NULL) return -ERROR_allocation_failed;
	cctxPtr->tmpInSize = 0;
	XXH32_resetState(&(cctxPtr->xxh), 0);

	*LZ4F_compressionContextPtr = (LZ4F_compressionContext_t)cctxPtr;

	return OK_NoError;
}


LZ4F_errorCode_t LZ4F_freeCompressionContext(LZ4F_compressionContext_t LZ4F_compressionContext)
{
	LZ4F_cctx_internal_t* cctxPtr = (LZ4F_cctx_internal_t*)LZ4F_compressionContext;

	FREEMEM(cctxPtr->tmpIn);
	FREEMEM(LZ4F_compressionContext);

	return OK_NoError;
}


/* LZ4F_compressBegin() :
 * will write the frame header into dstBuffer.
 * dstBuffer must be large enough to accommodate a header (dstMaxSize). Maximum header size is LZ4F_MAXHEADERFRAME_SIZE(19) bytes.
 * The result of the function is the number of bytes written into dstBuffer for the header
 * or an error code (can be tested using LZ4F_isError())
 */
size_t LZ4F_compressBegin(LZ4F_compressionContext_t compressionContext, void* dstBuffer, size_t dstMaxSize)
{
	LZ4F_cctx_internal_t* cctxPtr = (LZ4F_cctx_internal_t*)compressionContext;
	BYTE* const dstStart = (BYTE*)dstBuffer;
	BYTE* dstPtr = dstStart;
	BYTE* headerStart;

	if (dstMaxSize < LZ4F_MAXHEADERFRAME_SIZE) return -ERROR_dstMaxSize_tooSmall;
	if (cctxPtr->cStage != 0) return -ERROR_GENERIC;

	/* Magic Number */
	LZ4F_writeLE32(dstPtr, LZ4F_MAGICNUMBER);
	dstPtr += 4;
	headerStart = dstPtr;

	/* FLG Byte */
    *dstPtr  = (1 & _2BITS) << 6;    /* Version('01') */
    *dstPtr |= (1 & _1BIT ) << 5;    /* Blocks independents */
    *dstPtr |= (char)((cctxPtr->prefs.frameInfo.contentChecksumFlag & _1BIT ) << 2);   /* Stream checksum */
    dstPtr++;
	/* BD Byte */
    *dstPtr  = (char)((cctxPtr->prefs.frameInfo.blockSizeID & _3BITS) << 4);
    dstPtr++;
	/* CRC Byte */
	*dstPtr = LZ4F_headerChecksum(headerStart, 2);
    dstPtr++;

	cctxPtr->cStage = 1;   /* header written */

	return (dstPtr - dstStart);
}


/* LZ4F_compressBound() : gives the size of Dst buffer given a srcSize to handle worst case situations.
 *                        The LZ4F_frameInfo_t structure is optional :
 *                        you can provide NULL as argument, all preferences will then be set to default.
 * */
size_t LZ4F_compressBound(size_t srcSize, const LZ4F_frameInfo_t* frameInfoPtr)
{
	blockSizeID_t bid = (frameInfoPtr==NULL) ? LZ4F_BLOCKSIZEID_DEFAULT : frameInfoPtr->blockSizeID;
	size_t blockSize = LZ4F_getBlockSize(bid);
	size_t vSrcSize = srcSize + (blockSize-1);   /* worst case : tmp buffer almost filled */
	unsigned nbBlocks = vSrcSize / blockSize;
	size_t blockInfo = 4;   /* default, without block CRC option */
	size_t frameEnd = 4 + frameInfoPtr->contentChecksumFlag*4;
	size_t lastBlockSize = blockInfo + (blockSize-1) + frameEnd;
	size_t result = (blockSize + blockInfo) * nbBlocks;

    if (result < lastBlockSize) result = lastBlockSize;
	return result;
}

/* LZ4F_getMaxSrcSize() : gives max allowed srcSize given dstMaxSize to handle worst case situations.
 *                        You can use dstMaxSize==0 to know the "natural" srcSize instead (block size).
 *                        The LZ4F_frameInfo_t structure is optional :
 *                        you can provide NULL as argument, all preferences will then be set to default.
 * */
size_t LZ4F_getMaxSrcSize(size_t dstMaxSize, const LZ4F_frameInfo_t* frameInfoPtr)
{
	blockSizeID_t bid = (frameInfoPtr==NULL) ? LZ4F_BLOCKSIZEID_DEFAULT : frameInfoPtr->blockSizeID;
	size_t blockSize = LZ4F_getBlockSize(bid);
	size_t worstCBlockSize = blockSize + 4;   /* default, with no block CRC option */
	unsigned nbBlocks = dstMaxSize / worstCBlockSize;
	size_t maxSrcSize = nbBlocks * blockSize;

	if (dstMaxSize == 0) return blockSize;
	if (nbBlocks == 0) return -ERROR_dstMaxSize_tooSmall;   /* can't even fit one block */

	return maxSrcSize;
}


/* LZ4F_compress()
 * You can then call LZ4F_compress() repetitively to compress as much data as necessary.
 * The most important rule is that dstBuffer MUST be large enough (dstMaxSize) to ensure compression completion even in worst case.
 * You can get the minimum value of dstMaxSize by using LZ4F_compressBound()
 * Conversely, given a fixed dstMaxSize value, you can know the maximum srcSize authorized using LZ4F_getMaxSrcSize()
 * If this condition is not respected, LZ4F_compress() will fail (result is an errorCode)
 * The LZ4F_compressOptions_t structure is optional : you can provide NULL as argument.
 * The result of the function is the number of bytes written into dstBuffer (it can be zero, meaning input data is just stored within compressionContext for a future block to complete)
 * The function outputs an error code if it fails (can be tested using LZ4F_isError())
 */
size_t LZ4F_compress(LZ4F_compressionContext_t compressionContext, void* dstBuffer, size_t dstMaxSize, const void* srcBuffer, size_t srcSize, const LZ4F_compressOptions_t* compressOptionsPtr)
{
	LZ4F_compressOptions_t cOptionsNull = { 0 };
	LZ4F_cctx_internal_t* cctxPtr = (LZ4F_cctx_internal_t*)compressionContext;
	size_t blockSize = cctxPtr->maxBlockSize;
	const BYTE* srcPtr = (const BYTE*)srcBuffer;
	const BYTE* const srcEnd = srcPtr + srcSize;
	BYTE* const dstStart = (BYTE*)dstBuffer;
	BYTE* dstPtr = dstStart;


	if (cctxPtr->cStage != 1) return -ERROR_GENERIC;
	if (dstMaxSize < LZ4F_compressBound(srcSize, &(cctxPtr->prefs.frameInfo))) return -ERROR_dstMaxSize_tooSmall;
	if (compressOptionsPtr == NULL) compressOptionsPtr = &cOptionsNull;

	/* complete tmp buffer */
	if (cctxPtr->tmpInSize > 0)
	{
		size_t sizeToCopy = blockSize - cctxPtr->tmpInSize;
		if (sizeToCopy > srcSize)
		{
			/* add to tmp buffer */
			memcpy(cctxPtr->tmpIn + cctxPtr->tmpInSize, srcBuffer, srcSize);
			srcPtr = srcEnd;
			cctxPtr->tmpInSize += srcSize;
		}
		else
		{
			BYTE* cSizePtr = dstPtr;
			U32 cSize;
			memcpy(cctxPtr->tmpIn + cctxPtr->tmpInSize, srcBuffer, sizeToCopy);
			srcPtr += sizeToCopy;
			dstPtr += 4;   /* space for cSizePtr */
			cSize = (U32)LZ4_compress_limitedOutput((const char*)cctxPtr->tmpIn, (char*)dstPtr, (int)(blockSize), (int)(blockSize-1));
			dstPtr += cSize;
			LZ4F_writeLE32(cSizePtr, cSize);
			if (cSize == 0)   /* compression failed */
			{
				cSize = blockSize + LZ4F_BLOCKUNCOMPRESSED_FLAG;
				LZ4F_writeLE32(cSizePtr, cSize);
				memcpy(dstPtr, cctxPtr->tmpIn, blockSize);
				dstPtr += blockSize;
			}
			cctxPtr->tmpInSize = 0;
		}
	}

	while ((size_t)(srcEnd - srcPtr) >= blockSize)
	{
		/* compress one block */
		BYTE* cSizePtr = dstPtr;
		U32 cSize;
		dstPtr += 4;   /* space for cSizePtr */
		cSize = (U32)LZ4_compress_limitedOutput((const char*)srcPtr, (char*)dstPtr, (int)(blockSize), (int)(blockSize-1));
		dstPtr += cSize;
		LZ4F_writeLE32(cSizePtr, cSize);
		if (cSize == 0)   /* compression failed */
		{
			cSize = blockSize + LZ4F_BLOCKUNCOMPRESSED_FLAG;
			LZ4F_writeLE32(cSizePtr, cSize);
			memcpy(dstPtr, srcPtr, blockSize);
			dstPtr += blockSize;
		}
		srcPtr += blockSize;
	}

	if (srcPtr < srcEnd)
	{
		/* fill tmp buffer */
		size_t sizeToCopy = srcEnd - srcPtr;
		memcpy(cctxPtr->tmpIn, srcPtr, sizeToCopy);
		cctxPtr->tmpInSize = sizeToCopy;
	}

	if (cctxPtr->prefs.frameInfo.contentChecksumFlag == contentChecksumEnabled)
		XXH32_update(&(cctxPtr->xxh), srcBuffer, (unsigned)srcSize);

	return dstPtr - dstStart;
}


/* LZ4F_flush()
 * Should you need to create compressed data immediately, without waiting for a block to be filled,
 * you can call LZ4_flush(), which will immediately compress any remaining data stored within compressionContext.
 * The result of the function is the number of bytes written into dstBuffer
 * (it can be zero, this means there was no data left within compressionContext)
 * The function outputs an error code if it fails (can be tested using LZ4F_isError())
 * The LZ4F_compressOptions_t structure is optional : you can provide NULL as argument.
 */
size_t LZ4F_flush(LZ4F_compressionContext_t compressionContext, void* dstBuffer, size_t dstMaxSize, const LZ4F_compressOptions_t* compressOptionsPtr)
{
	LZ4F_compressOptions_t cOptionsNull = { 0 };
	LZ4F_cctx_internal_t* cctxPtr = (LZ4F_cctx_internal_t*)compressionContext;
	BYTE* const dstStart = (BYTE*)dstBuffer;
	BYTE* dstPtr = dstStart;


	if (cctxPtr->tmpInSize == 0) return 0;   /* nothing to flush */
	if (cctxPtr->cStage != 1) return -ERROR_GENERIC;
	if (dstMaxSize < LZ4F_compressBound(1, &(cctxPtr->prefs.frameInfo))) return -ERROR_dstMaxSize_tooSmall;
	if (compressOptionsPtr == NULL) compressOptionsPtr = &cOptionsNull;

	{
		BYTE* cSizePtr = dstPtr;
		U32 cSize;
		dstPtr += 4;   /* space for cSizePtr */
		cSize = (U32)LZ4_compress_limitedOutput((const char*)cctxPtr->tmpIn, (char*)dstPtr, (int)(cctxPtr->tmpInSize), (int)(cctxPtr->tmpInSize-1));
		dstPtr += cSize;
		LZ4F_writeLE32(cSizePtr, cSize);
		if (cSize == 0)   /* compression failed */
		{
			cSize = cctxPtr->tmpInSize + LZ4F_BLOCKUNCOMPRESSED_FLAG;
			LZ4F_writeLE32(cSizePtr, cSize);
			memcpy(dstPtr, cctxPtr->tmpIn, cctxPtr->tmpInSize);
			dstPtr += cctxPtr->tmpInSize;
		}
		cctxPtr->tmpInSize = 0;
	}

	return dstPtr - dstStart;
}


/* LZ4F_compressEnd()
 * When you want to properly finish the compressed frame, just call LZ4F_compressEnd().
 * It will flush whatever data remained within compressionContext (like LZ4_flush())
 * but also properly finalize the frame, with an endMark and a checksum.
 * The result of the function is the number of bytes written into dstBuffer (necessarily >= 4 (endMark size))
 * The function outputs an error code if it fails (can be tested using LZ4F_isError())
 * The LZ4F_compressOptions_t structure is optional : you can provide NULL as argument.
 * compressionContext can then be used again, starting with LZ4F_compressBegin(). The preferences will remain the same.
 */
size_t LZ4F_compressEnd(LZ4F_compressionContext_t compressionContext, void* dstBuffer, size_t dstMaxSize, const LZ4F_compressOptions_t* compressOptionsPtr)
{
	LZ4F_cctx_internal_t* cctxPtr = (LZ4F_cctx_internal_t*)compressionContext;
	BYTE* const dstStart = (BYTE*)dstBuffer;
	BYTE* dstPtr = dstStart;
	size_t errorCode;

	errorCode = LZ4F_flush(compressionContext, dstBuffer, dstMaxSize, compressOptionsPtr);
	if (LZ4F_isError(errorCode)) return errorCode;
	dstPtr += errorCode;

	LZ4F_writeLE32(dstPtr, 0); dstPtr+=4;   /* endMark */

	if (cctxPtr->prefs.frameInfo.contentChecksumFlag == contentChecksumEnabled)
	{
		U32 xxh = XXH32_intermediateDigest(&(cctxPtr->xxh));
		LZ4F_writeLE32(dstPtr, xxh);
		dstPtr+=4;   /* content Checksum */
	}

	cctxPtr->cStage = 0;   /* state is now re-usable (with identical preferences) */

	return dstPtr - dstStart;
}


/***********************************
 * Decompression functions
 * *********************************/

/* Resource management */

/* LZ4F_createDecompressionContext() :
 * The first thing to do is to create a decompressionContext object, which will be used in all decompression operations.
 * This is achieved using LZ4F_createDecompressionContext().
 * The function will provide a pointer to a fully allocated and initialized LZ4F_decompressionContext object.
 * If the result LZ4F_errorCode_t is not zero, there was an error during context creation.
 * Object can release its memory using LZ4F_freeDecompressionContext();
 */
LZ4F_errorCode_t LZ4F_createDecompressionContext(LZ4F_compressionContext_t* LZ4F_decompressionContextPtr, unsigned versionNumber)
{
    LZ4F_dctx_internal_t* dctxPtr;

    dctxPtr = ALLOCATOR(sizeof(LZ4F_dctx_internal_t));
    if (dctxPtr==NULL) return -ERROR_GENERIC;

    dctxPtr->version = versionNumber;
    *LZ4F_decompressionContextPtr = (LZ4F_compressionContext_t)dctxPtr;
    return OK_NoError;
}

LZ4F_errorCode_t LZ4F_freeDecompressionContext(LZ4F_compressionContext_t LZ4F_decompressionContext)
{
    LZ4F_dctx_internal_t* dctxPtr = (LZ4F_dctx_internal_t*)LZ4F_decompressionContext;
    FREEMEM(dctxPtr->tmpIn);
    FREEMEM(dctxPtr->tmpOut);
    FREEMEM(dctxPtr);
    return OK_NoError;
}


/* Decompression */

static size_t LZ4F_decodeHeader(LZ4F_dctx_internal_t* dctxPtr, const BYTE* srcPtr, size_t srcSize)
{
    BYTE FLG, BD, HC;
    unsigned version, blockMode, blockChecksumFlag, contentSizeFlag, contentChecksumFlag, dictFlag, blockSizeID;

    /* need to decode header to get frameInfo */
    if (srcSize < 7) return -ERROR_GENERIC;   /* minimal header size */

    /* control magic number */
    if (LZ4F_readLE32(srcPtr) != LZ4F_MAGICNUMBER) return -ERROR_GENERIC;
    srcPtr += 4;

    /* Flags */
    FLG = srcPtr[0];
    version = (FLG>>6)&_2BITS;
    blockMode = (FLG>>5) & _1BIT;
    blockChecksumFlag = (FLG>>4) & _1BIT;
    contentSizeFlag = (FLG>>3) & _1BIT;
    contentChecksumFlag = (FLG>>2) & _1BIT;
    dictFlag = (FLG>>0) & _1BIT;
    BD = srcPtr[1];
    blockSizeID = (BD>>4) & _3BITS;

    /* check */
    HC = LZ4F_headerChecksum(srcPtr, 2);
    if (HC != srcPtr[2]) return -ERROR_GENERIC;   /* Bad header checksum error */

    /* validate */
    if (version != 1) return -ERROR_GENERIC;   /* Version Number, only supported value */
    if (blockMode != blockIndependent) return -ERROR_GENERIC;   /* Only supported blockMode for the time being */
    if (blockChecksumFlag != 0) return -ERROR_GENERIC;   /* Only supported value for the time being */
    if (contentSizeFlag != 0) return -ERROR_GENERIC;   /* Only supported value for the time being */
    if (((FLG>>1)&_1BIT) != 0) return -ERROR_GENERIC;   /* Reserved bit */
    if (dictFlag != 0) return -ERROR_GENERIC;   /* Only supported value for the time being */
    if (((BD>>7)&_1BIT) != 0) return -ERROR_GENERIC;   /* Reserved bit */
    if (blockSizeID < 4) return -ERROR_GENERIC;   /* Only supported values for the time being */
    if (((BD>>0)&_4BITS) != 0) return -ERROR_GENERIC;   /* Reserved bits */

    /* save */
    dctxPtr->frameInfo.blockMode = blockMode;
    dctxPtr->frameInfo.contentChecksumFlag = contentChecksumFlag;
    dctxPtr->frameInfo.blockSizeID = blockSizeID;

    /* init */
    if (contentChecksumFlag) XXH32_resetState(&(dctxPtr->xxh), 0);

    if (LZ4F_getBlockSize(blockSizeID) > dctxPtr->maxBlockSize)   /* tmp buffers too small */
    {
        FREEMEM(dctxPtr->tmpIn);
        FREEMEM(dctxPtr->tmpOut);
        dctxPtr->tmpIn = ALLOCATOR(LZ4F_getBlockSize(blockSizeID));
        if (dctxPtr->tmpIn == NULL) return -ERROR_GENERIC;
        dctxPtr->tmpOut= ALLOCATOR(LZ4F_getBlockSize(blockSizeID));
        if (dctxPtr->tmpOut== NULL) return -ERROR_GENERIC;
        dctxPtr->maxBlockSize = LZ4F_getBlockSize(blockSizeID);
    }

    return 7;
}


typedef enum { dstage_getHeader=0, dstage_storeHeader, dstage_decodeHeader,
               dstage_getCBlockSize, dstage_storeCBlockSize, dstage_decodeCBlockSize,
               dstage_copyDirect,
               dstage_getCBlock, dstage_storeCBlock, dstage_decodeCBlock, dstage_flushOut,
               dstage_getSuffix, dstage_storeSuffix, dstage_checkSuffix } dStage_t;


/* LZ4F_getFrameInfo()
 * This function decodes frame header information, such as blockSize.
 * It is optional : you could start by calling directly LZ4F_decompress() instead.
 * The objective is to extract header information without starting decompression, typically for allocation purposes.
 * LZ4F_getFrameInfo() can also be used *after* starting decompression, on a valid LZ4F_decompressionContext_t.
 * The number of bytes read from srcBuffer will be provided within *srcSize (necessarily <= original value).
 * The function result is an error code which can be tested using LZ4F_isError().
 */
LZ4F_errorCode_t LZ4F_getFrameInfo(LZ4F_decompressionContext_t decompressionContext, LZ4F_frameInfo_t* frameInfoPtr, const void* srcBuffer, size_t* srcSize)
{
    LZ4F_dctx_internal_t* dctxPtr = (LZ4F_dctx_internal_t*)decompressionContext;

    if (dctxPtr->dStage==0)
    {
        LZ4F_errorCode_t errorCode = LZ4F_decodeHeader(dctxPtr, srcBuffer, *srcSize);
        if (LZ4F_isError(errorCode)) return errorCode;
        *srcSize = errorCode;
        dctxPtr->dStage = dstage_getCBlockSize;
        return OK_NoError;
    }

    /* frameInfo already decoded */
    *srcSize = 0;
    *frameInfoPtr = dctxPtr->frameInfo;
    return OK_NoError;
}


/* LZ4F_decompress()
 * Call this function repetitively to regenerate data compressed within srcBuffer.
 * The function will attempt to decode *srcSize from srcBuffer, into dstBuffer of maximum size *dstSize.
 *
 * The number of bytes generated into dstBuffer will be provided within *dstSize (necessarily <= original value).
 *
 * The number of bytes effectively read from srcBuffer will be provided within *srcSize (necessarily <= original value).
 * If the number of bytes read is < number of bytes provided, then the decompression operation is not complete.
 * You will have to call it again, using the same src arguments (but eventually different dst arguments).
 *
 * The function result is an error code which can be tested using LZ4F_isError().
 * When the frame is fully decoded, the function result will be OK_FrameEnd(=1).
 */
LZ4F_errorCode_t LZ4F_decompress(LZ4F_decompressionContext_t decompressionContext, void* dstBuffer, size_t* dstSize, const void* srcBuffer, size_t* srcSize, const LZ4F_decompressOptions_t* decompressOptionsPtr)
{
    LZ4F_dctx_internal_t* dctxPtr = (LZ4F_dctx_internal_t*)decompressionContext;
    LZ4F_decompressOptions_t optionsNull = { 0 };
    const BYTE* const srcStart = (const BYTE*)srcBuffer;
    const BYTE* const srcEnd = srcStart + *srcSize;
    const BYTE* srcPtr = srcStart;
    BYTE* const dstStart = (BYTE*)dstBuffer;
    BYTE* const dstEnd = dstStart + *dstSize;
    BYTE* dstPtr = dstStart;
    size_t nextCBlockSize=0;
    const BYTE* selectedIn=NULL;
    LZ4F_errorCode_t goodResult = OK_NoError;


    if (decompressOptionsPtr==NULL) decompressOptionsPtr = &optionsNull;
    *srcSize = 0; *dstSize = 0;

    /* expect to continue decoding src buffer where it left previously */
    if (dctxPtr->srcExpect != NULL)
    {
        if (srcStart != dctxPtr->srcExpect) return -ERROR_GENERIC;
    }

    while (srcPtr < srcEnd)
    {
        switch(dctxPtr->dStage)
        {
        case dstage_getHeader:
            {
                if (srcEnd-srcPtr >= 7)
                {
                    selectedIn = srcPtr;
                    srcPtr += 7;
                    dctxPtr->dStage = dstage_decodeHeader;
                    goto goto_decodeHeader;   /* break would risk leaving the while loop */
                }
                dctxPtr->tmpInSize = 0;
                dctxPtr->dStage = dstage_storeHeader;
                /* break;    break is useles, since storeHeader follows */
            }
        case dstage_storeHeader:
            {
                size_t sizeToCopy = 7 - dctxPtr->tmpInSize;
                if (sizeToCopy > (size_t)(srcEnd - srcPtr)) sizeToCopy =  srcEnd - srcPtr;
                memcpy(dctxPtr->header + dctxPtr->tmpInSize, srcPtr, sizeToCopy);
                dctxPtr->tmpInSize += sizeToCopy;
                srcPtr += sizeToCopy;
                if (dctxPtr->tmpInSize < 7) break;   /* src completed; come back later for more */
                selectedIn = dctxPtr->header;
                dctxPtr->dStage = dstage_decodeHeader;
                /* break;   useless because it follows */
            }
        case dstage_decodeHeader:
goto_decodeHeader:
            {
                LZ4F_errorCode_t errorCode = LZ4F_decodeHeader(dctxPtr, selectedIn, 7);
                if (LZ4F_isError(errorCode)) return errorCode;
                /* dctxPtr->dStage = dstage_getCBlockSize; break;     no need to change stage nor break : dstage_getCBlockSize is next stage, and stage will be modified */
            }
        case dstage_getCBlockSize:
            {
                if ((srcEnd - srcPtr) >= 4)
                {
                    selectedIn = srcPtr;
                    srcPtr += 4;
                    dctxPtr->dStage = dstage_decodeCBlockSize;
                    goto goto_decodeCBlockSize;   /* required : a break could leave while loop */
                }
                /* not enough input to read cBlockSize */
                dctxPtr->tmpInSize = 0;
                dctxPtr->dStage = dstage_storeCBlockSize;
                /* break;   No need to break : dstage_storeCBlockSize is next block */
            }
        case dstage_storeCBlockSize:
            {
                size_t sizeToCopy = 4 - dctxPtr->tmpInSize;
                if (sizeToCopy > (size_t)(srcEnd - srcPtr)) sizeToCopy = srcEnd - srcPtr;
                memcpy(dctxPtr->tmpIn + dctxPtr->tmpInSize, srcPtr, sizeToCopy);
                srcPtr += sizeToCopy;
                dctxPtr->tmpInSize += sizeToCopy;
                if (dctxPtr->tmpInSize < 4) break;   /* not enough input to read CBlockSize */
                selectedIn = dctxPtr->tmpIn;
                dctxPtr->dStage = dstage_decodeCBlockSize;
                /* break;   No need to break : dstage_decodeCBlockSize is next block */
            }
        case dstage_decodeCBlockSize:
goto_decodeCBlockSize:
            {
                nextCBlockSize = LZ4F_readLE32(selectedIn) & 0x7FFFFFFFU;
                if (nextCBlockSize==0)   /* no more CBlock */
                {
                    dctxPtr->dStage = dstage_getSuffix;
                    goto goto_getSuffix;   /* required : a break could leave the while loop */
                }
                if (nextCBlockSize > dctxPtr->maxBlockSize) return -ERROR_GENERIC;
                dctxPtr->sizeToDecode = nextCBlockSize;
                if (LZ4F_readLE32(selectedIn) & 0x80000000U)   /* uncompressed flag */
                {
                    dctxPtr->dStage = dstage_copyDirect;
                    break;
                }
                dctxPtr->dStage = dstage_getCBlock;
                goto goto_getCBlock;   /* break risk leaving while loop */
            }
        case dstage_copyDirect:
            {
                size_t sizeToCopy = dctxPtr->sizeToDecode;
                if ((size_t)(srcEnd-srcPtr) < sizeToCopy) sizeToCopy = srcEnd-srcPtr;  /* not enough input to read full block */
                if ((size_t)(dstEnd-dstPtr) < sizeToCopy) sizeToCopy = dstEnd - dstPtr;
                memcpy(dstPtr, srcPtr, sizeToCopy);
                srcPtr += sizeToCopy;
                dstPtr += sizeToCopy;
                if (sizeToCopy == dctxPtr->sizeToDecode)   /* all copied */
                {
                    dctxPtr->dStage = dstage_getCBlockSize;
                    break;
                }
                dctxPtr->sizeToDecode -= sizeToCopy;   /* still need to copy more */
                goto _end;                           /* either In or Out have reached end */
            }
        case dstage_getCBlock:
goto_getCBlock:
            {
                if ((size_t)(srcEnd-srcPtr) < nextCBlockSize)
                {
                    dctxPtr->tmpInTarget = nextCBlockSize;
                    dctxPtr->tmpInSize = 0;
                    dctxPtr->dStage = dstage_storeCBlock;
                    break;
                }
                selectedIn = srcPtr;
                srcPtr += nextCBlockSize;
                dctxPtr->dStage = dstage_decodeCBlock;
                break;
            }
        case dstage_storeCBlock:
            {
                size_t sizeToCopy = dctxPtr->tmpInTarget - dctxPtr->tmpInSize;
                if (sizeToCopy > (size_t)(srcEnd-srcPtr)) sizeToCopy = srcEnd-srcPtr;
                memcpy(dctxPtr->tmpIn + dctxPtr->tmpInSize, srcPtr, sizeToCopy);
                dctxPtr->tmpInSize += sizeToCopy;
                srcPtr += sizeToCopy;
                if (dctxPtr->tmpInSize < dctxPtr->tmpInTarget) break;   /* need to read more */
                selectedIn = dctxPtr->tmpIn;
                dctxPtr->dStage = dstage_decodeCBlock;
                /* break;   break unnecessary because it follows */
            }
        case dstage_decodeCBlock:
            {
                int decodedSize;
                if ((size_t)(dstEnd-dstPtr) < dctxPtr->maxBlockSize)   /* not enough room : decode into tmpOut */
                {
                    decodedSize = LZ4_decompress_safe((const char*)selectedIn, (char*)dctxPtr->tmpOut, (int)dctxPtr->sizeToDecode, (int)dctxPtr->maxBlockSize);
                    if (decodedSize < 0) return -ERROR_GENERIC;   /* decompression failed */
                    if (dctxPtr->frameInfo.contentChecksumFlag)
                        XXH32_update(&(dctxPtr->xxh), dctxPtr->tmpOut, decodedSize);
                    dctxPtr->tmpOutSize = decodedSize;
                    dctxPtr->tmpOutStart = 0;
                    dctxPtr->dStage = dstage_flushOut;
                    break;
                }
                decodedSize = LZ4_decompress_safe((const char*)selectedIn, (char*)dstPtr, (int)dctxPtr->sizeToDecode, (int)dctxPtr->maxBlockSize);
                if (decodedSize < 0) return -ERROR_GENERIC;   /* decompression failed */
                if (dctxPtr->frameInfo.contentChecksumFlag)
                    XXH32_update(&(dctxPtr->xxh), dstPtr, decodedSize);
                dstPtr += decodedSize;
                dctxPtr->dStage = dstage_getCBlockSize;
                break;
            }
        case dstage_flushOut:
            {
                size_t sizeToCopy = dctxPtr->tmpOutSize - dctxPtr->tmpOutStart;
                if (sizeToCopy > (size_t)(dstEnd-dstPtr)) sizeToCopy = dstEnd-dstPtr;
                memcpy(dstPtr, dctxPtr->tmpOut + dctxPtr->tmpOutStart, sizeToCopy);
                dctxPtr->tmpOutStart += sizeToCopy;
                dstPtr += sizeToCopy;
                if (dctxPtr->tmpOutStart < dctxPtr->tmpOutSize) goto _end;   /* need to write more */
                dctxPtr->dStage = dstage_getCBlockSize;
                break;
            }
        case dstage_getSuffix:
goto_getSuffix:
            {
                size_t suffixSize = dctxPtr->frameInfo.contentChecksumFlag * 4;
                if (suffixSize == 0)   /* frame completed */
                {
                    goodResult = OK_FrameEnd;
                    dctxPtr->dStage = dstage_getHeader;
                    goto _end;
                }
                if ((srcEnd - srcPtr) >= 4)   /* CRC present */
                {
                    selectedIn = srcPtr;
                    srcPtr += 4;
                    dctxPtr->dStage = dstage_checkSuffix;
                    goto goto_checkSuffix;   /* break risks leaving the while loop */
                }
                dctxPtr->tmpInSize = 0;
                dctxPtr->dStage = dstage_storeSuffix;
                /* break;   useless, it follow */
            }
        case dstage_storeSuffix:
            {
                size_t sizeToCopy = 4 - dctxPtr->tmpInSize;
                if (sizeToCopy < (size_t)(srcEnd - srcPtr)) sizeToCopy = srcEnd - srcPtr;
                memcpy(dctxPtr->tmpIn + dctxPtr->tmpInSize, srcPtr, sizeToCopy);
                srcPtr += sizeToCopy;
                dctxPtr->tmpInSize += sizeToCopy;
                if (dctxPtr->tmpInSize < 4) break;   /* not enough input to read suffix */
                selectedIn = dctxPtr->tmpIn;
                dctxPtr->dStage = dstage_checkSuffix;
                break;
            }
        case dstage_checkSuffix:
goto_checkSuffix:
            {
                U32 readCRC = LZ4F_readLE32(selectedIn);
                U32 resultCRC = XXH32_intermediateDigest(&(dctxPtr->xxh));
                if (readCRC != resultCRC) return -ERROR_GENERIC;
                goodResult = OK_FrameEnd;
                dctxPtr->dStage = dstage_getHeader;
                goto _end;
            }
        }
    }

    /* input fully read */

_end:

    if (srcPtr<srcEnd)   /* function must be called again with following source data */
    {
        dctxPtr->srcExpect = srcPtr;
    }
    else
    {
        dctxPtr->srcExpect = NULL;
    }
    *srcSize = (srcPtr - srcStart);
    *dstSize = (dstPtr - dstStart);
    return goodResult;
}
