/*
    fuzzer.c - Fuzzer test tool for LZ4
    Copyright (C) Yann Collet 2012-2020

    GPL v2 License

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

    You can contact the author at :
    - LZ4 homepage : http://www.lz4.org
    - LZ4 source repo : https://github.com/lz4/lz4
*/

/*-************************************
*  Compiler options
**************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  pragma warning(disable : 4127)    /* disable: C4127: conditional expression is constant */
#  pragma warning(disable : 4146)    /* disable: C4146: minus unsigned expression */
#  pragma warning(disable : 4310)    /* disable: C4310: constant char value > 127 */
#  pragma warning(disable : 26451)   /* disable: C26451: Arithmetic overflow */
#endif


/*-************************************
*  Dependencies
**************************************/
#if defined(__unix__) && !defined(_AIX)   /* must be included before platform.h for MAP_ANONYMOUS */
#  undef  _GNU_SOURCE     /* in case it's already defined */
#  define _GNU_SOURCE     /* MAP_ANONYMOUS even in -std=c99 mode */
#  include <sys/mman.h>   /* mmap */
#endif
#include "platform.h"   /* _CRT_SECURE_NO_WARNINGS */
#include "util.h"       /* U32 */
#include <stdlib.h>
#include <stdio.h>      /* fgets, sscanf */
#include <string.h>     /* strcmp */
#include <time.h>       /* clock_t, clock, CLOCKS_PER_SEC */
#include <assert.h>
#include <limits.h>     /* INT_MAX */

#if defined(_AIX)
#  include <sys/mman.h>   /* mmap */
#endif

#ifndef LZ4_DISABLE_DEPRECATE_WARNINGS
#  define LZ4_DISABLE_DEPRECATE_WARNINGS   /* LZ4_decompress_fast */
#endif
#define LZ4_STATIC_LINKING_ONLY
#include "lz4.h"
#define LZ4_HC_STATIC_LINKING_ONLY
#include "lz4hc.h"
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"


/*-************************************
*  Constants
**************************************/
#define NB_ATTEMPTS (1<<16)
#define COMPRESSIBLE_NOISE_LENGTH (1 << 21)
#define FUZ_MAX_BLOCK_SIZE (1 << 17)
#define FUZ_MAX_DICT_SIZE  (1 << 15)
#define FUZ_COMPRESSIBILITY_DEFAULT 60
#define PRIME1   2654435761U
#define PRIME2   2246822519U
#define PRIME3   3266489917U

#define KB *(1U<<10)
#define MB *(1U<<20)
#define GB *(1U<<30)


/*-***************************************
*  Macros
*****************************************/
#define DISPLAY(...)         fprintf(stdout, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...) do { if (g_displayLevel>=l) DISPLAY(__VA_ARGS__); } while (0)
static int g_displayLevel = 2;

#undef MIN
#define MIN(a,b)   ( (a) < (b) ? (a) : (b) )


/*-*******************************************************
*  Fuzzer functions
*********************************************************/
static clock_t FUZ_GetClockSpan(clock_t clockStart)
{
    return clock() - clockStart;   /* works even if overflow; max span ~ 30mn */
}

static void FUZ_displayUpdate(unsigned testNb)
{
    static clock_t g_time = 0;
    static const clock_t g_refreshRate = CLOCKS_PER_SEC / 5;
    if ((FUZ_GetClockSpan(g_time) > g_refreshRate) || (g_displayLevel>=4)) {
        g_time = clock();
        DISPLAY("\r%5u   ", testNb);
        fflush(stdout);
    }
}

static U32 FUZ_rotl32(U32 u32, U32 nbBits)
{
    return ((u32 << nbBits) | (u32 >> (32 - nbBits)));
}

static U32 FUZ_highbit32(U32 v32)
{
    unsigned nbBits = 0;
    if (v32==0) return 0;
    while (v32) { v32 >>= 1; nbBits++; }
    return nbBits;
}

static U32 FUZ_rand(U32* src)
{
    U32 rand32 = *src;
    rand32 *= PRIME1;
    rand32 ^= PRIME2;
    rand32  = FUZ_rotl32(rand32, 13);
    *src = rand32;
    return rand32;
}


#define FUZ_RAND15BITS  ((FUZ_rand(seed) >> 3) & 32767)
#define FUZ_RANDLENGTH  ( ((FUZ_rand(seed) >> 7) & 3) ? (FUZ_rand(seed) % 15) : (FUZ_rand(seed) % 510) + 15)
static void FUZ_fillCompressibleNoiseBuffer(void* buffer, size_t bufferSize, double proba, U32* seed)
{
    BYTE* const BBuffer = (BYTE*)buffer;
    size_t pos = 0;
    U32 const P32 = (U32)(32768 * proba);

    /* First Bytes */
    while (pos < 20)
        BBuffer[pos++] = (BYTE)(FUZ_rand(seed));

    while (pos < bufferSize) {
        /* Select : Literal (noise) or copy (within 64K) */
        if (FUZ_RAND15BITS < P32) {
            /* Copy (within 64K) */
            size_t const length = (size_t)FUZ_RANDLENGTH + 4;
            size_t const d = MIN(pos+length, bufferSize);
            size_t match;
            size_t offset = (size_t)FUZ_RAND15BITS + 1;
            while (offset > pos) offset >>= 1;
            match = pos - offset;
            while (pos < d) BBuffer[pos++] = BBuffer[match++];
        } else {
            /* Literal (noise) */
            size_t const length = FUZ_RANDLENGTH;
            size_t const d = MIN(pos+length, bufferSize);
            while (pos < d) BBuffer[pos++] = (BYTE)(FUZ_rand(seed) >> 5);
        }
    }
}


#ifdef __unix__   /* is expected to be triggered on linux+gcc */

static void* FUZ_createLowAddr(size_t size)
{
    void* const lowBuff = mmap((void*)(0x1000), size,
                    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                    -1, 0);
    DISPLAYLEVEL(2, "generating low buffer at address %p \n", lowBuff);
    return lowBuff;
}

static void FUZ_freeLowAddr(void* buffer, size_t size)
{
    if (munmap(buffer, size)) {
        perror("fuzzer: freeing low address buffer");
        abort();
    }
}

#else

static void* FUZ_createLowAddr(size_t size)
{
    return malloc(size);
}

static void FUZ_freeLowAddr(void* buffer, size_t size)
{
    (void)size;
    free(buffer);
}

#endif


/*! FUZ_findDiff() :
*   find the first different byte between buff1 and buff2.
*   presumes buff1 != buff2.
*   presumes a difference exists before end of either buffer.
*   Typically invoked after a checksum mismatch.
*/
static void FUZ_findDiff(const void* buff1, const void* buff2)
{
    const BYTE* const b1 = (const BYTE*)buff1;
    const BYTE* const b2 = (const BYTE*)buff2;
    size_t u = 0;
    if ((buff1 == NULL) || (buff2 == NULL)) {
        DISPLAY(" one of the buffers is NULL \n");
        return;
    }
    while (b1[u]==b2[u]) u++;
    DISPLAY("\nWrong Byte at position %u \n", (unsigned)u);
}

static int FUZ_test(U32 seed, U32 nbCycles, const U32 startCycle, const double compressibility, U32 duration_s)
{
    unsigned long long bytes = 0;
    unsigned long long cbytes = 0;
    unsigned long long hcbytes = 0;
    unsigned long long ccbytes = 0;
    void* const CNBuffer = malloc(COMPRESSIBLE_NOISE_LENGTH);
    size_t const compressedBufferSize = (size_t)LZ4_compressBound(FUZ_MAX_BLOCK_SIZE);
    char* const compressedBuffer = (char*)malloc(compressedBufferSize);
    char* const decodedBuffer = (char*)malloc(FUZ_MAX_DICT_SIZE + FUZ_MAX_BLOCK_SIZE);
    size_t const labSize = 96 KB;
    void* const lowAddrBuffer = FUZ_createLowAddr(labSize);
    void* const stateLZ4   = malloc((size_t)LZ4_sizeofState());
    void* const stateLZ4HC = malloc((size_t)LZ4_sizeofStateHC());
    LZ4_stream_t LZ4dictBody;
    LZ4_stream_t* const reusedStream = LZ4_createStream();   /* keeps a history across cycles */
    LZ4_streamHC_t* const LZ4dictHC = LZ4_createStreamHC();
    U32 coreRandState = seed;
    clock_t const clockStart = clock();
    clock_t const clockDuration = (clock_t)duration_s * CLOCKS_PER_SEC;
    int result = 0;
    unsigned cycleNb;

#   define EXIT_MSG(...) do {                          \
    printf("Test %u : ", testNb); printf(__VA_ARGS__); \
    printf(" (seed %u, cycle %u) \n", seed, cycleNb);  \
    exit(1);                                           \
} while (0)

#   define FUZ_CHECKTEST(cond, ...)  do { if (cond) EXIT_MSG(__VA_ARGS__); } while (0)

#   define FUZ_DISPLAYTEST(...) do {              \
                testNb++;                         \
                if (g_displayLevel>=4) {          \
                    printf("\r%4u - %2u :", cycleNb, testNb);  \
                    printf(" " __VA_ARGS__);      \
                    printf("   ");                \
                    fflush(stdout);               \
                }                                 \
            } while (0)


    /* init */
    if(!CNBuffer || !compressedBuffer || !decodedBuffer || !LZ4dictHC || !reusedStream) {
        DISPLAY("Not enough memory to start fuzzer tests");
        exit(1);
    }
    if ( LZ4_initStream(&LZ4dictBody, sizeof(LZ4dictBody)) == NULL) abort();
    {   U32 randState = coreRandState ^ PRIME3;
        FUZ_fillCompressibleNoiseBuffer(CNBuffer, COMPRESSIBLE_NOISE_LENGTH, compressibility, &randState);
    }

    /* move to startCycle */
    for (cycleNb = 0; cycleNb < startCycle; cycleNb++)
        (void) FUZ_rand(&coreRandState);   /* sync coreRandState */

    /* Main test loop */
    for (cycleNb = startCycle;
        (cycleNb < nbCycles) || (FUZ_GetClockSpan(clockStart) < clockDuration);
        cycleNb++) {
        U32 testNb = 0;
        U32 randState = FUZ_rand(&coreRandState) ^ PRIME3;
        int const blockSize  = (FUZ_rand(&randState) % (FUZ_MAX_BLOCK_SIZE-1));
        int const blockStart = (int)(FUZ_rand(&randState) % (U32)(COMPRESSIBLE_NOISE_LENGTH - blockSize - 1)) + 1;
        int const dictSizeRand = FUZ_rand(&randState) % FUZ_MAX_DICT_SIZE;
        int const dictSize = MIN(dictSizeRand, blockStart - 1);
        int const compressionLevel = FUZ_rand(&randState) % (LZ4HC_CLEVEL_MAX+1);
        const char* block = blockSize ? ((char*)CNBuffer) + blockStart : NULL;
        const char* dict = (char*)CNBuffer + blockStart - dictSize;
        int compressedSize, HCcompressedSize;
        int extStateSize = 0;
        U32 extStateCrc = 0;
        int blockContinueCompressedSize;
        U32 const crcOrig = XXH32(block, (size_t)blockSize, 0);
        int ret;

        FUZ_displayUpdate(cycleNb);

        /* Compression tests */
        if ( ((FUZ_rand(&randState) & 63) == 2)
          && ((size_t)blockSize < labSize) ) {
            if (blockSize) { assert(block); memcpy(lowAddrBuffer, block, blockSize); }
            block = (const char*)lowAddrBuffer;
        }

        /* Test compression destSize */
        FUZ_DISPLAYTEST("test LZ4_compress_destSize()");
        {   int cSize, srcSize = blockSize;
            int const targetSize = srcSize * (int)((FUZ_rand(&randState) & 127)+1) >> 7;
            char const endCheck = (char)(FUZ_rand(&randState) & 255);
            compressedBuffer[targetSize] = endCheck;
            cSize = LZ4_compress_destSize(block, compressedBuffer, &srcSize, targetSize);
            FUZ_CHECKTEST(cSize > targetSize, "LZ4_compress_destSize() result larger than dst buffer !");
            FUZ_CHECKTEST(compressedBuffer[targetSize] != endCheck, "LZ4_compress_destSize() overwrite dst buffer !");
            FUZ_CHECKTEST(srcSize > blockSize, "LZ4_compress_destSize() read more than src buffer !");
            DISPLAYLEVEL(5, "destSize : %7i/%7i; content%7i/%7i ", cSize, targetSize, srcSize, blockSize);
            if (targetSize>0) {
                /* check correctness */
                U32 const crcBase = XXH32(block, (size_t)srcSize, 0);
                char const canary = (char)(FUZ_rand(&randState) & 255);
                FUZ_CHECKTEST((cSize==0), "LZ4_compress_destSize() compression failed");
                FUZ_DISPLAYTEST();
                decodedBuffer[srcSize] = canary;
                {   int const dSize = LZ4_decompress_safe(compressedBuffer, decodedBuffer, cSize, srcSize);
                    FUZ_CHECKTEST(dSize<0, "LZ4_decompress_safe() failed on data compressed by LZ4_compress_destSize");
                    FUZ_CHECKTEST(dSize!=srcSize, "LZ4_decompress_safe() failed : did not fully decompressed data");
                }
                FUZ_CHECKTEST(decodedBuffer[srcSize] != canary, "LZ4_decompress_safe() overwrite dst buffer !");
                {   U32 const crcDec = XXH32(decodedBuffer, (size_t)srcSize, 0);
                    FUZ_CHECKTEST(crcDec!=crcBase, "LZ4_decompress_safe() corrupted decoded data");
            }   }
            DISPLAYLEVEL(5, " OK \n");
        }

        /* Test compression HC destSize */
        FUZ_DISPLAYTEST("test LZ4_compress_HC_destSize()");
        {   int cSize, srcSize = blockSize;
            int const targetSize = srcSize * (int)((FUZ_rand(&randState) & 127)+1) >> 7;
            char const endCheck = (char)(FUZ_rand(&randState) & 255);
            void* const ctx = LZ4_createHC(block);
            FUZ_CHECKTEST(ctx==NULL, "LZ4_createHC() allocation failed");
            compressedBuffer[targetSize] = endCheck;
            cSize = LZ4_compress_HC_destSize(ctx, block, compressedBuffer, &srcSize, targetSize, compressionLevel);
            DISPLAYLEVEL(5, "LZ4_compress_HC_destSize(%i): destSize : %7i/%7i; content%7i/%7i ",
                            compressionLevel, cSize, targetSize, srcSize, blockSize);
            LZ4_freeHC(ctx);
            FUZ_CHECKTEST(cSize > targetSize, "LZ4_compress_HC_destSize() result larger than dst buffer !");
            FUZ_CHECKTEST(compressedBuffer[targetSize] != endCheck, "LZ4_compress_HC_destSize() overwrite dst buffer !");
            FUZ_CHECKTEST(srcSize > blockSize, "LZ4_compress_HC_destSize() fed more than src buffer !");
            if (targetSize>0) {
                /* check correctness */
                U32 const crcBase = XXH32(block, (size_t)srcSize, 0);
                char const canary = (char)(FUZ_rand(&randState) & 255);
                FUZ_CHECKTEST((cSize==0), "LZ4_compress_HC_destSize() compression failed");
                FUZ_DISPLAYTEST();
                decodedBuffer[srcSize] = canary;
                {   int const dSize = LZ4_decompress_safe(compressedBuffer, decodedBuffer, cSize, srcSize);
                    FUZ_CHECKTEST(dSize<0, "LZ4_decompress_safe failed (%i) on data compressed by LZ4_compress_HC_destSize", dSize);
                    FUZ_CHECKTEST(dSize!=srcSize, "LZ4_decompress_safe failed : decompressed %i bytes, was supposed to decompress %i bytes", dSize, srcSize);
                }
                FUZ_CHECKTEST(decodedBuffer[srcSize] != canary, "LZ4_decompress_safe overwrite dst buffer !");
                {   U32 const crcDec = XXH32(decodedBuffer, (size_t)srcSize, 0);
                    FUZ_CHECKTEST(crcDec!=crcBase, "LZ4_decompress_safe() corrupted decoded data");
            }   }
            DISPLAYLEVEL(5, " OK \n");
        }

        /* Test compression HC */
        FUZ_DISPLAYTEST("test LZ4_compress_HC()");
        HCcompressedSize = LZ4_compress_HC(block, compressedBuffer, blockSize, (int)compressedBufferSize, compressionLevel);
        FUZ_CHECKTEST(HCcompressedSize==0, "LZ4_compress_HC() failed");

        /* Test compression HC using external state */
        FUZ_DISPLAYTEST("test LZ4_compress_HC_extStateHC()");
        {   int const r = LZ4_compress_HC_extStateHC(stateLZ4HC, block, compressedBuffer, blockSize, (int)compressedBufferSize, compressionLevel);
            FUZ_CHECKTEST(r==0, "LZ4_compress_HC_extStateHC() failed");
        }

        /* Test compression HC using fast reset external state */
        FUZ_DISPLAYTEST("test LZ4_compress_HC_extStateHC_fastReset()");
        {   int const r = LZ4_compress_HC_extStateHC_fastReset(stateLZ4HC, block, compressedBuffer, blockSize, (int)compressedBufferSize, compressionLevel);
            FUZ_CHECKTEST(r==0, "LZ4_compress_HC_extStateHC_fastReset() failed");
        }

        /* Test compression using external state */
        FUZ_DISPLAYTEST("test LZ4_compress_fast_extState()");
        {   int const r = LZ4_compress_fast_extState(stateLZ4, block, compressedBuffer, blockSize, (int)compressedBufferSize, 8);
            FUZ_CHECKTEST(r==0, "LZ4_compress_fast_extState() failed");
            extStateSize = r;
            extStateCrc = XXH32(compressedBuffer, (size_t)r, 0);

            FUZ_DISPLAYTEST("test LZ4_compress_fast_extState() with a too small destination buffer (must fail)");
            {   int const r2 = LZ4_compress_fast_extState(stateLZ4, block, compressedBuffer, blockSize, r-1, 8);
                FUZ_CHECKTEST(r2!=0, "LZ4_compress_fast_extState() should have failed");
            }

            FUZ_DISPLAYTEST("test LZ4_compress_destSize_extState() with too small dest buffer (must succeed, compress less than full input)");
            {   int inputSize = blockSize;
                /* note: LZ4_compress_destSize() still requires a targetDstSize > 0, since it's the minimum valid output size produced by LZ4 */
                int targetDstSize = r-1 < 1 ? 1 : r-1;
                int const r3 = LZ4_compress_destSize_extState(stateLZ4, block, compressedBuffer, &inputSize, targetDstSize, 8);
                FUZ_CHECKTEST(r3==0, "LZ4_compress_destSize_extState() failed");
                if (targetDstSize < r)
                    FUZ_CHECKTEST(inputSize>=blockSize, "LZ4_compress_destSize_extState() should consume less than full input");
            }
        }

        /* Test compression using fast reset external state : same output as a fully initialized state */
        FUZ_DISPLAYTEST("test LZ4_compress_fast_extState_fastReset()");
        {   int const r = LZ4_compress_fast_extState_fastReset(stateLZ4, block, compressedBuffer, blockSize, (int)compressedBufferSize, 8);
            FUZ_CHECKTEST(r==0, "LZ4_compress_fast_extState_fastReset() failed");
            FUZ_CHECKTEST(r != extStateSize || XXH32(compressedBuffer, (size_t)r, 0) != extStateCrc,
                "LZ4_compress_fast_extState_fastReset() output differs from LZ4_compress_fast_extState() (%i vs %i bytes)",
                r, extStateSize);
        }

        /* new stream on a used state : same output as a new stream, if its first block is large enough */
        if (blockSize > 0) {
            FUZ_DISPLAYTEST("test LZ4_compress_fast_continue() after LZ4_resetStream_fast()");
            LZ4_resetStream_fast(reusedStream);
            ret = LZ4_compress_fast_continue(reusedStream, block, compressedBuffer, blockSize, (int)compressedBufferSize, 1);
            FUZ_CHECKTEST(ret <= 0, "LZ4_compress_fast_continue() failed");
            if (blockSize >= (int)(64 KB)) {   /* beyond the table clearing threshold, whatever LZ4_MEMORY_USAGE */
                U32 const reusedCrc = XXH32(compressedBuffer, (size_t)ret, 0);
                LZ4_stream_t newStream;
                int r;
                LZ4_initStream(&newStream, sizeof(newStream));
                r = LZ4_compress_fast_continue(&newStream, block, compressedBuffer, blockSize, (int)compressedBufferSize, 1);
                FUZ_CHECKTEST(r != ret || XXH32(compressedBuffer, (size_t)r, 0) != reusedCrc,
                    "LZ4_compress_fast_continue() after LZ4_resetStream_fast() : output differs from a new stream (%i vs %i bytes)",
                    ret, r);
        }   }

        /* Test compression */
        FUZ_DISPLAYTEST("test LZ4_compress_default()");
        compressedSize = LZ4_compress_default(block, compressedBuffer, blockSize, (int)compressedBufferSize);
        FUZ_CHECKTEST(compressedSize<=0, "LZ4_compress_default() failed");

        /* Decompression tests */

        /* Test decompress_fast() with input buffer size exactly correct => must not read out of bound */
        {   char* const cBuffer_exact = (char*)malloc((size_t)compressedSize);
            assert(cBuffer_exact != NULL);
            assert(compressedSize <= (int)compressedBufferSize);
#if defined(_MSC_VER) && (_MSC_VER <= 1936) /* MSVC 2022 ver 17.6 or earlier */
#  pragma warning(push)
#  pragma warning(disable : 6385) /* lz4\tests\fuzzer.c(497): warning C6385: Reading invalid data from 'compressedBuffer'. */
#endif
            memcpy(cBuffer_exact, compressedBuffer, compressedSize);
#if defined(_MSC_VER) && (_MSC_VER <= 1936) /* MSVC 2022 ver 17.6 or earlier */
#  pragma warning(pop)
#endif

            /* Test decoding with output size exactly correct => must work */
            FUZ_DISPLAYTEST("LZ4_decompress_fast() with exact output buffer");
            {   int const r = LZ4_decompress_fast(cBuffer_exact, decodedBuffer, blockSize);
                FUZ_CHECKTEST(r<0, "LZ4_decompress_fast failed despite correct space");
                FUZ_CHECKTEST(r!=compressedSize, "LZ4_decompress_fast failed : did not fully read compressed data");
            }
            {   U32 const crcCheck = XXH32(decodedBuffer, (size_t)blockSize, 0);
                FUZ_CHECKTEST(crcCheck!=crcOrig, "LZ4_decompress_fast corrupted decoded data");
            }

            /* Test decoding with one byte missing => must fail */
            if (blockSize >= 1) {
                FUZ_DISPLAYTEST("LZ4_decompress_fast() with output buffer 1-byte too short");
                decodedBuffer[blockSize-1] = 0;
                {   int const r = LZ4_decompress_fast(cBuffer_exact, decodedBuffer, blockSize-1);
                    FUZ_CHECKTEST(r>=0, "LZ4_decompress_fast should have failed, due to Output Size being too small");
                }
                FUZ_CHECKTEST(decodedBuffer[blockSize-1]!=0, "LZ4_decompress_fast overrun specified output buffer");
            }

            /* Test decoding with one byte too much => must fail */
            FUZ_DISPLAYTEST();
            {   int const r = LZ4_decompress_fast(cBuffer_exact, decodedBuffer, blockSize+1);
                FUZ_CHECKTEST(r>=0, "LZ4_decompress_fast should have failed, due to Output Size being too large");
            }

            /* Test decoding with a negative output size => must fail */
            FUZ_DISPLAYTEST("LZ4_decompress_fast() with negative output size");
            {   int const r = LZ4_decompress_fast(cBuffer_exact, decodedBuffer, -1);
                FUZ_CHECKTEST(r>=0, "LZ4_decompress_fast should have failed, due to negative Output Size");
            }

            /* Test decoding with output size exactly what's necessary => must work */
            FUZ_DISPLAYTEST();
            decodedBuffer[blockSize] = 0;
            {   int const r = LZ4_decompress_safe(cBuffer_exact, decodedBuffer, compressedSize, blockSize);
                FUZ_CHECKTEST(r<0, "LZ4_decompress_safe failed despite sufficient space");
                FUZ_CHECKTEST(r!=blockSize, "LZ4_decompress_safe did not regenerate original data");
            }
            FUZ_CHECKTEST(decodedBuffer[blockSize], "LZ4_decompress_safe overrun specified output buffer size");
            {   U32 const crcCheck = XXH32(decodedBuffer, (size_t)blockSize, 0);
                FUZ_CHECKTEST(crcCheck!=crcOrig, "LZ4_decompress_safe corrupted decoded data");
            }

            /* Test decoding with more than enough output size => must work */
            FUZ_DISPLAYTEST();
            decodedBuffer[blockSize] = 0;
            decodedBuffer[blockSize+1] = 0;
            {   int const r = LZ4_decompress_safe(cBuffer_exact, decodedBuffer, compressedSize, blockSize+1);
                FUZ_CHECKTEST(r<0, "LZ4_decompress_safe failed despite amply sufficient space");
                FUZ_CHECKTEST(r!=blockSize, "LZ4_decompress_safe did not regenerate original data");
            }
            FUZ_CHECKTEST(decodedBuffer[blockSize+1], "LZ4_decompress_safe overrun specified output buffer size");
            {   U32 const crcCheck = XXH32(decodedBuffer, (size_t)blockSize, 0);
                FUZ_CHECKTEST(crcCheck!=crcOrig, "LZ4_decompress_safe corrupted decoded data");
            }

            /* Test decoding with output size being one byte too short => must fail */
            if (blockSize >= 1) {
                FUZ_DISPLAYTEST();
                decodedBuffer[blockSize-1] = 0;
                {   int const r = LZ4_decompress_safe(cBuffer_exact, decodedBuffer, compressedSize, blockSize-1);
                    FUZ_CHECKTEST(r>=0, "LZ4_decompress_safe should have failed, due to Output Size being one byte too short");
                }
                FUZ_CHECKTEST(decodedBuffer[blockSize-1], "LZ4_decompress_safe overrun specified output buffer size");
            }

            /* Test decoding with output size being 10 bytes too short => must fail */
            FUZ_DISPLAYTEST();
            if (blockSize>10) {
                decodedBuffer[blockSize-10] = 0;
                {   int const r = LZ4_decompress_safe(cBuffer_exact, decodedBuffer, compressedSize, blockSize-10);
                    FUZ_CHECKTEST(r>=0, "LZ4_decompress_safe should have failed, due to Output Size being 10 bytes too short");
                }
                FUZ_CHECKTEST(decodedBuffer[blockSize-10], "LZ4_decompress_safe overrun specified output buffer size");
            }

            /* noisy src decompression test */

            /* insert noise into src */
            {   U32 const maxNbBits = FUZ_highbit32((U32)compressedSize);
                size_t pos = 0;
                for (;;) {
                    /* keep some original src */
                    {   U32 const nbBits = FUZ_rand(&randState) % maxNbBits;
                        size_t const mask = (1ULL <<nbBits) - 1;
                        size_t const skipLength = FUZ_rand(&randState) & mask;
                        pos += skipLength;
                    }
                    if (pos >= (size_t)compressedSize) break;
                    /* add noise */
                    {   U32 const nbBitsCodes = FUZ_rand(&randState) % maxNbBits;
                        U32 const nbBits = nbBitsCodes ? nbBitsCodes-1 : 0;
                        size_t const mask = (1ULL <<nbBits) - 1;
                        size_t const rNoiseLength = (FUZ_rand(&randState) & mask) + 1;
                        size_t const noiseLength = MIN(rNoiseLength, (size_t)compressedSize-pos);
                        size_t const noiseStart = FUZ_rand(&randState) % (COMPRESSIBLE_NOISE_LENGTH - noiseLength);
                        memcpy(cBuffer_exact + pos, (const char*)CNBuffer + noiseStart, noiseLength);
                        pos += noiseLength;
            }   }   }

            /* decompress noisy source */
            FUZ_DISPLAYTEST("decompress noisy source ");
            {   U32 const endMark = 0xA9B1C3D6;
                memcpy(decodedBuffer+blockSize, &endMark, sizeof(endMark));
                {   int const decompressResult = LZ4_decompress_safe(cBuffer_exact, decodedBuffer, compressedSize, blockSize);
                    /* result *may* be an unlikely success, but even then, it must strictly respect dst buffer boundaries */
                    FUZ_CHECKTEST(decompressResult > blockSize, "LZ4_decompress_safe on noisy src : result is too large : %u > %u (dst buffer)", (unsigned)decompressResult, (unsigned)blockSize);
                }
                {   U32 endCheck; memcpy(&endCheck, decodedBuffer+blockSize, sizeof(endCheck));
                    FUZ_CHECKTEST(endMark!=endCheck, "LZ4_decompress_safe on noisy src : dst buffer overflow");
            }   }   /* noisy src decompression test */

            free(cBuffer_exact);
        }

        /* Test decoding with input size being one byte too short => must fail */
        FUZ_DISPLAYTEST();
        {   int const r = LZ4_decompress_safe(compressedBuffer, decodedBuffer, compressedSize-1, blockSize);
            FUZ_CHECKTEST(r>=0, "LZ4_decompress_safe should have failed, due to input size being one byte too short (blockSize=%i, result=%i, compressedSize=%i)", blockSize, r, compressedSize);
        }

        /* Test decoding with input size being one byte too large => must fail */
        FUZ_DISPLAYTEST();
        decodedBuffer[blockSize] = 0;
        {   int const r = LZ4_decompress_safe(compressedBuffer, decodedBuffer, compressedSize+1, blockSize);
            FUZ_CHECKTEST(r>=0, "LZ4_decompress_safe should have failed, due to input size being too large");
        }
        FUZ_CHECKTEST(decodedBuffer[blockSize], "LZ4_decompress_safe overrun specified output buffer size");

        if (blockSize) {
            /* Test partial decoding with a negative input size => must fail */
            FUZ_DISPLAYTEST("LZ4_decompress_safe_partial() with negative input size");
            {   int const r = LZ4_decompress_safe_partial(compressedBuffer, decodedBuffer, -1, blockSize, blockSize);
                FUZ_CHECKTEST(r>=0, "LZ4_decompress_safe_partial should have failed, due to negative input size");
            }

            /* Test partial decoding => must work */
            FUZ_DISPLAYTEST("test LZ4_decompress_safe_partial");
            {   size_t const missingOutBytes = FUZ_rand(&randState) % (unsigned)blockSize;
                int const targetSize = (int)((size_t)blockSize - missingOutBytes);
                size_t const extraneousInBytes = FUZ_rand(&randState) % 2;
                int const inCSize = (int)((size_t)compressedSize + extraneousInBytes);
                char const sentinel = decodedBuffer[targetSize] = block[targetSize] ^ 0x5A;
                int const decResult = LZ4_decompress_safe_partial(compressedBuffer, decodedBuffer, inCSize, targetSize, blockSize);
                FUZ_CHECKTEST(decResult<0, "LZ4_decompress_safe_partial failed despite valid input data (error:%i)", decResult);
                FUZ_CHECKTEST(decResult != targetSize, "LZ4_decompress_safe_partial did not regenerated required amount of data (%i < %i <= %i)", decResult, targetSize, blockSize);
                FUZ_CHECKTEST(decodedBuffer[targetSize] != sentinel, "LZ4_decompress_safe_partial overwrite beyond requested size (though %i <= %i <= %i)", decResult, targetSize, blockSize);
                FUZ_CHECKTEST(memcmp(block, decodedBuffer, (size_t)targetSize), "LZ4_decompress_safe_partial: corruption detected in regenerated data");
            }

            /* Partial decompression using dictionary. */
            FUZ_DISPLAYTEST("test LZ4_decompress_safe_partial_usingDict using no dict");
            {   size_t const missingOutBytes = FUZ_rand(&randState) % (unsigned)blockSize;
                int const targetSize = (int)((size_t)blockSize - missingOutBytes);
                size_t const extraneousInBytes = FUZ_rand(&randState) % 2;
                int const inCSize = (int)((size_t)compressedSize + extraneousInBytes);
                char const sentinel = decodedBuffer[targetSize] = block[targetSize] ^ 0x5A;
                int const decResult = LZ4_decompress_safe_partial_usingDict(compressedBuffer, decodedBuffer, inCSize, targetSize, blockSize, NULL, 0);
                FUZ_CHECKTEST(decResult<0, "LZ4_decompress_safe_partial_usingDict failed despite valid input data (error:%i)", decResult);
                FUZ_CHECKTEST(decResult != targetSize, "LZ4_decompress_safe_partial_usingDict did not regenerated required amount of data (%i < %i <= %i)", decResult, targetSize, blockSize);
                FUZ_CHECKTEST(decodedBuffer[targetSize] != sentinel, "LZ4_decompress_safe_partial_usingDict overwrite beyond requested size (though %i <= %i <= %i)", decResult, targetSize, blockSize);
                FUZ_CHECKTEST(memcmp(block, decodedBuffer, (size_t)targetSize), "LZ4_decompress_safe_partial_usingDict: corruption detected in regenerated data");
            }

            FUZ_DISPLAYTEST("test LZ4_decompress_safe_partial_usingDict() using prefix as dict");
            {   size_t const missingOutBytes = FUZ_rand(&randState) % (unsigned)blockSize;
                int const targetSize = (int)((size_t)blockSize - missingOutBytes);
                size_t const extraneousInBytes = FUZ_rand(&randState) % 2;
                int const inCSize = (int)((size_t)compressedSize + extraneousInBytes);
                char const sentinel = decodedBuffer[targetSize] = block[targetSize] ^ 0x5A;
                int const decResult = LZ4_decompress_safe_partial_usingDict(compressedBuffer, decodedBuffer, inCSize, targetSize, blockSize, decodedBuffer, dictSize);
                FUZ_CHECKTEST(decResult<0, "LZ4_decompress_safe_partial_usingDict failed despite valid input data (error:%i)", decResult);
                FUZ_CHECKTEST(decResult != targetSize, "LZ4_decompress_safe_partial_usingDict did not regenerated required amount of data (%i < %i <= %i)", decResult, targetSize, blockSize);
                FUZ_CHECKTEST(decodedBuffer[targetSize] != sentinel, "LZ4_decompress_safe_partial_usingDict overwrite beyond requested size (though %i <= %i <= %i)", decResult, targetSize, blockSize);
                FUZ_CHECKTEST(memcmp(block, decodedBuffer, (size_t)targetSize), "LZ4_decompress_safe_partial_usingDict: corruption detected in regenerated data");
            }

            FUZ_DISPLAYTEST("test LZ4_decompress_safe_partial_usingDict() using external dict");
            {   size_t const missingOutBytes = FUZ_rand(&randState) % (unsigned)blockSize;
                int const targetSize = (int)((size_t)blockSize - missingOutBytes);
                size_t const extraneousInBytes = FUZ_rand(&randState) % 2;
                int const inCSize = (int)((size_t)compressedSize + extraneousInBytes);
                char const sentinel = decodedBuffer[targetSize] = block[targetSize] ^ 0x5A;
                int const decResult = LZ4_decompress_safe_partial_usingDict(compressedBuffer, decodedBuffer, inCSize, targetSize, blockSize, dict, dictSize);
                FUZ_CHECKTEST(decResult<0, "LZ4_decompress_safe_partial_usingDict failed despite valid input data (error:%i)", decResult);
                FUZ_CHECKTEST(decResult != targetSize, "LZ4_decompress_safe_partial_usingDict did not regenerated required amount of data (%i < %i <= %i)", decResult, targetSize, blockSize);
                FUZ_CHECKTEST(decodedBuffer[targetSize] != sentinel, "LZ4_decompress_safe_partial_usingDict overwrite beyond requested size (though %i <= %i <= %i)", decResult, targetSize, blockSize);
                FUZ_CHECKTEST(memcmp(block, decodedBuffer, (size_t)targetSize), "LZ4_decompress_safe_partial_usingDict: corruption detected in regenerated data");
            }
        }

        /* Test Compression with limited output size */

        /* Test compression with output size being exactly what's necessary (should work) */
        FUZ_DISPLAYTEST("test LZ4_compress_default() with output buffer just the right size");
        ret = LZ4_compress_default(block, compressedBuffer, blockSize, compressedSize);
        FUZ_CHECKTEST(ret==0, "LZ4_compress_default() failed despite sufficient space");

        /* Test compression with output size being exactly what's necessary and external state (should work) */
        FUZ_DISPLAYTEST("test LZ4_compress_fast_extState() with output buffer just the right size");
        ret = LZ4_compress_fast_extState(stateLZ4, block, compressedBuffer, blockSize, compressedSize, 1);
        FUZ_CHECKTEST(ret==0, "LZ4_compress_fast_extState() failed despite sufficient space");

        /* Test HC compression with output size being exactly what's necessary (should work) */
        FUZ_DISPLAYTEST("test LZ4_compress_HC() with output buffer just the right size");
        ret = LZ4_compress_HC(block, compressedBuffer, blockSize, HCcompressedSize, compressionLevel);
        FUZ_CHECKTEST(ret==0, "LZ4_compress_HC() failed despite sufficient space");

        /* Test HC compression with output size being exactly what's necessary (should work) */
        FUZ_DISPLAYTEST("test LZ4_compress_HC_extStateHC() with output buffer just the right size");
        ret = LZ4_compress_HC_extStateHC(stateLZ4HC, block, compressedBuffer, blockSize, HCcompressedSize, compressionLevel);
        FUZ_CHECKTEST(ret==0, "LZ4_compress_HC_extStateHC() failed despite sufficient space");

        /* Test compression with missing bytes into output buffer => must fail */
        FUZ_DISPLAYTEST("test LZ4_compress_default() with output buffer a bit too short");
        {   int missingBytes = (FUZ_rand(&randState) % 0x3F) + 1;
            if (missingBytes >= compressedSize) missingBytes = compressedSize-1;
            missingBytes += !missingBytes;   /* avoid special case missingBytes==0 */
            compressedBuffer[compressedSize-missingBytes] = 0;
            {   int const cSize = LZ4_compress_default(block, compressedBuffer, blockSize, compressedSize-missingBytes);
                FUZ_CHECKTEST(cSize, "LZ4_compress_default should have failed (output buffer too small by %i byte)", missingBytes);
            }
            FUZ_CHECKTEST(compressedBuffer[compressedSize-missingBytes], "LZ4_compress_default overran output buffer ! (%i missingBytes)", missingBytes);
        }

        /* Test HC compression with missing bytes into output buffer => must fail */
        FUZ_DISPLAYTEST("test LZ4_compress_HC() with output buffer a bit too short");
        {   int missingBytes = (FUZ_rand(&randState) % 0x3F) + 1;
            if (missingBytes >= HCcompressedSize) missingBytes = HCcompressedSize-1;
            missingBytes += !missingBytes;   /* avoid special case missingBytes==0 */
            compressedBuffer[HCcompressedSize-missingBytes] = 0;
            {   int const hcSize = LZ4_compress_HC(block, compressedBuffer, blockSize, HCcompressedSize-missingBytes, compressionLevel);
                FUZ_CHECKTEST(hcSize, "LZ4_compress_HC should have failed (output buffer too small by %i byte)", missingBytes);
            }
            FUZ_CHECKTEST(compressedBuffer[HCcompressedSize-missingBytes], "LZ4_compress_HC overran output buffer ! (%i missingBytes)", missingBytes);
        }


        /*-******************/
        /* Dictionary tests */
        /*-******************/

        /* Compress using dictionary */
        FUZ_DISPLAYTEST("test LZ4_compress_fast_continue() with dictionary of size %i", dictSize);
        {   LZ4_stream_t LZ4_stream;
            LZ4_initStream(&LZ4_stream, sizeof(LZ4_stream));
            LZ4_compress_fast_continue (&LZ4_stream, dict, compressedBuffer, dictSize, (int)compressedBufferSize, 1);   /* Just to fill hash tables */
            blockContinueCompressedSize = LZ4_compress_fast_continue (&LZ4_stream, block, compressedBuffer, blockSize, (int)compressedBufferSize, 1);
            FUZ_CHECKTEST(blockContinueCompressedSize==0, "LZ4_compress_fast_continue failed");
        }

        /* Decompress with dictionary as prefix */
        FUZ_DISPLAYTEST("test LZ4_decompress_fast_usingDict() with dictionary as prefix");
        if (dictSize) memcpy(decodedBuffer, dict, dictSize);
        ret = LZ4_decompress_fast_usingDict(compressedBuffer, decodedBuffer+dictSize, blockSize, decodedBuffer, dictSize);
        FUZ_CHECKTEST(ret!=blockContinueCompressedSize, "LZ4_decompress_fast_usingDict did not read all compressed block input");
        {   U32 const crcCheck = XXH32(decodedBuffer+dictSize, (size_t)blockSize, 0);
            if (crcCheck!=crcOrig) {
                FUZ_findDiff(block, decodedBuffer);
                EXIT_MSG("LZ4_decompress_fast_usingDict corrupted decoded data (dict %i)", dictSize);
        }   }

        FUZ_DISPLAYTEST("test LZ4_decompress_safe_usingDict()");
        ret = LZ4_decompress_safe_usingDict(compressedBuffer, decodedBuffer+dictSize, blockContinueCompressedSize, blockSize, decodedBuffer, dictSize);
        FUZ_CHECKTEST(ret!=blockSize, "LZ4_decompress_safe_usingDict did not regenerate original data");
        {   U32 const crcCheck = XXH32(decodedBuffer+dictSize, (size_t)blockSize, 0);
            FUZ_CHECKTEST(crcCheck!=crcOrig, "LZ4_decompress_safe_usingDict corrupted decoded data");
        }

        /* Compress using External dictionary */
        FUZ_DISPLAYTEST("test LZ4_compress_fast_continue(), with non-contiguous dictionary");
        dict -= (size_t)(FUZ_rand(&randState) & 0xF) + 1;   /* create space, so now dictionary is an ExtDict */
        if (dict < (char*)CNBuffer) dict = (char*)CNBuffer;
        LZ4_loadDict(&LZ4dictBody, dict, dictSize);
        blockContinueCompressedSize = LZ4_compress_fast_continue(&LZ4dictBody, block, compressedBuffer, blockSize, (int)compressedBufferSize, 1);
        FUZ_CHECKTEST(blockContinueCompressedSize==0, "LZ4_compress_fast_continue failed");

        FUZ_DISPLAYTEST("LZ4_compress_fast_continue() with dictionary and output buffer too short by one byte");
        LZ4_loadDict(&LZ4dictBody, dict, dictSize);
        assert(blockContinueCompressedSize >= 1);
        ret = LZ4_compress_fast_continue(&LZ4dictBody, block, compressedBuffer, blockSize, blockContinueCompressedSize-1, 1);
        FUZ_CHECKTEST(ret>0, "LZ4_compress_fast_continue using ExtDict should fail : one missing byte for output buffer : %i written, %i buffer", ret, blockContinueCompressedSize);

        FUZ_DISPLAYTEST("test LZ4_compress_fast_continue() with dictionary loaded with LZ4_loadDict()");
        DISPLAYLEVEL(5, " compress %i bytes from buffer(%p) into dst(%p) using dict(%p) of size %i \n",
                     blockSize, (const void *)block, (void *)decodedBuffer, (const void *)dict, dictSize);
        LZ4_loadDict(&LZ4dictBody, dict, dictSize);
        ret = LZ4_compress_fast_continue(&LZ4dictBody, block, compressedBuffer, blockSize, blockContinueCompressedSize, 1);
        FUZ_CHECKTEST(ret!=blockContinueCompressedSize, "LZ4_compress_limitedOutput_compressed size is different (%i != %i)", ret, blockContinueCompressedSize);
        FUZ_CHECKTEST(ret<=0, "LZ4_compress_fast_continue should work : enough size available within output buffer");

        /* Decompress with dictionary as external */
        FUZ_DISPLAYTEST("test LZ4_decompress_fast_usingDict() with dictionary as extDict");
        DISPLAYLEVEL(5, " decoding %i bytes from buffer(%p) using dict(%p) of size %i \n",
                     blockSize, (void *)decodedBuffer, (const void *)dict, dictSize);
        decodedBuffer[blockSize] = 0;
        ret = LZ4_decompress_fast_usingDict(compressedBuffer, decodedBuffer, blockSize, dict, dictSize);
        FUZ_CHECKTEST(ret!=blockContinueCompressedSize, "LZ4_decompress_fast_usingDict did not read all compressed block input");
        FUZ_CHECKTEST(decodedBuffer[blockSize], "LZ4_decompress_fast_usingDict overrun specified output buffer size");
        {   U32 const crcCheck = XXH32(decodedBuffer, (size_t)blockSize, 0);
            if (crcCheck!=crcOrig) {
                FUZ_findDiff(block, decodedBuffer);
                EXIT_MSG("LZ4_decompress_fast_usingDict corrupted decoded data (dict %i)", dictSize);
        }   }

        FUZ_DISPLAYTEST();
        decodedBuffer[blockSize] = 0;
        ret = LZ4_decompress_safe_usingDict(compressedBuffer, decodedBuffer, blockContinueCompressedSize, blockSize, dict, dictSize);
        FUZ_CHECKTEST(ret!=blockSize, "LZ4_decompress_safe_usingDict did not regenerate original data");
        FUZ_CHECKTEST(decodedBuffer[blockSize], "LZ4_decompress_safe_usingDict overrun specified output buffer size");
        {   U32 const crcCheck = XXH32(decodedBuffer, (size_t)blockSize, 0);
            FUZ_CHECKTEST(crcCheck!=crcOrig, "LZ4_decompress_safe_usingDict corrupted decoded data");
        }

        if (blockSize >= 1) {
            FUZ_DISPLAYTEST();
            decodedBuffer[blockSize-1] = 0;
            ret = LZ4_decompress_fast_usingDict(compressedBuffer, decodedBuffer, blockSize-1, dict, dictSize);
            FUZ_CHECKTEST(ret>=0, "LZ4_decompress_fast_usingDict should have failed : wrong original size (-1 byte)");
            FUZ_CHECKTEST(decodedBuffer[blockSize-1], "LZ4_decompress_fast_usingDict overrun specified output buffer size");

            FUZ_DISPLAYTEST();
            decodedBuffer[blockSize-1] = 0;
            ret = LZ4_decompress_safe_usingDict(compressedBuffer, decodedBuffer, blockContinueCompressedSize, blockSize-1, dict, dictSize);
            FUZ_CHECKTEST(ret>=0, "LZ4_decompress_safe_usingDict should have failed : not enough output size (-1 byte)");
            FUZ_CHECKTEST(decodedBuffer[blockSize-1], "LZ4_decompress_safe_usingDict overrun specified output buffer size");
        }

        FUZ_DISPLAYTEST();
        {   int const missingBytes = (FUZ_rand(&randState) & 0xF) + 2;
            if (blockSize > missingBytes) {
                decodedBuffer[blockSize-missingBytes] = 0;
                ret = LZ4_decompress_safe_usingDict(compressedBuffer, decodedBuffer, blockContinueCompressedSize, blockSize-missingBytes, dict, dictSize);
                FUZ_CHECKTEST(ret>=0, "LZ4_decompress_safe_usingDict should have failed : output buffer too small (-%i byte)", missingBytes);
                FUZ_CHECKTEST(decodedBuffer[blockSize-missingBytes], "LZ4_decompress_safe_usingDict overrun specified output buffer size (-%i byte) (blockSize=%i)", missingBytes, blockSize);
        }   }

        /* Test decoding with a negative dictionary size => must fail */
        FUZ_DISPLAYTEST("LZ4_decompress_*_usingDict() with negative dictionary size");
        ret = LZ4_decompress_safe_usingDict(compressedBuffer, decodedBuffer, blockContinueCompressedSize, blockSize, dict, -1);
        FUZ_CHECKTEST(ret>=0, "LZ4_decompress_safe_usingDict should have failed, due to negative dictionary size");
        ret = LZ4_decompress_safe_partial_usingDict(compressedBuffer, decodedBuffer, blockContinueCompressedSize, blockSize, blockSize, dict, -1);
        FUZ_CHECKTEST(ret>=0, "LZ4_decompress_safe_partial_usingDict should have failed, due to negative dictionary size");
        ret = LZ4_decompress_fast_usingDict(compressedBuffer, decodedBuffer, blockSize, dict, -1);
        FUZ_CHECKTEST(ret>=0, "LZ4_decompress_fast_usingDict should have failed, due to negative dictionary size");
        {   LZ4_streamDecode_t sd;
            FUZ_CHECKTEST(LZ4_setStreamDecode(&sd, dict, -1) != 0, "LZ4_setStreamDecode should have failed, due to negative dictionary size");
        }

        /* Compress using external dictionary stream */
        {   LZ4_stream_t LZ4_stream;
            int expectedSize;
            U32 expectedCrc;

            /* one-shot compression ignores the dictionary attached to a new state */
            FUZ_DISPLAYTEST("LZ4_compress_fast_extState_fastReset() after LZ4_attach_dictionary()");
            LZ4_loadDict(&LZ4dictBody, dict, dictSize);
            LZ4_initStream(&LZ4_stream, sizeof(LZ4_stream));
            LZ4_attach_dictionary(&LZ4_stream, &LZ4dictBody);
            ret = LZ4_compress_fast_extState_fastReset(&LZ4_stream, block, compressedBuffer, blockSize, (int)compressedBufferSize, 8);
            FUZ_CHECKTEST(ret != extStateSize || XXH32(compressedBuffer, (size_t)ret, 0) != extStateCrc,
                "LZ4_compress_fast_extState_fastReset() after LZ4_attach_dictionary() differs from LZ4_compress_fast_extState() (%i vs %i bytes)",
                ret, extStateSize);

            FUZ_DISPLAYTEST("LZ4_compress_fast_continue() after LZ4_loadDict()");
            LZ4_loadDict(&LZ4dictBody, dict, dictSize);
            expectedSize = LZ4_compress_fast_continue(&LZ4dictBody, block, compressedBuffer, blockSize, (int)compressedBufferSize, 1);
            FUZ_CHECKTEST(expectedSize<=0, "LZ4_compress_fast_continue reference compression for extDictCtx should have succeeded");
            expectedCrc = XXH32(compressedBuffer, (size_t)expectedSize, 0);

            FUZ_DISPLAYTEST("LZ4_compress_fast_continue() after LZ4_attach_dictionary()");
            LZ4_loadDict(&LZ4dictBody, dict, dictSize);
            LZ4_initStream(&LZ4_stream, sizeof(LZ4_stream));
            LZ4_attach_dictionary(&LZ4_stream, &LZ4dictBody);
            blockContinueCompressedSize = LZ4_compress_fast_continue(&LZ4_stream, block, compressedBuffer, blockSize, (int)compressedBufferSize, 1);
            FUZ_CHECKTEST(blockContinueCompressedSize==0, "LZ4_compress_fast_continue using extDictCtx failed");

            /* In the future, it might be desirable to let extDictCtx mode's
             * output diverge from the output generated by regular extDict mode.
             * Until that time, this comparison serves as a good regression
             * test.
             */
            FUZ_CHECKTEST(blockContinueCompressedSize != expectedSize, "LZ4_compress_fast_continue using extDictCtx produced different-sized output (%d expected vs %d actual)", expectedSize, blockContinueCompressedSize);
            FUZ_CHECKTEST(XXH32(compressedBuffer, (size_t)blockContinueCompressedSize, 0) != expectedCrc, "LZ4_compress_fast_continue using extDictCtx produced different output");

            FUZ_DISPLAYTEST("LZ4_compress_fast_continue() after LZ4_attach_dictionary(), but output buffer is 1 byte too short");
            LZ4_resetStream_fast(&LZ4_stream);
            LZ4_attach_dictionary(&LZ4_stream, &LZ4dictBody);
            assert(blockContinueCompressedSize >= 1);
            ret = LZ4_compress_fast_continue(&LZ4_stream, block, compressedBuffer, blockSize, blockContinueCompressedSize-1, 1);
            FUZ_CHECKTEST(ret>0, "LZ4_compress_fast_continue using extDictCtx should fail : one missing byte for output buffer : %i written, %i buffer", ret, blockContinueCompressedSize);
            /* note : context is no longer dirty after a failed compressed block */

            FUZ_DISPLAYTEST();
            LZ4_resetStream_fast(&LZ4_stream);
            LZ4_attach_dictionary(&LZ4_stream, &LZ4dictBody);
            ret = LZ4_compress_fast_continue(&LZ4_stream, block, compressedBuffer, blockSize, blockContinueCompressedSize, 1);
            FUZ_CHECKTEST(ret!=blockContinueCompressedSize, "LZ4_compress_limitedOutput_compressed size is different (%i != %i)", ret, blockContinueCompressedSize);
            FUZ_CHECKTEST(ret<=0, "LZ4_compress_fast_continue using extDictCtx should work : enough size available within output buffer");
            FUZ_CHECKTEST(ret != expectedSize, "LZ4_compress_fast_continue using extDictCtx produced different-sized output");
            FUZ_CHECKTEST(XXH32(compressedBuffer, (size_t)ret, 0) != expectedCrc, "LZ4_compress_fast_continue using extDictCtx produced different output");

            FUZ_DISPLAYTEST();
            LZ4_resetStream_fast(&LZ4_stream);
            LZ4_attach_dictionary(&LZ4_stream, &LZ4dictBody);
            ret = LZ4_compress_fast_continue(&LZ4_stream, block, compressedBuffer, blockSize, blockContinueCompressedSize, 1);
            FUZ_CHECKTEST(ret!=blockContinueCompressedSize, "LZ4_compress_limitedOutput_compressed size is different (%i != %i)", ret, blockContinueCompressedSize);
            FUZ_CHECKTEST(ret<=0, "LZ4_compress_fast_continue using extDictCtx with re-used context should work : enough size available within output buffer");
            FUZ_CHECKTEST(ret != expectedSize, "LZ4_compress_fast_continue using extDictCtx produced different-sized output");
            FUZ_CHECKTEST(XXH32(compressedBuffer, (size_t)ret, 0) != expectedCrc, "LZ4_compress_fast_continue using extDictCtx produced different output");
        }

        /* Decompress with dictionary as external */
        FUZ_DISPLAYTEST();
        decodedBuffer[blockSize] = 0;
        ret = LZ4_decompress_fast_usingDict(compressedBuffer, decodedBuffer, blockSize, dict, dictSize);
        FUZ_CHECKTEST(ret!=blockContinueCompressedSize, "LZ4_decompress_fast_usingDict did not read all compressed block input");
        FUZ_CHECKTEST(decodedBuffer[blockSize], "LZ4_decompress_fast_usingDict overrun specified output buffer size");
        {   U32 const crcCheck = XXH32(decodedBuffer, (size_t)blockSize, 0);
            if (crcCheck!=crcOrig) {
                FUZ_findDiff(block, decodedBuffer);
                EXIT_MSG("LZ4_decompress_fast_usingDict corrupted decoded data (dict %i)", dictSize);
        }   }

        FUZ_DISPLAYTEST();
        decodedBuffer[blockSize] = 0;
        ret = LZ4_decompress_safe_usingDict(compressedBuffer, decodedBuffer, blockContinueCompressedSize, blockSize, dict, dictSize);
        FUZ_CHECKTEST(ret!=blockSize, "LZ4_decompress_safe_usingDict did not regenerate original data");
        FUZ_CHECKTEST(decodedBuffer[blockSize], "LZ4_decompress_safe_usingDict overrun specified output buffer size");
        {   U32 const crcCheck = XXH32(decodedBuffer, (size_t)blockSize, 0);
            FUZ_CHECKTEST(crcCheck!=crcOrig, "LZ4_decompress_safe_usingDict corrupted decoded data");
        }

        if (blockSize >= 1) {
            FUZ_DISPLAYTEST();
            decodedBuffer[blockSize-1] = 0;
            ret = LZ4_decompress_fast_usingDict(compressedBuffer, decodedBuffer, blockSize-1, dict, dictSize);
            FUZ_CHECKTEST(ret>=0, "LZ4_decompress_fast_usingDict should have failed : wrong original size (-1 byte)");
            FUZ_CHECKTEST(decodedBuffer[blockSize-1], "LZ4_decompress_fast_usingDict overrun specified output buffer size");

            FUZ_DISPLAYTEST();
            decodedBuffer[blockSize-1] = 0;
            ret = LZ4_decompress_safe_usingDict(compressedBuffer, decodedBuffer, blockContinueCompressedSize, blockSize-1, dict, dictSize);
            FUZ_CHECKTEST(ret>=0, "LZ4_decompress_safe_usingDict should have failed : not enough output size (-1 byte)");
            FUZ_CHECKTEST(decodedBuffer[blockSize-1], "LZ4_decompress_safe_usingDict overrun specified output buffer size");
        }

        FUZ_DISPLAYTEST("LZ4_decompress_safe_usingDict with a too small output buffer");
        {   int const missingBytes = (FUZ_rand(&randState) & 0xF) + 2;
            if (blockSize > missingBytes) {
                decodedBuffer[blockSize-missingBytes] = 0;
                ret = LZ4_decompress_safe_usingDict(compressedBuffer, decodedBuffer, blockContinueCompressedSize, blockSize-missingBytes, dict, dictSize);
                FUZ_CHECKTEST(ret>=0, "LZ4_decompress_safe_usingDict should have failed : output buffer too small (-%i byte)", missingBytes);
                FUZ_CHECKTEST(decodedBuffer[blockSize-missingBytes], "LZ4_decompress_safe_usingDict overrun specified output buffer size (-%i byte) (blockSize=%i)", missingBytes, blockSize);
        }   }

        /* Compress HC using External dictionary */
        FUZ_DISPLAYTEST("LZ4_compress_HC_continue with an external dictionary");
        dict -= (FUZ_rand(&randState) & 7);    /* even bigger separation */
        if (dict < (char*)CNBuffer) dict = (char*)CNBuffer;
        LZ4_setCompressionLevel (LZ4dictHC, compressionLevel);
        FUZ_CHECKTEST(LZ4_loadDictHC(LZ4dictHC, dict, -1) != 0, "LZ4_loadDictHC should load nothing, due to negative dictionary size");
        LZ4_loadDictHC(LZ4dictHC, dict, dictSize);
        blockContinueCompressedSize = LZ4_compress_HC_continue(LZ4dictHC, block, compressedBuffer, blockSize, (int)compressedBufferSize);
        FUZ_CHECKTEST(blockContinueCompressedSize==0, "LZ4_compress_HC_continue failed");

        FUZ_DISPLAYTEST("LZ4_compress_HC_continue with same external dictionary, but output buffer 1 byte too short");
        LZ4_loadDictHC(LZ4dictHC, dict, dictSize);
        assert(blockContinueCompressedSize>=1);
        ret = LZ4_compress_HC_continue(LZ4dictHC, block, compressedBuffer, blockSize, blockContinueCompressedSize-1);
        FUZ_CHECKTEST(ret>0, "LZ4_compress_HC_continue using ExtDict should fail : one missing byte for output buffer (expected %i, but result=%i)", blockContinueCompressedSize, ret);
        /* note : context is no longer dirty after a failed compressed block */

        FUZ_DISPLAYTEST("LZ4_compress_HC_continue with same external dictionary, and output buffer exactly the right size");
        LZ4_loadDictHC(LZ4dictHC, dict, dictSize);
        ret = LZ4_compress_HC_continue(LZ4dictHC, block, compressedBuffer, blockSize, blockContinueCompressedSize);
        FUZ_CHECKTEST(ret!=blockContinueCompressedSize, "LZ4_compress_HC_continue size is different : ret(%i) != expected(%i)", ret, blockContinueCompressedSize);
        FUZ_CHECKTEST(ret<=0, "LZ4_compress_HC_continue should work : enough size available within output buffer");

        FUZ_DISPLAYTEST();
        decodedBuffer[blockSize] = 0;
        ret = LZ4_decompress_safe_usingDict(compressedBuffer, decodedBuffer, blockContinueCompressedSize, blockSize, dict, dictSize);
        FUZ_CHECKTEST(ret!=blockSize, "LZ4_decompress_safe_usingDict did not regenerate original data");
        FUZ_CHECKTEST(decodedBuffer[blockSize], "LZ4_decompress_safe_usingDict overrun specified output buffer size");
        {   U32 const crcCheck = XXH32(decodedBuffer, (size_t)blockSize, 0);
            if (crcCheck!=crcOrig) {
                FUZ_findDiff(block, decodedBuffer);
                EXIT_MSG("LZ4_decompress_safe_usingDict corrupted decoded data");
        }   }

        /* Compress HC using external dictionary stream */
        FUZ_DISPLAYTEST();
        {   LZ4_streamHC_t* const LZ4_streamHC = LZ4_createStreamHC();

            LZ4_loadDictHC(LZ4dictHC, dict, dictSize);
            LZ4_attach_HC_dictionary(LZ4_streamHC, LZ4dictHC);
            LZ4_setCompressionLevel (LZ4_streamHC, compressionLevel);
            blockContinueCompressedSize = LZ4_compress_HC_continue(LZ4_streamHC, block, compressedBuffer, blockSize, (int)compressedBufferSize);
            FUZ_CHECKTEST(blockContinueCompressedSize==0, "LZ4_compress_HC_continue with ExtDictCtx failed");

            FUZ_DISPLAYTEST();
            LZ4_resetStreamHC_fast (LZ4_streamHC, compressionLevel);
            LZ4_attach_HC_dictionary(LZ4_streamHC, LZ4dictHC);
            assert(blockContinueCompressedSize>=1);
            ret = LZ4_compress_HC_continue(LZ4_streamHC, block, compressedBuffer, blockSize, blockContinueCompressedSize-1);
            FUZ_CHECKTEST(ret>0, "LZ4_compress_HC_continue using ExtDictCtx should fail : one missing byte for output buffer (%i != %i)", ret, blockContinueCompressedSize);
            /* note : context is no longer dirty after a failed compressed block */

            FUZ_DISPLAYTEST();
            LZ4_resetStreamHC_fast (LZ4_streamHC, compressionLevel);
            LZ4_attach_HC_dictionary(LZ4_streamHC, LZ4dictHC);
            ret = LZ4_compress_HC_continue(LZ4_streamHC, block, compressedBuffer, blockSize, blockContinueCompressedSize);
            FUZ_CHECKTEST(ret!=blockContinueCompressedSize, "LZ4_compress_HC_continue using ExtDictCtx size is different (%i != %i)", ret, blockContinueCompressedSize);
            FUZ_CHECKTEST(ret<=0, "LZ4_compress_HC_continue using ExtDictCtx should work : enough size available within output buffer");

            FUZ_DISPLAYTEST();
            LZ4_resetStreamHC_fast (LZ4_streamHC, compressionLevel);
            LZ4_attach_HC_dictionary(LZ4_streamHC, LZ4dictHC);
            ret = LZ4_compress_HC_continue(LZ4_streamHC, block, compressedBuffer, blockSize, blockContinueCompressedSize);
            FUZ_CHECKTEST(ret!=blockContinueCompressedSize, "LZ4_compress_HC_continue using ExtDictCtx and fast reset size is different (%i != %i)",
                        ret, blockContinueCompressedSize);
            FUZ_CHECKTEST(ret<=0, "LZ4_compress_HC_continue using ExtDictCtx and fast reset should work : enough size available within output buffer");

            LZ4_freeStreamHC(LZ4_streamHC);
        }

        FUZ_DISPLAYTEST();
        decodedBuffer[blockSize] = 0;
        ret = LZ4_decompress_safe_usingDict(compressedBuffer, decodedBuffer, blockContinueCompressedSize, blockSize, dict, dictSize);
        FUZ_CHECKTEST(ret!=blockSize, "LZ4_decompress_safe_usingDict did not regenerate original data");
        FUZ_CHECKTEST(decodedBuffer[blockSize], "LZ4_decompress_safe_usingDict overrun specified output buffer size");
        {   U32 const crcCheck = XXH32(decodedBuffer, (size_t)blockSize, 0);
            if (crcCheck!=crcOrig) {
                FUZ_findDiff(block, decodedBuffer);
                EXIT_MSG("LZ4_decompress_safe_usingDict corrupted decoded data");
        }   }

        /* Compress HC continue destSize */
        FUZ_DISPLAYTEST();
        {   int const availableSpace = (blockSize ? (int)(FUZ_rand(&randState) % (U32)blockSize) : 0) + 5;
            int consumedSize = blockSize;
            FUZ_DISPLAYTEST();
            LZ4_loadDictHC(LZ4dictHC, dict, dictSize);
            LZ4_setCompressionLevel(LZ4dictHC, compressionLevel);
            blockContinueCompressedSize = LZ4_compress_HC_continue_destSize(LZ4dictHC, block, compressedBuffer, &consumedSize, availableSpace);
            DISPLAYLEVEL(5, " LZ4_compress_HC_continue_destSize : compressed %6i/%6i into %6i/%6i at cLevel=%i \n",
                        consumedSize, blockSize, blockContinueCompressedSize, availableSpace, compressionLevel);
            FUZ_CHECKTEST(blockContinueCompressedSize==0, "LZ4_compress_HC_continue_destSize failed");
            FUZ_CHECKTEST(blockContinueCompressedSize > availableSpace, "LZ4_compress_HC_continue_destSize write overflow");
            FUZ_CHECKTEST(consumedSize > blockSize, "LZ4_compress_HC_continue_destSize read overflow");

            FUZ_DISPLAYTEST();
            decodedBuffer[consumedSize] = 0;
            ret = LZ4_decompress_safe_usingDict(compressedBuffer, decodedBuffer, blockContinueCompressedSize, consumedSize, dict, dictSize);
            FUZ_CHECKTEST(ret != consumedSize, "LZ4_decompress_safe_usingDict regenerated %i bytes (%i expected)", ret, consumedSize);
            FUZ_CHECKTEST(decodedBuffer[consumedSize], "LZ4_decompress_safe_usingDict overrun specified output buffer size");
            {   U32 const crcSrc = XXH32(block, (size_t)consumedSize, 0);
                U32 const crcDst = XXH32(decodedBuffer, (size_t)consumedSize, 0);
                if (crcSrc!=crcDst) {
                    FUZ_findDiff(block, decodedBuffer);
                    EXIT_MSG("LZ4_decompress_safe_usingDict corrupted decoded data");
            }   }
        }

        /* ***** End of tests *** */
        /* Fill stats */
        assert(blockSize >= 0);
        bytes += (unsigned)blockSize;
        assert(compressedSize >= 0);
        cbytes += (unsigned)compressedSize;
        assert(HCcompressedSize >= 0);
        hcbytes += (unsigned)HCcompressedSize;
        assert(blockContinueCompressedSize >= 0);
        ccbytes += (unsigned)blockContinueCompressedSize;
    }

    if (nbCycles<=1) nbCycles = cycleNb;   /* end by time */
    bytes += !bytes;   /* avoid division by 0 */
    printf("\r%7u /%7u   - ", cycleNb, nbCycles);
    printf("all tests completed successfully \n");
    printf("compression ratio: %0.3f%%\n", (double)cbytes/(double)bytes*100);
    printf("HC compression ratio: %0.3f%%\n", (double)hcbytes/(double)bytes*100);
    printf("ratio with dict: %0.3f%%\n", (double)ccbytes/(double)bytes*100);

    /* release memory */
    free(CNBuffer);
    free(compressedBuffer);
    free(decodedBuffer);
    FUZ_freeLowAddr(lowAddrBuffer, labSize);
    LZ4_freeStreamHC(LZ4dictHC);
    LZ4_freeStream(reusedStream);
    free(stateLZ4);
    free(stateLZ4HC);
    return result;
}


/* =======================================
 * CLI
 * ======================================= */

static int FUZ_usage(const char* programName)
{
    DISPLAY( "Usage :\n");
    DISPLAY( "      %s [args]\n", programName);
    DISPLAY( "\n");
    DISPLAY( "Arguments :\n");
    DISPLAY( " -i#    : Nb of tests (default:%i) \n", NB_ATTEMPTS);
    DISPLAY( " -T#    : Duration of tests, in seconds (default: use Nb of tests) \n");
    DISPLAY( " -s#    : Select seed (default:prompt user)\n");
    DISPLAY( " -t#    : Select starting test number (default:0)\n");
    DISPLAY( " -P#    : Select compressibility in %% (default:%i%%)\n", FUZ_COMPRESSIBILITY_DEFAULT);
    DISPLAY( " -v     : verbose\n");
    DISPLAY( " -p     : pause at the end\n");
    DISPLAY( " -h     : display help and exit\n");
    return 0;
}


int main(int argc, const char** argv)
{
    U32 seed = 0;
    int seedset = 0;
    int argNb;
    unsigned nbTests = NB_ATTEMPTS;
    unsigned testNb = 0;
    int proba = FUZ_COMPRESSIBILITY_DEFAULT;
    int use_pause = 0;
    const char* programName = argv[0];
    U32 duration = 0;

    /* Check command line */
    for(argNb=1; argNb<argc; argNb++) {
        const char* argument = argv[argNb];

        if(!argument) continue;   // Protection if argument empty

        // Decode command (note : aggregated commands are allowed)
        if (argument[0]=='-') {
            if (!strcmp(argument, "--no-prompt")) { use_pause=0; seedset=1; g_displayLevel=1; continue; }
            argument++;

            while (*argument!=0) {
                switch(*argument)
                {
                case 'h':   /* display help */
                    return FUZ_usage(programName);

                case 'v':   /* verbose mode */
                    g_displayLevel++;
                    argument++;
                    break;

                case 'p':   /* pause at the end */
                    use_pause=1;
                    argument++;
                    break;

                case 'i':
                    argument++;
                    nbTests = 0; duration = 0;
                    while ((*argument>='0') && (*argument<='9')) {
                        nbTests *= 10;
                        nbTests += (unsigned)(*argument - '0');
                        argument++;
                    }
                    break;

                case 'T':
                    argument++;
                    nbTests = 0; duration = 0;
                    for (;;) {
                        switch(*argument)
                        {
                            case 'm': duration *= 60; argument++; continue;
                            case 's':
                            case 'n': argument++; continue;
                            case '0':
                            case '1':
                            case '2':
                            case '3':
                            case '4':
                            case '5':
                            case '6':
                            case '7':
                            case '8':
                            case '9': duration *= 10; duration += (U32)(*argument++ - '0'); continue;
                        }
                        break;
                    }
                    break;

                case 's':
                    argument++;
                    seed=0; seedset=1;
                    while ((*argument>='0') && (*argument<='9')) {
                        seed *= 10;
                        seed += (U32)(*argument - '0');
                        argument++;
                    }
                    break;

                case 't':   /* select starting test nb */
                    argument++;
                    testNb=0;
                    while ((*argument>='0') && (*argument<='9')) {
                        testNb *= 10;
                        testNb += (unsigned)(*argument - '0');
                        argument++;
                    }
                    break;

                case 'P':  /* change probability */
                    argument++;
                    proba=0;
                    while ((*argument>='0') && (*argument<='9')) {
                        proba *= 10;
                        proba += *argument - '0';
                        argument++;
                    }
                    if (proba<0) proba=0;
                    if (proba>100) proba=100;
                    break;
                default: ;
                }
            }
        }
    }

    printf("Starting LZ4 fuzzer (%i-bits, v%s)\n", (int)(sizeof(size_t)*8), LZ4_versionString());

    if (!seedset) {
        time_t const t = time(NULL);
        U32 const h = XXH32(&t, sizeof(t), 1);
        seed = h % 10000;
    }
    printf("Seed = %u\n", seed);

    if (proba!=FUZ_COMPRESSIBILITY_DEFAULT) printf("Compressibility : %i%%\n", proba);


    nbTests += (nbTests==0);  /* avoid zero */

    {   int const result = FUZ_test(seed, nbTests, testNb, ((double)proba) / 100, duration);
        if (use_pause) {
            DISPLAY("press enter ... \n");
            (void)getchar();
        }
        return result;
    }
}
