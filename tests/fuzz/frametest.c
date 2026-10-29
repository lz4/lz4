/*
    frameTest - test tool for lz4frame
    Copyright (C) Yann Collet 2014-2020

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
    - LZ4 source repository : https://github.com/lz4/lz4
*/

/*-************************************
*  Compiler specific
**************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  pragma warning(disable : 26451)     /* disable: Arithmetic overflow */
#  pragma warning(disable : 4127)    /* disable: C4127: conditional expression is constant */
#endif


/*-************************************
*  Includes
**************************************/
#include "util.h"       /* U32 */
#include <stdlib.h>     /* malloc, free */
#include <stdio.h>      /* fprintf */
#include <string.h>     /* strcmp */
#include <time.h>       /* clock_t, clock(), CLOCKS_PER_SEC */
#include <assert.h>
#include "lz4frame.h"   /* included multiple times to test correctness/safety */
#include "lz4frame.h"
#define LZ4F_STATIC_LINKING_ONLY
#include "lz4frame.h"
#include "lz4frame.h"
#include "lz4.h"        /* LZ4_VERSION_STRING */
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"     /* XXH64 */


/* unoptimized version; solves endianness & alignment issues */
static void FUZ_writeLE32 (void* dstVoidPtr, U32 value32)
{
    BYTE* const dstPtr = (BYTE*)dstVoidPtr;
    dstPtr[0] = (BYTE) value32;
    dstPtr[1] = (BYTE)(value32 >> 8);
    dstPtr[2] = (BYTE)(value32 >> 16);
    dstPtr[3] = (BYTE)(value32 >> 24);
}


/*-************************************
*  Constants
**************************************/
#define KB *(1U<<10)
#define MB *(1U<<20)
#define GB *(1U<<30)

static const U32 nbTestsDefault = 256 KB;
#define FUZ_COMPRESSIBILITY_DEFAULT 50
static const U32 prime1 = 2654435761U;
static const U32 prime2 = 2246822519U;

static const U64 extCrcSeed = 1;

/*-************************************
*  Macros
**************************************/
#define DISPLAY(...)          fprintf(stderr, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...)  do { if (displayLevel>=(l)) DISPLAY(__VA_ARGS__); } while (0)
#define DISPLAYUPDATE(l, ...) do { if (displayLevel>=(l)) { \
            if ((FUZ_GetClockSpan(g_clockTime) > refreshRate) || (displayLevel>=4)) \
            { g_clockTime = clock(); DISPLAY(__VA_ARGS__); \
            if (displayLevel>=4) fflush(stdout); } } } while (0)
static const clock_t refreshRate = CLOCKS_PER_SEC / 6;
static clock_t g_clockTime = 0;


/*-***************************************
*  Local Parameters
*****************************************/
static U32 displayLevel = 2;
static U32 use_pause = 0;


/*-*******************************************************
*  Fuzzer functions
*********************************************************/
#undef MIN
#define MIN(a,b)  ( (a) < (b) ? (a) : (b) )
#undef MAX
#define MAX(a,b)  ( (a) > (b) ? (a) : (b) )

static clock_t FUZ_GetClockSpan(clock_t clockStart)
{
    return clock() - clockStart;   /* works even if overflow; max span ~ 30 mn */
}

#define FUZ_rotl32(x,r) ((x << r) | (x >> (32 - r)))
unsigned int FUZ_rand(unsigned int* src)
{
    U32 rand32 = *src;
    rand32 *= prime1;
    rand32 += prime2;
    rand32  = FUZ_rotl32(rand32, 13);
    *src = rand32;
    return rand32 >> 5;
}

#define RAND_BITS(N) (FUZ_rand(randState) & ((1 << (N))-1))
#define FUZ_LITERAL (RAND_BITS(6) + '0')
#define FUZ_ABOUT(_R) ((FUZ_rand(randState) % (_R)) + (FUZ_rand(randState) % (_R)) + 1)
static void FUZ_fillCompressibleNoiseBuffer(void* buffer, size_t bufferSize, double proba, U32* randState)
{
    BYTE* BBuffer = (BYTE*)buffer;
    size_t pos = 0;
    U32 P32 = (U32)(32768 * proba);

    /* First Byte */
    BBuffer[pos++] = FUZ_LITERAL;

    while (pos < bufferSize) {
        /* Select : Literal (noise) or copy (within 64K) */
        if (RAND_BITS(15) < P32) {
            /* Copy (within 64K) */
            size_t const lengthRand = FUZ_ABOUT(8) + 4;
            size_t const length = MIN(lengthRand, bufferSize - pos);
            size_t const end = pos + length;
            size_t const offsetRand = RAND_BITS(15) + 1;
            size_t const offset = MIN(offsetRand, pos);
            size_t match = pos - offset;
            while (pos < end) BBuffer[pos++] = BBuffer[match++];
        } else {
            /* Literal (noise) */
            size_t const lengthRand = FUZ_ABOUT(4);
            size_t const length = MIN(lengthRand, bufferSize - pos);
            size_t const end = pos + length;
            while (pos < end) BBuffer[pos++] = FUZ_LITERAL;
    }   }
}

static unsigned FUZ_highbit(U32 v32)
{
    unsigned nbBits = 0;
    if (v32==0) return 0;
    while (v32) {v32 >>= 1; nbBits ++;}
    return nbBits;
}


typedef enum { o_contiguous, o_noncontiguous, o_overwrite } o_scenario_e;

static void locateBuffDiff(const void* buff1, const void* buff2, size_t size, o_scenario_e o_scenario)
{
    if (displayLevel >= 2) {
        size_t p=0;
        const BYTE* b1=(const BYTE*)buff1;
        const BYTE* b2=(const BYTE*)buff2;
        DISPLAY("locateBuffDiff: looking for error position \n");
        if (o_scenario != o_contiguous) {
            DISPLAY("mode %i: non-contiguous output (%u bytes), cannot search \n",
                    (int)o_scenario, (unsigned)size);
            return;
        }
        while (p < size && b1[p]==b2[p]) p++;
        if (p != size) {
            DISPLAY("Error at pos %i/%i : %02X != %02X \n", (int)p, (int)size, b1[p], b2[p]);
        }
    }
}

#define EXIT_MSG(...) { DISPLAY("Error => "); DISPLAY(__VA_ARGS__); \
                        DISPLAY(" (seed %u, test nb %u)  \n", seed, testNb); exit(1); }
#undef CHECK
#define CHECK(cond, ...) { if (cond) { EXIT_MSG(__VA_ARGS__); } }

static LZ4F_errorCode_t LZ4F_returnErrorCode(LZ4F_errorCodes code)
{
    return (LZ4F_errorCode_t)-(ptrdiff_t)code;
}

#define RETURN_ERROR(e) return LZ4F_returnErrorCode(LZ4F_ERROR_ ## e)


/* @o_scenario: output behavior: contiguous, non-contiguous, or overwrite
 * @crcOrig: xxh64 of original data using extCrcSeed
 * @return : 0 on success, or LZ4F_ErrorCode
 * */
size_t test_lz4f_decompression_wBuffers(
          const void* cSrc, size_t cSize,
                void* dst, size_t dstCapacity,
                o_scenario_e o_scenario,
          const void* srcRef, size_t decompressedSize,
                U64 crcOrig,
                U32* const randState,
                LZ4F_dctx* const dCtx,
                U32 seed, U32 testNb,
                int shouldSucceed)
{
    const BYTE* ip = (const BYTE*)cSrc;
    const BYTE* const iend = ip + cSize;

    BYTE* op = (BYTE*)dst;
    BYTE* const oend = op + dstCapacity;

    unsigned const suggestedBits = FUZ_highbit((U32)cSize);
    unsigned const maxBits = MAX(3, suggestedBits);
    size_t totalOut = 0;
    size_t moreToFlush = 0;
    XXH64_state_t xxh64;
    XXH64_reset(&xxh64, extCrcSeed);
    o_scenario = o_contiguous;
    assert(ip < iend);
    while (ip < iend) {
        unsigned const nbBitsI = (FUZ_rand(randState) % (maxBits-1)) + 1;
        unsigned const nbBitsO = (FUZ_rand(randState) % (maxBits)) + 1;
        size_t const iSizeCand = RAND_BITS(nbBitsI) + 1;
        size_t const iSizeMax = MIN(iSizeCand, (size_t)(iend-ip));
        size_t iSize = iSizeMax;
        size_t const oSizeCand = RAND_BITS(nbBitsO) + 2;
        size_t const oSizeMax = MIN(oSizeCand, (size_t)(oend-op));
        int const sentinelTest = (op + oSizeMax < oend);
        size_t oSize = oSizeMax;
        BYTE const mark = (BYTE)(RAND_BITS(8));
        LZ4F_decompressOptions_t dOptions;
        memset(&dOptions, 0, sizeof(dOptions));
        dOptions.stableDst = RAND_BITS(1);
        if (o_scenario == o_overwrite) dOptions.stableDst = 0;  /* overwrite mode */
        dOptions.skipChecksums = RAND_BITS(1);
        if (sentinelTest) op[oSizeMax] = mark;

        DISPLAYLEVEL(7, "dstCapacity=%u,  presentedInput=%u \n", (unsigned)oSize, (unsigned)iSize);

        /* read data from byte-exact buffer to catch out-of-bound reads */
        {   void* const iBuffer = malloc(iSizeMax);
            /*test NULL supported if size==0 */
            void* const tmpop = RAND_BITS(oSize == 0) ? NULL : op;
            const void* const tmpip = RAND_BITS(iSize == 0) ? NULL : iBuffer;
            assert(iBuffer != NULL);
            memcpy(iBuffer, ip, iSizeMax);
            moreToFlush = LZ4F_decompress(dCtx, tmpop, &oSize, tmpip, &iSize, &dOptions);
            free(iBuffer);
        }
        DISPLAYLEVEL(7, "oSize=%u, readSize=%u \n", (unsigned)oSize, (unsigned)iSize);

        if (sentinelTest) {
            CHECK(op[oSizeMax] != mark, "op[oSizeMax] = %02X != %02X : "
                    "Decompression overwrites beyond assigned dst size",
                    op[oSizeMax], mark);
        }
        if (LZ4F_getErrorCode(moreToFlush) == LZ4F_ERROR_contentChecksum_invalid) {
            if (shouldSucceed) DISPLAYLEVEL(1, "checksum error reported after decompressing segment with LZ4F_decompress() \n");
            if (shouldSucceed) locateBuffDiff(srcRef, dst, decompressedSize, o_scenario);
            RETURN_ERROR(contentChecksum_invalid);
        }
        if (LZ4F_isError(moreToFlush)) {
            if (shouldSucceed) DISPLAYLEVEL(1, "error reported after decompressing segment with LZ4F_decompress() \n");
            return moreToFlush;
        }

        XXH64_update(&xxh64, op, oSize);
        totalOut += oSize;
        op += oSize;
        ip += iSize;
        if (o_scenario == o_noncontiguous) {
            if (op == oend) RETURN_ERROR(GENERIC);  /* can theoretically happen with bogus data */
            op++; /* create a gap between consecutive output */
        }
        if (o_scenario==o_overwrite) op = (BYTE*)dst;   /* overwrite destination */
        if ( (op == oend) /* no more room for output; can happen with bogus input */
          && (iSize == 0)) /* no input consumed */
            break;
    }
    if (moreToFlush != 0) RETURN_ERROR(decompressionFailed);
    if (totalOut) {  /* otherwise, it's a skippable frame */
        U64 const crcDecoded = XXH64_digest(&xxh64);
        if (crcDecoded != crcOrig) {
            if (shouldSucceed) DISPLAYLEVEL(1, "checksum error detected at end of decompression \n");
            if (shouldSucceed) locateBuffDiff(srcRef, dst, decompressedSize, o_scenario);
            RETURN_ERROR(contentChecksum_invalid);
    }   }
    return 0;
}


/* @return : 0 on success, or LZ4F_ErrorCode */
size_t test_lz4f_decompression(const void* cSrc, size_t cSize,
                               const void* srcRef, size_t decompressedSize,
                               U64 crcOrig,
                               U32* const randState,
                               LZ4F_dctx* const dCtx,
                               U32 seed, U32 testNb,
                               int findErrorPos)
{
    o_scenario_e const o_scenario = (o_scenario_e)(FUZ_rand(randState) % 3);   /* 0 : contiguous; 1 : non-contiguous; 2 : dst overwritten */
    /* tighten dst buffer conditions */
    size_t const dstCapacity = (o_scenario == o_noncontiguous) ?
                               (decompressedSize * 2) + 128 : /* should provide enough room to insert spaces beween blocks */
                               decompressedSize;
    size_t result;
    void* const dstBuffer = malloc(dstCapacity);
    assert(dstBuffer != NULL);

    result = test_lz4f_decompression_wBuffers(cSrc, cSize,
                                     dstBuffer, dstCapacity, o_scenario,
                                     srcRef, decompressedSize,
                                     crcOrig,
                                     randState,
                                     dCtx,
                                     seed, testNb, findErrorPos);

    free(dstBuffer);
    return result;
}


int fuzzerTests(U32 seed, unsigned nbTests, unsigned startTest, double compressibility, U32 duration_s)
{
    unsigned testNb = 0;
    size_t const CNBufferLength = 9 MB;  /* needs to be > 2x4MB to test large blocks */
    void* CNBuffer = NULL;
    size_t const compressedBufferSize = LZ4F_compressFrameBound(CNBufferLength, NULL) + 4 MB;  /* needs some margin */
    void* compressedBuffer = NULL;
    void* decodedBuffer = NULL;
    U32 coreRand = seed;
    LZ4F_decompressionContext_t dCtx = NULL;
    LZ4F_compressionContext_t cCtx = NULL;
    clock_t const startClock = clock();
    clock_t const clockDuration = duration_s * CLOCKS_PER_SEC;

    /* Create states & buffers */
    {   size_t const creationStatus = LZ4F_createDecompressionContext(&dCtx, LZ4F_VERSION);
        CHECK(LZ4F_isError(creationStatus), "Allocation failed (error %i)", (int)creationStatus); }
    {   size_t const creationStatus = LZ4F_createCompressionContext(&cCtx, LZ4F_VERSION);
        CHECK(LZ4F_isError(creationStatus), "Allocation failed (error %i)", (int)creationStatus); }
    CNBuffer = malloc(CNBufferLength);
    CHECK(CNBuffer==NULL, "CNBuffer Allocation failed");
    compressedBuffer = malloc(compressedBufferSize);
    CHECK(compressedBuffer==NULL, "compressedBuffer Allocation failed");
    decodedBuffer = calloc(1, CNBufferLength);   /* calloc avoids decodedBuffer being considered "garbage" by scan-build */
    CHECK(decodedBuffer==NULL, "decodedBuffer Allocation failed");
    FUZ_fillCompressibleNoiseBuffer(CNBuffer, CNBufferLength, compressibility, &coreRand);

    /* jump to requested testNb */
    for (testNb =0; (testNb < startTest); testNb++) (void)FUZ_rand(&coreRand);   /* sync randomizer */

    /* main fuzzer test loop */
    for ( ; (testNb < nbTests) || (clockDuration > FUZ_GetClockSpan(startClock)) ; testNb++) {
        U32 randState = coreRand ^ prime1;
        unsigned const srcBits = (FUZ_rand(&randState) % (FUZ_highbit((U32)(CNBufferLength-1)) - 1)) + 1;
        size_t const srcSize = (FUZ_rand(&randState) & ((1<<srcBits)-1));
        size_t const srcStartId = FUZ_rand(&randState) % (CNBufferLength - srcSize);
        const BYTE* const srcStart = (const BYTE*)CNBuffer + srcStartId;
        unsigned const neverFlush = (FUZ_rand(&randState) & 15) == 1;
        U64 const crcOrig = XXH64(srcStart, srcSize, extCrcSeed);
        LZ4F_preferences_t prefs;
        const LZ4F_preferences_t* prefsPtr = &prefs;
        size_t cSize;

        (void)FUZ_rand(&coreRand);   /* update seed */
        memset(&prefs, 0, sizeof(prefs));
        prefs.frameInfo.blockMode = (LZ4F_blockMode_t)(FUZ_rand(&randState) & 1);
        prefs.frameInfo.blockSizeID = (LZ4F_blockSizeID_t)(4 + (FUZ_rand(&randState) & 3));
        prefs.frameInfo.blockChecksumFlag = (LZ4F_blockChecksum_t)(FUZ_rand(&randState) & 1);
        prefs.frameInfo.contentChecksumFlag = (LZ4F_contentChecksum_t)(FUZ_rand(&randState) & 1);
        prefs.frameInfo.contentSize = ((FUZ_rand(&randState) & 0xF) == 1) ? srcSize : 0;
        prefs.autoFlush = neverFlush ? 0 : (FUZ_rand(&randState) & 7) == 2;
        prefs.compressionLevel = -5 + (int)(FUZ_rand(&randState) % 11);
        if ((FUZ_rand(&randState) & 0xF) == 1) prefsPtr = NULL;

        DISPLAYUPDATE(2, "\r%5u   ", testNb);

        if ((FUZ_rand(&randState) & 0xFFF) == 0) {
            /* create a skippable frame (rare case) */
            BYTE* op = (BYTE*)compressedBuffer;
            FUZ_writeLE32(op, LZ4F_MAGIC_SKIPPABLE_START + (FUZ_rand(&randState) & 15));
            FUZ_writeLE32(op+4, (U32)srcSize);
            cSize = srcSize+8;

        } else if ((FUZ_rand(&randState) & 0xF) == 2) {  /* single pass compression (simple) */
            cSize = LZ4F_compressFrame(compressedBuffer, LZ4F_compressFrameBound(srcSize, prefsPtr), srcStart, srcSize, prefsPtr);
            CHECK(LZ4F_isError(cSize), "LZ4F_compressFrame failed : error %i (%s)", (int)cSize, LZ4F_getErrorName(cSize));

            if ((FUZ_rand(&randState) & 3) == 1) {
                /* any capacity : must succeed if and only if the frame fits, producing the same frame */
                size_t const bound = LZ4F_compressFrameBound(srcSize, prefsPtr);
                size_t const delta = FUZ_rand(&randState) % 16;
                size_t dstCapacity;
                switch (FUZ_rand(&randState) & 3) {
                case 0: dstCapacity = FUZ_rand(&randState) % (bound + 1); break;
                case 1: dstCapacity = cSize - 1 - MIN(delta, cSize - 1); break;
                case 2: dstCapacity = MIN(cSize + delta, bound); break;
                default: dstCapacity = cSize; break;
                }
                {   BYTE* const dst = (BYTE*)malloc(dstCapacity + 1);
                    BYTE const canaryByte = (BYTE)FUZ_rand(&randState);
                    size_t r;
                    CHECK(dst == NULL, "allocation failed");
                    dst[dstCapacity] = canaryByte;
                    r = LZ4F_compressFrame(dst, dstCapacity, srcStart, srcSize, prefsPtr);
                    CHECK(dst[dstCapacity] != canaryByte, "LZ4F_compressFrame writes beyond dstCapacity");
                    if (dstCapacity >= cSize) {
                        CHECK(r != cSize, "LZ4F_compressFrame into %u bytes (frame size %u) : %s",
                                (unsigned)dstCapacity, (unsigned)cSize,
                                LZ4F_isError(r) ? LZ4F_getErrorName(r) : "different frame size");
                        CHECK(memcmp(dst, compressedBuffer, cSize),
                                "LZ4F_compressFrame into %u bytes : different frame content", (unsigned)dstCapacity);
                    } else {
                        CHECK(LZ4F_getErrorCode(r) != LZ4F_ERROR_dstCapacity_tooSmall,
                                "LZ4F_compressFrame into %u bytes (frame size %u) : %s",
                                (unsigned)dstCapacity, (unsigned)cSize,
                                LZ4F_isError(r) ? LZ4F_getErrorName(r) : "should have failed");
                    }
                    free(dst);
            }   }

        } else {   /* multi-segments compression */
            const BYTE* ip = srcStart;
            const BYTE* const iend = srcStart + srcSize;
            BYTE* op = (BYTE*)compressedBuffer;
            BYTE* const oend = op + (neverFlush ? LZ4F_compressFrameBound(srcSize, prefsPtr) : compressedBufferSize);  /* when flushes are possible, can't guarantee a max compressed size */
            unsigned const maxBits = FUZ_highbit((U32)srcSize);
            LZ4F_compressOptions_t cOptions;
            int frameAbandoned = 0;
            memset(&cOptions, 0, sizeof(cOptions));
            {   size_t const fhSize = LZ4F_compressBegin(cCtx, op, (size_t)(oend-op), prefsPtr);
                CHECK(LZ4F_isError(fhSize), "Compression header failed (error %i)",
                                            (int)fhSize);
                op += fhSize;
            }
            while (ip < iend) {
                unsigned const nbBitsSeg = FUZ_rand(&randState) % maxBits;
                size_t sampleMax = (FUZ_rand(&randState) & ((1<<nbBitsSeg)-1)) + 1;
                unsigned const uncompressed = !neverFlush && prefsPtr != NULL
                    && (FUZ_rand(&randState) & 15) == 0;
                /* Regularly cross block boundaries after buffering a partial block.
                 * Choosing each segment's mode independently exercises both switches. */
                if (!neverFlush && prefsPtr != NULL
                  && (FUZ_rand(&randState) & 3) == 0) {
                    size_t const blockSize = LZ4F_getBlockSize(prefs.frameInfo.blockSizeID);
                    unsigned const edge = FUZ_rand(&randState) & 3;
                    sampleMax = edge == 3 ? 2 * blockSize : blockSize + edge - 1;
                }
                cOptions.stableSrc = ((FUZ_rand(&randState) & 3) == 1);
                cOptions.uncompressed = uncompressed;

                {   size_t const iSize = MIN(sampleMax, (size_t)(iend-ip));
                    size_t const oSize = LZ4F_compressBound(iSize, prefsPtr);
                    /* occasionally provide less than the bound : the update may then fail */
                    unsigned const smallDst = (FUZ_rand(&randState) & 31) == 7;
                    size_t const dstCapacity = smallDst ? FUZ_rand(&randState) % (oSize + 1) : oSize;
                    BYTE* const dst = (BYTE*)malloc(dstCapacity + 1);
                    BYTE const canaryByte = (BYTE)FUZ_rand(&randState);
                    size_t flushedSize;
                    CHECK(dst == NULL, "Update output allocation failed");
                    dst[dstCapacity] = canaryByte;
                    DISPLAYLEVEL(6, "Sending %u bytes (uncompressed:%u, stableSrc:%u, capacity:%u) \n",
                                    (unsigned)iSize, uncompressed, cOptions.stableSrc, (unsigned)dstCapacity);
                    flushedSize = LZ4F_compressUpdate(cCtx, dst, dstCapacity, ip, iSize, &cOptions);
                    CHECK(dst[dstCapacity] != canaryByte, "Update writes beyond dstCapacity (uncompressed:%u)", uncompressed);
                    if (smallDst && LZ4F_isError(flushedSize)) {
                        CHECK(LZ4F_getErrorCode(flushedSize) != LZ4F_ERROR_dstCapacity_tooSmall,
                                "Update into %u bytes (bound %u) : unexpected error %s",
                                (unsigned)dstCapacity, (unsigned)oSize, LZ4F_getErrorName(flushedSize));
                        free(dst);
                        frameAbandoned = 1;
                        break;
                    }
                    CHECK(LZ4F_isError(flushedSize), "Compression failed (error %i : %s)",
                            (int)flushedSize, LZ4F_getErrorName(flushedSize));
                    CHECK(flushedSize > dstCapacity, "Update exceeds compressBound (uncompressed:%u)", uncompressed);
                    CHECK(flushedSize > (size_t)(oend-op), "Compression output buffer too small");
                    memcpy(op, dst, flushedSize);
                    free(dst);
                    op += flushedSize;
                    ip += iSize;
                }

                {   unsigned const forceFlush = neverFlush ? 0 : ((FUZ_rand(&randState) & 3) == 1);
                    if (forceFlush) {
                        /* occasionally provide less than the bound : the flush then fills dstBuffer,
                         * and output remains, emitted by next invocations of LZ4F_flush() or LZ4F_compressUpdate() */
                        unsigned const smallDst = (FUZ_rand(&randState) & 31) == 7;
                        size_t const smallCapacity = FUZ_rand(&randState) % (LZ4F_compressBound(0, prefsPtr) + 1);
                        size_t flushCapacity = smallDst
                            ? MIN(smallCapacity, (size_t)(oend-op) - 1)
                            : (size_t)(oend-op);
                        BYTE const canaryByte = (BYTE)FUZ_rand(&randState);
                        size_t flushSize;
                        int flushCompleted;
                        if (smallDst) op[flushCapacity] = canaryByte;
                        flushSize = LZ4F_flush(cCtx, op, flushCapacity, &cOptions);
                        DISPLAYLEVEL(6, "flushing %u bytes \n", (unsigned)flushSize);
                        if (smallDst) {
                            CHECK(op[flushCapacity] != canaryByte, "LZ4F_flush writes beyond dstCapacity");
                            if (LZ4F_isError(flushSize)) {
                                CHECK(LZ4F_getErrorCode(flushSize) != LZ4F_ERROR_dstCapacity_tooSmall,
                                        "Flush into %u bytes : unexpected error %s",
                                        (unsigned)flushCapacity, LZ4F_getErrorName(flushSize));
                                CHECK(flushCapacity != 0, "Flush into %u bytes must not fail", (unsigned)flushCapacity);
                                flushSize = 0;   /* 0 byte : rejected, the frame continues */
                        }   }
                        CHECK(LZ4F_isError(flushSize), "Compression failed (error %i)", (int)flushSize);
                        op += flushSize;
                        flushCompleted = (flushSize < flushCapacity);
                        if (!flushCompleted && (FUZ_rand(&randState) & 1)) {
                            /* complete the flush ; otherwise, next update emits remaining output first */
                            do {
                                flushCapacity = (size_t)(oend-op);
                                flushSize = LZ4F_flush(cCtx, op, flushCapacity, &cOptions);
                                CHECK(LZ4F_isError(flushSize), "Flush continuation failed (%s)", LZ4F_getErrorName(flushSize));
                                op += flushSize;
                            } while (flushSize == flushCapacity);
                            flushCompleted = 1;
                        }
                        if (flushCompleted && ((FUZ_rand(&randState) % 1024) == 3)) {
                            /* add an empty block (requires uncompressed flag) */
                            op[0] = op[1] = op[2] = 0;
                            op[3] = 0x80; /* 0x80000000U in little-endian format */
                            op += 4;
                            if ((prefsPtr!= NULL) && prefsPtr->frameInfo.blockChecksumFlag) {
                                /* add block checksum (even for empty blocks) */
                                FUZ_writeLE32(op, XXH32(op, 0, 0));
                                op += 4;
                }   }   }   }
            }  /* while (ip<iend) */

            if (frameAbandoned) {
                /* after a failed operation, the frame can't be continued */
                size_t const r = LZ4F_compressEnd(cCtx, op, (size_t)(oend-op), &cOptions);
                CHECK(LZ4F_getErrorCode(r) != LZ4F_ERROR_compressionState_uninitialized,
                        "LZ4F_compressEnd after a failed operation : %s",
                        LZ4F_isError(r) ? LZ4F_getErrorName(r) : "should have failed");
                continue;
            }

            /* check compressBound guarantees */
            if (neverFlush) CHECK(op>=oend, "LZ4F_compressFrameBound overflow");

            {   size_t const dstEndSafeSize = LZ4F_compressBound(0, prefsPtr);
                int const tooSmallDstEnd = ((FUZ_rand(&randState) & 31) == 3);
                size_t const dstEndTooSmallSize = (FUZ_rand(&randState) % dstEndSafeSize) + 1;
                size_t const dstEndSize = tooSmallDstEnd ? dstEndTooSmallSize : dstEndSafeSize;
                BYTE const canaryByte = (BYTE)(FUZ_rand(&randState) & 255);
                size_t flushedSize;
                size_t capacity = dstEndSize;
                DISPLAYLEVEL(7,"canaryByte at pos %u / %u \n",
                            (unsigned)((size_t)(op - (BYTE*)compressedBuffer) + dstEndSize),
                            (unsigned)compressedBufferSize);
                (void)tooSmallDstEnd;
                assert(op + dstEndSize < (BYTE*)compressedBuffer + compressedBufferSize);
                for (;;) {
                    /* LZ4F_compressEnd() fills dstBuffer, or completes the frame */
                    op[capacity] = canaryByte;
                    flushedSize = LZ4F_compressEnd(cCtx, op, capacity, &cOptions);
                    CHECK(op[capacity] != canaryByte, "LZ4F_compressEnd writes beyond dstCapacity !");
                    CHECK(LZ4F_isError(flushedSize), "Compression completion into %u bytes failed (error %i : %s)",
                            (unsigned)capacity, (int)flushedSize, LZ4F_getErrorName(flushedSize));
                    op += flushedSize;
                    if (flushedSize < capacity) break;
                    capacity = (FUZ_rand(&randState) % dstEndSafeSize) + 1;
                    assert(op + capacity < (BYTE*)compressedBuffer + compressedBufferSize);
                }
            }
            cSize = (size_t)(op - (BYTE*)compressedBuffer);
            DISPLAYLEVEL(5, "\nCompressed %u bytes into %u \n", (U32)srcSize, (U32)cSize);
        }


        /* multi-segments decompression */
        DISPLAYLEVEL(6, "normal decompression \n");
        {   size_t result = test_lz4f_decompression(compressedBuffer, cSize, srcStart, srcSize, crcOrig, &randState, dCtx, seed, testNb, 1 /*findError*/ );
            CHECK (LZ4F_isError(result), "multi-segment decompression failed (error %i => %s)",
                                        (int)result, LZ4F_getErrorName(result));
        }

        /* insert noise into src - ensure decoder survives with no sanitizer error */
        {   U32 const maxNbBits = FUZ_highbit((U32)cSize);
            size_t pos = 0;
            for (;;) {
                /* keep some original src */
                {   U32 const nbBits = FUZ_rand(&randState) % maxNbBits;
                    size_t const mask = (1<<nbBits) - 1;
                    size_t const skipLength = FUZ_rand(&randState) & mask;
                    pos += skipLength;
                }
                if (pos >= cSize) break;
                /* add noise */
                {   U32 const nbBitsCodes = FUZ_rand(&randState) % maxNbBits;
                    U32 const nbBits = nbBitsCodes ? nbBitsCodes-1 : 0;
                    size_t const mask = (1<<nbBits) - 1;
                    size_t const rNoiseLength = (FUZ_rand(&randState) & mask) + 1;
                    size_t const noiseLength = MIN(rNoiseLength, cSize-pos);
                    size_t const noiseStart = FUZ_rand(&randState) % (CNBufferLength - noiseLength);
                    memcpy((BYTE*)compressedBuffer + pos, (const char*)CNBuffer + noiseStart, noiseLength);
                    pos += noiseLength;
        }   }   }

        /* test decompression on noisy src */
        DISPLAYLEVEL(6, "noisy decompression \n");
        test_lz4f_decompression(compressedBuffer, cSize, srcStart, srcSize, crcOrig, &randState, dCtx, seed, testNb, 0 /*don't search error Pos*/ );
        /* note : we don't analyze result here : it probably failed, which is expected.
         * The sole purpose is to catch potential out-of-bound reads and writes. */
        LZ4F_resetDecompressionContext(dCtx);  /* state must be reset to clean after an error */

    }   /* for ( ; (testNb < nbTests) ; ) */

    DISPLAYLEVEL(2, "\rAll tests completed   \n");

    LZ4F_freeDecompressionContext(dCtx);
    LZ4F_freeCompressionContext(cCtx);
    free(CNBuffer);
    free(compressedBuffer);
    free(decodedBuffer);

    if (use_pause) {
        DISPLAY("press enter to finish \n");
        (void)getchar();
    }
    return 0;
}


int FUZ_usage(const char* programName)
{
    DISPLAY( "Usage :\n");
    DISPLAY( "      %s [args]\n", programName);
    DISPLAY( "\n");
    DISPLAY( "Arguments :\n");
    DISPLAY( " -i#    : Nb of tests (default:%u) \n", nbTestsDefault);
    DISPLAY( " -T#    : Duration of tests, in seconds (default: use Nb of tests) \n");
    DISPLAY( " -s#    : Select seed (default:prompt user)\n");
    DISPLAY( " -t#    : Select starting test number (default:0)\n");
    DISPLAY( " -P#    : Select compressibility in %% (default:%i%%)\n", FUZ_COMPRESSIBILITY_DEFAULT);
    DISPLAY( " -v     : verbose\n");
    DISPLAY( " -h     : display help and exit\n");
    return 0;
}


int main(int argc, const char** argv)
{
    U32 seed=0;
    int seedset=0;
    int argNb;
    unsigned nbTests = nbTestsDefault;
    unsigned testNb = 0;
    int proba = FUZ_COMPRESSIBILITY_DEFAULT;
    U32 duration=0;
    const char* const programName = argv[0];

    /* Check command line */
    for (argNb=1; argNb<argc; argNb++) {
        const char* argument = argv[argNb];

        if(!argument) continue;   /* Protection if argument empty */

        /* Decode command (note : aggregated short commands are allowed) */
        if (argument[0]=='-') {
            if (!strcmp(argument, "--no-prompt")) {
                seedset=1;
                displayLevel=1;
                continue;
            }
            argument++;

            while (*argument!=0) {
                switch(*argument)
                {
                case 'h':
                    return FUZ_usage(programName);
                case 'v':
                    argument++;
                    displayLevel++;
                    break;
                case 'q':
                    argument++;
                    displayLevel--;
                    break;
                case 'p': /* pause at the end */
                    argument++;
                    use_pause = 1;
                    break;

                case 'i':
                    argument++;
                    nbTests=0; duration=0;
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
                    seed=0;
                    seedset=1;
                    while ((*argument>='0') && (*argument<='9')) {
                        seed *= 10;
                        seed += (U32)(*argument - '0');
                        argument++;
                    }
                    break;
                case 't':
                    argument++;
                    testNb=0;
                    while ((*argument>='0') && (*argument<='9')) {
                        testNb *= 10;
                        testNb += (unsigned)(*argument - '0');
                        argument++;
                    }
                    break;
                case 'P':   /* compressibility % */
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
                default:
                    ;
                    return FUZ_usage(programName);
                }
            }
        } /* if (argument[0]=='-') */
    } /* for (argNb=1; argNb<argc; argNb++) */

    DISPLAY("Starting lz4frame tester (%i-bits, %s) \n", (int)(sizeof(size_t)*8), LZ4_VERSION_STRING);

    /* Select a random seed if none given */
    if (!seedset) {
        time_t const t = time(NULL);
        U32 const h = XXH32(&t, sizeof(t), 1);
        seed = h % 10000;
    }
    DISPLAY("Seed = %u \n", seed);
    if (proba != FUZ_COMPRESSIBILITY_DEFAULT)
        DISPLAY("Compressibility : %i%% \n", proba);

    {   double const compressibility = (double)proba / 100;
        nbTests += (nbTests==0);  /* avoid zero */
        return fuzzerTests(seed, nbTests, testNb, compressibility, duration);
    }
}
