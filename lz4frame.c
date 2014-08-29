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
   CPU Feature Detection
**************************************/
/* 32 or 64 bits ? */
static const int LZ4F_32bits = (sizeof(void*)==4);
static const int LZ4F_64bits = (sizeof(void*)==8);

/* Little Endian or Big Endian ? */
typedef union {
        int num;
        char bytes[4];
    } endian_t;
static const endian_t endianTest = { .num = 1 };
#define LZ4F_isLittleEndian (endianTest.bytes[0])


/**************************************
 Compiler Options
**************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  pragma warning(disable : 4127)        /* disable: C4127: conditional expression is constant */
#endif

#define GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)


/**************************************
   Memory routines
**************************************/
#include <stdlib.h>   /* malloc, calloc, free */
#define ALLOCATOR(n,s) calloc(n,s)
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

#if defined(__GNUC__)
#  define _PACKED __attribute__ ((packed))
#else
#  define _PACKED
#endif

#if !defined(__GNUC__)
#  if defined(__IBMC__) || defined(__SUNPRO_C) || defined(__SUNPRO_CC)
#    pragma pack(1)
#  else
#    pragma pack(push, 1)
#  endif
#endif

typedef struct { U16 v; }  _PACKED U16_S;
typedef struct { U32 v; }  _PACKED U32_S;
typedef struct { U64 v; }  _PACKED U64_S;
typedef struct {size_t v;} _PACKED size_t_S;

#if !defined(__GNUC__)
#  if defined(__SUNPRO_C) || defined(__SUNPRO_CC)
#    pragma pack(0)
#  else
#    pragma pack(pop)
#  endif
#endif

#define A16(x)   (((U16_S *)(x))->v)
#define A32(x)   (((U32_S *)(x))->v)
#define A64(x)   (((U64_S *)(x))->v)
#define AARCH(x) (((size_t_S *)(x))->v)


/**************************************
   Constants
**************************************/
#define KB *(1<<10)
#define MB *(1<<20)
#define GB *(1<<30)

#define LZ4F_BLOCKSIZEID_DEFAULT 4


/**************************************
   Structures and local types
**************************************/


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
	if (blockSizeID > 3) return ERROR_maxBlockSize_invalid;
	return blockSizes[blockSizeID];
}


/**************************************
   Error management
**************************************/
int LZ4F_isError(LZ4F_errorCode_t code)
{
	return (code > (LZ4F_errorCode_t)(-ERROR_maxCode));
}


/**************************************
   Compression functions
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
	
	blockSize = LZ4F_getBlockSize(frameInfoPtr->maxBlockSizeID);
	nbBlocks = (srcSize + (blockSize-1)) / blockSize;
	blockInfoSize *= nbBlocks;   /* total block info size */
	
	frameSuffixSize = 4;  /* basic frameSuffixSize (no option) */
	if (frameInfoPtr->contentChecksumFlag == contentChecksumEnabled) frameSuffixSize += 4;
	
	totalBound = headerSize + srcSize + blockInfoSize + frameSuffixSize;
	if (totalBound < srcSize) return ERROR_srcSize_tooLarge;   /* overflow error */
	
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
	LZ4F_compressionContext_t cctx;
	LZ4F_errorCode_t errorCode;
	BYTE* const dstStart = (BYTE*) dstBuffer;
	BYTE* dstPtr = dstStart;
	size_t blockSize = LZ4F_getBlockSize(frameInfoPtr->maxBlockSizeID);
	unsigned nbBlocks = (srcSize + (blockSize-1)) / blockSize;
	unsigned blockNb;
	const BYTE* srcPtr = (const BYTE*) srcBuffer;
	const size_t dstBlockSize = LZ4F_compressBound(blockSize, frameInfoPtr);
	

	if (dstMaxSize < LZ4F_compressFrameBound(srcSize, frameInfoPtr))
		return ERROR_maxDstSize_tooSmall;
	
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
	blockSize = srcSize % blockSize;
	errorCode = LZ4F_compress(cctx, dstPtr, dstBlockSize, srcPtr, blockSize, NULL);
	if (LZ4F_isError(errorCode)) return errorCode;
	dstPtr += errorCode;
	
	errorCode = LZ4F_compressEnd(cctx, dstPtr, dstMaxSize, NULL);   /* flush last block, and generate suffix */
	if (LZ4F_isError(errorCode)) return errorCode;
	dstPtr += errorCode;

	errorCode = LZ4F_freeCompressionContext(cctx);
	if (LZ4F_isError(errorCode)) return errorCode;

	return (dstPtr - dstStart);
}
