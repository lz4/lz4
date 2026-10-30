/*
    unittest_lz4 - unit tests of lz4 (block format, fast levels)
    Copyright (C) Yann Collet 2026

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

#ifdef _MSC_VER    /* Visual Studio */
#  pragma warning(disable : 4127)    /* disable: C4127: conditional expression is constant */
#endif

#include "unittest.h"
#include <limits.h>     /* INT_MIN, INT_MAX */
#include <stdint.h>     /* uintptr_t */
#include <stdlib.h>     /* malloc, calloc, free */
#include <string.h>     /* memset, memcpy */
#ifndef LZ4_DISABLE_DEPRECATE_WARNINGS
#  define LZ4_DISABLE_DEPRECATE_WARNINGS   /* LZ4_decompress_fast_continue */
#endif
#define LZ4_STATIC_LINKING_ONLY   /* LZ4_compress_destSize_extState, LZ4_attach_dictionary */
#include "lz4.h"
#include "lz4hc.h"


/*-************************************
*  Helpers
**************************************/
static void freeStream(void* stream) { LZ4_freeStream((LZ4_stream_t*)stream); }
static void freeStreamHC(void* stream) { LZ4_freeStreamHC((LZ4_streamHC_t*)stream); }

/* initialized stream, released at the end of the test */
static LZ4_stream_t* newStream(void)
{
    LZ4_stream_t* const stream = (LZ4_stream_t*)UT_defer(freeStream, LZ4_createStream());
    ASSERT(stream != NULL);
    return stream;
}

/* 64 KB block, beyond the table clearing threshold whatever LZ4_MEMORY_USAGE,
 * in which the first 4 bytes reappear with a different 5th byte :
 * a match only found when starting from a cleared table */
#define BLOCK_64K (64 KB)
static char* clearedTableBlock(void)
{
    char* const block = (char*)UT_sample(BLOCK_64K);
    memcpy(block, "ABCDx", 5);
    memcpy(block + 16, "ABCDy", 5);
    return block;
}

/* large enough to enter any decoding loop */
#define DECODED_CAPACITY (64 KB)


/*-************************************
*  Decoding of invalid input
**************************************/
TEST(decompress_safe_emptyInput)
{
    char const src[1] = { 0 };
    char* const decoded = (char*)UT_malloc(DECODED_CAPACITY);
    ASSERT(LZ4_decompress_safe(src, decoded, 0, DECODED_CAPACITY) < 0);
}

TEST(decompress_safe_oneByteInput)
{
    unsigned char const src[1] = { 0xFF };
    char* const decoded = (char*)UT_malloc(DECODED_CAPACITY);
    ASSERT(LZ4_decompress_safe((const char*)src, decoded, 1, DECODED_CAPACITY) < 0);
}

TEST(decompress_safe_nullInput)
{
    char* const decoded = (char*)UT_malloc(DECODED_CAPACITY);
    ASSERT(LZ4_decompress_safe(NULL, decoded, 1, DECODED_CAPACITY) < 0);
}

/* 14 bytes of literals, followed by a 14 byte match :
 * must not read beyond the end of the input (#508) */
TEST(decompress_safe_shortcutEdgeCase)
{
    unsigned char block[17];
    char* const decoded = (char*)UT_malloc(DECODED_CAPACITY);
    block[0] = 0xEE;
    memset(block + 1, 0, 14);
    block[15] = 14;
    block[16] = 0;
    ASSERT(LZ4_decompress_safe((const char*)block, decoded, sizeof(block), DECODED_CAPACITY) < 0);
}

/* out-of-bounds read regression test */
TEST(decompress_safe_all0xFF)
{
    char block[31];
    char* const decoded = (char*)UT_malloc(DECODED_CAPACITY);
    memset(block, 0xFF, sizeof(block));
    ASSERT(LZ4_decompress_safe(block, decoded, sizeof(block), DECODED_CAPACITY) < 0);
}

/* ip must not go beyond iend.
 * The block has 14 literals, ML_MASK match token, offset=14, one extension byte=0,
 * which positions ip exactly at iend when read_variable_length() reads the extension.
 * - 65 bytes output : large enough to enter the fast loop (>=64),
 *   small enough that op+length >= oend-64 triggers the safe_match_copy path,
 *   which would loop back with ip==iend, triggering assert(ip<iend).
 * - 40 bytes output : small enough to skip the fast loop (<64), large enough for the shortcut (>=32),
 *   which reads literals+offset, landing ip at iend-1, then goto _copy_match
 *   calls read_variable_length() with ip >= ilimit. */
TEST(decompress_safe_ipAtIend)
{
    static const int dstCapacities[] = { 65, 40 };
    unsigned char block[18];
    char decoded[65];
    size_t n;
    block[0] = 0xEF;                /* 14 literals, ML_MASK match */
    memset(block + 1, 'A', 14);     /* 14 literal bytes */
    block[15] = 0x0E; block[16] = 0x00;  /* offset = 14 */
    block[17] = 0x00;               /* match length extension = 0 */
    for (n = 0; n < ARRAY_SIZE(dstCapacities); n++)
        ASSERT(LZ4_decompress_safe((const char*)block, decoded, sizeof(block), dstCapacities[n]) < 0);
}

/* error codes : -1 malformed input, -2 decoded data larger than dstCapacity,
 * -3 reference before the start of output (missing dictionary).
 * Fixed content : the block must reference the dictionary */
TEST(decompress_safe_errorCodes)
{
    int const srcSize = 8 KB;
    const char* const sample = (const char*)UT_sample((size_t)srcSize);
    char* const src = (char*)UT_fixedNoise((size_t)srcSize);
    const char* const dict = (const char*)UT_fixedSample(64 KB);
    int const capacity = LZ4_COMPRESSBOUND(srcSize);
    char* const cBlock = (char*)UT_malloc((size_t)capacity);
    char* const decoded = (char*)UT_malloc((size_t)srcSize);
    LZ4_stream_t* const stream = newStream();
    int cSize, dstCapacity, pos;

    /* without dictionary : buffer too small, truncated input */
    cSize = LZ4_compress_default(sample, cBlock, srcSize, capacity);
    ASSERT(cSize > 0);
    for (dstCapacity = 0; dstCapacity < srcSize; dstCapacity += 61)
        ASSERT_EQ(LZ4_decompress_safe(cBlock, decoded, cSize, dstCapacity), -2);
    ASSERT_EQ(LZ4_decompress_safe(cBlock, decoded, cSize, srcSize - 1), -2);
    ASSERT_EQ(LZ4_decompress_safe(cBlock, decoded, cSize - 1, srcSize), -1);
    ASSERT_EQ(LZ4_decompress_safe(cBlock, decoded, cSize, srcSize), srcSize);

    /* block referencing a dictionary, decoded without it */
    for (pos = 0; pos < srcSize; pos += 1 KB)
        memcpy(src + pos, dict + 64 KB - 1 KB + pos / 16, 200);
    LZ4_loadDict(stream, dict, 64 KB);
    cSize = LZ4_compress_fast_continue(stream, src, cBlock, srcSize, capacity, 1);
    ASSERT(cSize > 0);
    ASSERT_EQ(LZ4_decompress_safe(cBlock, decoded, cSize, srcSize), -3);
    ASSERT_EQ(LZ4_decompress_safe_usingDict(cBlock, decoded, cSize, srcSize, dict, 64 KB), srcSize);
}

/* Invalid offset 0 must not leave previous output buffer content in decoded data
 * (see doc/lz4_Block_format.md). 1 literal, then a match at offset 0,
 * partially decoded into a 20-byte target which ends within the match. */
TEST(decompress_safe_partial_offset0)
{
    char block[32];
    unsigned char decoded[20];
    int r, i;
    memset(block, 0, sizeof(block));
    block[0] = 0x1F;                /* 1 literal, ML_MASK match */
    block[1] = 'A';                 /* literal */
    block[2] = 0x00; block[3] = 0x00;  /* offset = 0 (invalid) */
    block[4] = 64;                  /* match length extension */
    memset(decoded, 0xAA, sizeof(decoded));
    r = LZ4_decompress_safe_partial(block, (char*)decoded, sizeof(block), sizeof(decoded), sizeof(decoded));
    for (i = 1; i < r; i++)
        ASSERT_MSG(decoded[i] != 0xAA, "previous content kept at position %i", i);
}


/* srcSize can be larger than the block's compressed size,
 * provided targetOutputSize is <= the block's decompressed size */
#define PARTIAL_SRC_SIZE 1500
#define PARTIAL_EXTRA_MAX 10

/* @return : compressed size of @src, followed by PARTIAL_EXTRA_MAX bytes of padding */
static int compressPadded(const char* src, char* dst)
{
    int const capacity = LZ4_COMPRESSBOUND(PARTIAL_SRC_SIZE) + PARTIAL_EXTRA_MAX;
    int cSize;
    memset(dst, 0, (size_t)capacity);
    cSize = LZ4_compress_default(src, dst, PARTIAL_SRC_SIZE, capacity - PARTIAL_EXTRA_MAX);
    ASSERT(cSize > 0);
    return cSize;
}

TEST(decompress_safe_partial_srcSizeBeyondBlock)
{
    const char* const src = (const char*)UT_sample(PARTIAL_SRC_SIZE);
    char* const cBuffer = (char*)UT_malloc(LZ4_COMPRESSBOUND(PARTIAL_SRC_SIZE) + PARTIAL_EXTRA_MAX);
    char decoded[2 KB];
    int const cSize = compressPadded(src, cBuffer);
    int extra;
    for (extra = 0; extra < PARTIAL_EXTRA_MAX; extra++) {
        int const r = LZ4_decompress_safe_partial(cBuffer, decoded, cSize + extra, PARTIAL_SRC_SIZE, sizeof(decoded));
        ASSERT_MSG(r == PARTIAL_SRC_SIZE && !memcmp(decoded, src, PARTIAL_SRC_SIZE),
                   "srcSize = compressed size + %i : %i bytes decoded", extra, r);
    }
}

TEST(decompress_safe_partial_usingDict_srcSizeBeyondBlock)
{
    int const smallSize = 1 KB;
    int const largeSize = 64 KB - 1;
    const char* const src = (const char*)UT_sample(PARTIAL_SRC_SIZE);
    char* const cBuffer = (char*)UT_malloc(LZ4_COMPRESSBOUND(PARTIAL_SRC_SIZE) + PARTIAL_EXTRA_MAX);
    char* const buffer = (char*)UT_malloc((size_t)largeSize + 2 KB);   /* prefix, then decoded data */
    char* const decoded = buffer + largeSize;
    char* const extDict = (char*)UT_malloc((size_t)largeSize);
    struct { const char* name; const char* start; int size; } const dicts[] = {
        { "no dictionary",       NULL,                                0 },
        { "small prefix",        decoded - smallSize,                 smallSize },
        { "large prefix",        buffer,                              largeSize },
        { "small external dict", extDict + largeSize - smallSize,     smallSize },
        { "large external dict", extDict,                             largeSize } };
    int const cSize = compressPadded(src, cBuffer);
    size_t d;
    int extra;
    memset(buffer, 0, (size_t)largeSize);
    memset(extDict, 0, (size_t)largeSize);
    for (d = 0; d < ARRAY_SIZE(dicts); d++)
    for (extra = 0; extra < PARTIAL_EXTRA_MAX; extra++) {
        int const r = LZ4_decompress_safe_partial_usingDict(cBuffer, decoded, cSize + extra, PARTIAL_SRC_SIZE, 2 KB,
                                                            dicts[d].start, dicts[d].size);
        ASSERT_MSG(r == PARTIAL_SRC_SIZE && !memcmp(decoded, src, PARTIAL_SRC_SIZE),
                   "%s, srcSize = compressed size + %i : %i bytes decoded", dicts[d].name, extra, r);
    }
}


/*-************************************
*  Decoding edge cases
**************************************/
/* zero capacity : only an empty block (a single token 0) succeeds, or partial decoding of nothing */
TEST(decompress_safe_zeroCapacity)
{
    static const char empty[2] = { 0, 0 };
    static const char literal[2] = { 0x10, 'a' };
    int const srcSize = 1 KB;
    const char* const src = (const char*)UT_sample(srcSize);
    char* const cBlock = (char*)UT_malloc(LZ4_COMPRESSBOUND(srcSize));
    int const cSize = LZ4_compress_default(src, cBlock, srcSize, LZ4_COMPRESSBOUND(srcSize));
    char dst[1];
    ASSERT(cSize > 0);
    ASSERT_EQ(LZ4_decompress_safe(empty, dst, 1, 0), 0);
    ASSERT(LZ4_decompress_safe(empty, dst, 2, 0) < 0);   /* trailing byte */
    ASSERT(LZ4_decompress_safe(literal, dst, 2, 0) < 0);
    ASSERT_EQ(LZ4_decompress_safe_partial(cBlock, dst, cSize, 0, 0), 0);
    ASSERT(LZ4_decompress_safe(cBlock, dst, cSize, -1) < 0);
}

/* LZ4_decompress_fast() reads the single token of an empty block */
TEST(decompress_fast_emptyBlock)
{
    static const char empty[1] = { 0 };
    char dst[1];
    ASSERT_EQ(LZ4_decompress_fast(empty, dst, 0), 1);
}

TEST(decompress_fast_usingDict_noDict)
{
    int const srcSize = 4 KB;
    const char* const src = (const char*)UT_sample(srcSize);
    char* const cBlock = (char*)UT_malloc(LZ4_COMPRESSBOUND(srcSize));
    char* const decoded = (char*)UT_malloc(srcSize);
    int const cSize = LZ4_compress_default(src, cBlock, srcSize, LZ4_COMPRESSBOUND(srcSize));
    ASSERT(cSize > 0);
    ASSERT_EQ(LZ4_decompress_fast_usingDict(cBlock, decoded, srcSize, NULL, 0), cSize);
    ASSERT_MEMEQ(decoded, src, srcSize);
}


/*-************************************
*  One-shot compression
**************************************/
/* to be tested with UBSan */
TEST(compress_default_nullInput)
{
    char dst[16];
    int const cSize = LZ4_compress_default(NULL, dst, 0, LZ4_compressBound(0));
    ASSERT_EQ(cSize, 1);
    ASSERT_EQ(dst[0], 0);
}

TEST(compress_default_nullInputAndOutput)
{
    ASSERT_EQ(LZ4_compress_default(NULL, NULL, 0, 0), 0);
}

/* rejected without being read */
TEST(compress_default_tooLargeInput)
{
    char const src[1] = { 0 };
    char dst[16];
    ASSERT_EQ(LZ4_compress_default(src, dst, LZ4_MAX_INPUT_SIZE + 1, (int)sizeof(dst)), 0);
}

/* an empty block takes 1 byte ; nothing fits into 0 byte */
TEST(compress_smallCapacity)
{
    const char* const src = (const char*)UT_sample(1 KB);
    char dst[1];
    int srcSize = 0;
    ASSERT_EQ(LZ4_compress_default(src, dst, 0, 1), 1);
    ASSERT_EQ(LZ4_compress_destSize(src, dst, &srcSize, 1), 1);
    ASSERT_EQ(srcSize, 0);
    srcSize = 1 KB;
    ASSERT_EQ(LZ4_compress_destSize(src, dst, &srcSize, 1), 1);   /* empty block, no input consumed */
    ASSERT_EQ(srcSize, 0);
    srcSize = 1 KB;
    ASSERT_EQ(LZ4_compress_destSize(src, dst, &srcSize, 0), 0);
}

/* LZ4_compress_destSize_extState() initializes its state */
TEST(compress_destSize_extState_dirtyState)
{
    int const srcSize = 64 KB, targetDstSize = 16 KB;
    const char* const src = (const char*)UT_sample((size_t)srcSize);
    char* const ref = (char*)UT_malloc((size_t)targetDstSize);
    char* const dst = (char*)UT_malloc((size_t)targetDstSize);
    LZ4_stream_t* const state = (LZ4_stream_t*)UT_malloc(sizeof(LZ4_stream_t));
    int refConsumed = srcSize, consumed = srcSize;
    int refSize, cSize;
    LZ4_initStream(state, sizeof(*state));
    refSize = LZ4_compress_destSize_extState(state, src, ref, &refConsumed, targetDstSize, 1);
    ASSERT(refSize > 0);
    memset(state, 0xA5, sizeof(*state));
    cSize = LZ4_compress_destSize_extState(state, src, dst, &consumed, targetDstSize, 1);
    ASSERT(cSize == refSize && consumed == refConsumed && !memcmp(dst, ref, (size_t)cSize));
}

/* deprecated functions give the same output as their replacements */
TEST(compress_deprecatedFunctions)
{
    int const srcSize = 32 KB;
    int const capacity = LZ4_COMPRESSBOUND(srcSize);
    const char* const src = (const char*)UT_sample((size_t)srcSize);
    char* const ref = (char*)UT_malloc((size_t)capacity);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    LZ4_stream_t* const stream = newStream();
    int refSize = LZ4_compress_default(src, ref, srcSize, capacity);
    ASSERT(refSize > 0);
    ASSERT(LZ4_compress_withState(stream, src, dst, srcSize) == refSize && !memcmp(dst, ref, (size_t)refSize));
    ASSERT(LZ4_compress_limitedOutput_withState(stream, src, dst, srcSize, capacity) == refSize && !memcmp(dst, ref, (size_t)refSize));

    LZ4_initStream(stream, sizeof(*stream));
    refSize = LZ4_compress_fast_continue(stream, src, ref, srcSize, capacity, 1);
    ASSERT(refSize > 0);
    LZ4_initStream(stream, sizeof(*stream));
    ASSERT(LZ4_compress_continue(stream, src, dst, srcSize) == refSize && !memcmp(dst, ref, (size_t)refSize));
    LZ4_initStream(stream, sizeof(*stream));
    ASSERT(LZ4_compress_limitedOutput_continue(stream, src, dst, srcSize, capacity) == refSize && !memcmp(dst, ref, (size_t)refSize));
    ASSERT_EQ(LZ4_resetStreamState(stream, dst), 0);   /* the stream had a history */
    ASSERT(LZ4_compress_fast_continue(stream, src, dst, srcSize, capacity, 1) == refSize && !memcmp(dst, ref, (size_t)refSize));
}

/* acceleration < 1 is replaced by the default acceleration, 1 */
TEST(compress_destSize_extState_outOfRangeAcceleration)
{
    static const int accels[] = { 0, -1, -100, INT_MIN, INT_MAX };
    int const srcSize = 128 KB;
    int const targetDstSize = srcSize / 4;   /* < LZ4_compressBound(srcSize) : fillOutput mode */
    const char* const src = (const char*)UT_sample((size_t)srcSize);
    char* const ref = (char*)UT_malloc((size_t)targetDstSize);
    char* const dst = (char*)UT_malloc((size_t)targetDstSize);
    char* const decoded = (char*)UT_malloc((size_t)srcSize);
    LZ4_stream_t state;
    int refConsumed = srcSize, refSize;
    size_t n;
    refSize = LZ4_compress_destSize_extState(&state, src, ref, &refConsumed, targetDstSize, 1);
    ASSERT(refSize > 0);
    for (n = 0; n < ARRAY_SIZE(accels); n++) {
        int consumed = srcSize;
        int const cSize = LZ4_compress_destSize_extState(&state, src, dst, &consumed, targetDstSize, accels[n]);
        ASSERT_MSG(cSize > 0 && cSize <= targetDstSize, "acceleration %i : compression failed (%i)", accels[n], cSize);
        if (accels[n] < 1) {
            ASSERT_MSG(cSize == refSize && consumed == refConsumed && !memcmp(dst, ref, (size_t)cSize),
                       "acceleration %i : output differs from acceleration 1", accels[n]);
        }
        ASSERT_EQ(LZ4_decompress_safe(dst, decoded, cSize, srcSize), consumed);
        ASSERT_MEMEQ(decoded, src, (size_t)consumed);
    }
}

TEST(compress_default_inPlace)
{
    int const srcSize = 65 KB;
    int const maxCSize = LZ4_COMPRESSBOUND(srcSize);
    int const bufferSize = LZ4_COMPRESS_INPLACE_BUFFER_SIZE(maxCSize);
    const char* const src = (const char*)UT_sample((size_t)srcSize);
    char* const buffer = (char*)UT_malloc((size_t)bufferSize);
    char* const input = buffer + bufferSize - srcSize;   /* at the end of the buffer */
    char* const decoded = (char*)UT_malloc((size_t)srcSize);
    int cSize;
    memcpy(input, src, (size_t)srcSize);
    cSize = LZ4_compress_default(input, buffer, srcSize, maxCSize);
    ASSERT(cSize > 0);
    ASSERT_EQ(LZ4_decompress_safe(buffer, decoded, cSize, srcSize), srcSize);
    ASSERT_MEMEQ(decoded, src, (size_t)srcSize);
}


/*-************************************
*  Streaming compression
**************************************/
TEST(initStream_alignment)
{
    typedef struct {
        LZ4_stream_t state1;
        LZ4_stream_t state2;
        char         c;
        LZ4_stream_t state3;
    } shct;
    shct* const shc = (shct*)UT_malloc(sizeof(shct));
    memset(shc, 0, sizeof(*shc));
    ASSERT(LZ4_initStream(&shc->state1, sizeof(shc->state1)) != NULL);
    ASSERT(LZ4_initStream(&shc->state2, sizeof(shc->state2)) != NULL);
    ASSERT(LZ4_initStream(&shc->state3, sizeof(shc->state3)) != NULL);
    ASSERT(LZ4_initStream((char*)&shc->state1 + 1, sizeof(shc->state1)) == NULL);   /* bad alignment */
}

TEST(createStream)
{
    newStream();
}

TEST(stream_returnValues)
{
    LZ4_streamDecode_t sd;
    char dict[16] = { 0 };
    ASSERT_EQ(LZ4_freeStream(NULL), 0);
    ASSERT_EQ(LZ4_freeStream(LZ4_createStream()), 0);
    ASSERT_EQ(LZ4_freeStreamDecode(NULL), 0);
    ASSERT_EQ(LZ4_freeStreamDecode(LZ4_createStreamDecode()), 0);   /* released : checked by LeakSanitizer */
    ASSERT_EQ(LZ4_setStreamDecode(&sd, dict, (int)sizeof(dict)), 1);
    ASSERT_EQ(LZ4_setStreamDecode(&sd, NULL, 0), 1);
}

/* LZ4_loadDict() returns the loaded size : 0 below HASH_UNIT, at most 64 KB */
TEST(loadDict_returnedSize)
{
    static const int sizes[][2] = { { 3, 0 }, { 8, 8 }, { 1 KB, 1 KB }, { 63 KB + 512, 63 KB + 512 },
                                    { 64 KB + 512, 64 KB }, { 100 KB, 64 KB } };
    const char* const dict = (const char*)UT_sample(100 KB);
    LZ4_stream_t* const stream = newStream();
    size_t i;
    for (i = 0; i < ARRAY_SIZE(sizes); i++) {
        int const loaded = LZ4_loadDict(stream, dict, sizes[i][0]);
        ASSERT_MSG(loaded == sizes[i][1], "dictionary of %i bytes : %i loaded, instead of %i", sizes[i][0], loaded, sizes[i][1]);
    }
}

/* acceleration < 1 is replaced by 1, > LZ4_ACCELERATION_MAX by LZ4_ACCELERATION_MAX,
 * by streaming and fastReset compression too */
TEST(compress_fast_continue_outOfRangeAcceleration)
{
    static const int accels[][2] = { { 0, 1 }, { -1, 1 }, { INT_MIN, 1 },
                                     { LZ4_ACCELERATION_MAX + 1, LZ4_ACCELERATION_MAX }, { INT_MAX, LZ4_ACCELERATION_MAX } };
    int const srcSize = 32 KB;
    int const capacity = LZ4_COMPRESSBOUND(srcSize);
    const char* const src = (const char*)UT_sample((size_t)srcSize);
    char* const ref = (char*)UT_malloc((size_t)capacity);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    LZ4_stream_t* const stream = newStream();
    size_t i;
    for (i = 0; i < ARRAY_SIZE(accels); i++) {
        int refSize, cSize;
        LZ4_initStream(stream, sizeof(*stream));
        refSize = LZ4_compress_fast_continue(stream, src, ref, srcSize, capacity, accels[i][1]);
        LZ4_initStream(stream, sizeof(*stream));
        cSize = LZ4_compress_fast_continue(stream, src, dst, srcSize, capacity, accels[i][0]);
        ASSERT_MSG(refSize > 0 && cSize == refSize && !memcmp(dst, ref, (size_t)cSize),
                   "LZ4_compress_fast_continue(), acceleration %i : output differs from acceleration %i", accels[i][0], accels[i][1]);

        LZ4_initStream(stream, sizeof(*stream));
        refSize = LZ4_compress_fast_extState_fastReset(stream, src, ref, srcSize, capacity, accels[i][1]);
        LZ4_initStream(stream, sizeof(*stream));
        cSize = LZ4_compress_fast_extState_fastReset(stream, src, dst, srcSize, capacity, accels[i][0]);
        ASSERT_MSG(refSize > 0 && cSize == refSize && !memcmp(dst, ref, (size_t)cSize),
                   "LZ4_compress_fast_extState_fastReset(), acceleration %i : output differs from acceleration %i", accels[i][0], accels[i][1]);
    }
}

TEST(compress_fast_continue_roundTrip)
{
    int const srcSize = 130 KB;
    const char* const src = (const char*)UT_sample((size_t)srcSize);
    char* const dst = (char*)UT_malloc((size_t)srcSize);
    char* const decoded = (char*)UT_malloc((size_t)srcSize);
    int const cSize = LZ4_compress_fast_continue(newStream(), src, dst, srcSize, srcSize - 1, 1);
    ASSERT(cSize > 0);
    ASSERT_EQ(LZ4_decompress_safe(dst, decoded, cSize, srcSize), srcSize);
    ASSERT_MEMEQ(decoded, src, (size_t)srcSize);
}

/* external dictionary, blocks larger than 64 KB : compression continues in prefix mode,
 * once the dictionary is out of reach. The block starts with a copy of a part of the dictionary,
 * and contains another one beyond reach. */
TEST(compress_fast_continue_extDictLargeBlock)
{
    static const int sizes[] = { 64 KB + 12, 64 KB + 100, 200 KB };
    int const maxSize = 200 KB;
    const char* const content = (const char*)UT_fixedSample(64 KB + (size_t)maxSize);   /* the dictionary must be used */
    char* const dict = (char*)UT_malloc(64 KB);
    char* const block = (char*)UT_malloc((size_t)maxSize);
    int const capacity = LZ4_COMPRESSBOUND(maxSize);
    char* const ref = (char*)UT_malloc((size_t)capacity);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    char* const decoded = (char*)UT_malloc((size_t)maxSize);
    LZ4_stream_t* const stream = newStream();
    LZ4_stream_t* const dictStream = newStream();
    size_t i;
    memcpy(dict, content, 64 KB);
    memcpy(block, content + 64 KB, (size_t)maxSize);
    memcpy(block, dict + 32 KB, 1 KB);
    memcpy(block + 100 KB, dict + 48 KB, 1 KB);
    LZ4_loadDict(dictStream, dict, 64 KB);

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int const srcSize = sizes[i];
        int refSize;
        LZ4_loadDict(stream, dict, 64 KB);
        refSize = LZ4_compress_fast_continue(stream, block, ref, srcSize, capacity, 1);
        ASSERT(refSize > 0);
        ASSERT_EQ(LZ4_decompress_safe_usingDict(ref, decoded, refSize, srcSize, dict, 64 KB), srcSize);
        ASSERT_MEMEQ(decoded, block, (size_t)srcSize);
        ASSERT(LZ4_decompress_safe(ref, decoded, refSize, srcSize) < 0);   /* the dictionary is used */

        /* attached dictionary (tables copied for large inputs) : same output */
        LZ4_resetStream_fast(stream);
        LZ4_attach_dictionary(stream, dictStream);
        ASSERT_EQ(LZ4_compress_fast_continue(stream, block, dst, srcSize, capacity, 1), refSize);
        ASSERT_MEMEQ(dst, ref, (size_t)refSize);

        /* output buffer one byte too small */
        LZ4_loadDict(stream, dict, 64 KB);
        ASSERT_EQ(LZ4_compress_fast_continue(stream, block, dst, srcSize, refSize - 1, 1), 0);
    }
}

/* attached dictionary, then small blocks : the first one employs the dictionary's tables in place
 * (below LZ4_DICTCTX_COPY_THRESHOLD), next ones reference the dictionary and previous blocks,
 * which are contiguous (prefix mode) or separate (external dictionary mode) */
TEST(attach_dictionary_smallBlocks)
{
    enum { blockSize = 1 KB, nbBlocks = 8 };
    const char* const dict = (const char*)UT_sample(64 KB);
    char* const src = (char*)UT_noise(nbBlocks * blockSize);
    char* const separate = (char*)UT_malloc(2 * nbBlocks * blockSize);   /* blocks at even positions */
    int const capacity = LZ4_COMPRESSBOUND(blockSize);
    char* const dst = (char*)UT_malloc((size_t)(nbBlocks * capacity));
    char* const decoded = (char*)UT_malloc(nbBlocks * blockSize);
    LZ4_stream_t* const stream = newStream();
    LZ4_stream_t* const dictStream = newStream();
    int n, contiguous;
    for (n = 0; n < nbBlocks; n++) {
        char* const block = src + n * blockSize;
        memcpy(block, dict + n * 7 KB, 300);   /* consecutive matches within the dictionary */
        memcpy(block + 300, dict + 60 KB - n * 3 KB, 200);
        if (n) memcpy(block + 600, block - blockSize + 100, 300);   /* match within the previous block */
    }
    /* first block, from position 600 : a match within the block, immediately followed by a match reaching the end of the dictionary,
     * which continues at the beginning of the block */
    memcpy(src + 600, src, 100);
    memcpy(src + 700, dict + 64 KB - 100, 100);   /* position indexed by LZ4_loadDict() */
    memcpy(src + 800, src, 200);
    for (n = 0; n < nbBlocks; n++) memcpy(separate + 2 * n * blockSize, src + n * blockSize, blockSize);
    LZ4_loadDict(dictStream, dict, 64 KB);
    for (contiguous = 0; contiguous < 2; contiguous++) {
        LZ4_streamDecode_t sd;
        int cSizes[nbBlocks];
        LZ4_resetStream_fast(stream);
        LZ4_attach_dictionary(stream, dictStream);
        for (n = 0; n < nbBlocks; n++) {
            const char* const block = contiguous ? src + n * blockSize : separate + 2 * n * blockSize;
            cSizes[n] = LZ4_compress_fast_continue(stream, block, dst + n * capacity, blockSize, capacity, 1);
            ASSERT(cSizes[n] > 0);
        }
        LZ4_setStreamDecode(&sd, dict, 64 KB);
        for (n = 0; n < nbBlocks; n++) {
            int const r = LZ4_decompress_safe_continue(&sd, dst + n * capacity, decoded + n * blockSize, cSizes[n], blockSize);
            ASSERT_MSG(r == blockSize && !memcmp(decoded + n * blockSize, src + n * blockSize, blockSize),
                       "%s blocks : block %i corrupted", contiguous ? "contiguous" : "separate", n);
        }
    }
}

TEST(resetStream_fast_largeFirstBlock)   /* a large first block gives the same output as a new stream */
{
    const char* const block = clearedTableBlock();
    int const capacity = LZ4_COMPRESSBOUND(BLOCK_64K);
    char* const ref = (char*)UT_malloc((size_t)capacity);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    char saveBuffer[1];
    LZ4_stream_t* const stream = newStream();
    int const refSize = LZ4_compress_fast_continue(stream, block, ref, BLOCK_64K, capacity, 1);
    ASSERT(refSize > 0);

    LZ4_resetStream_fast(stream);   /* the state now has a history */
    ASSERT_EQ(LZ4_compress_fast_continue(stream, block, dst, BLOCK_64K, capacity, 1), refSize);
    ASSERT_MEMEQ(dst, ref, (size_t)refSize);

    LZ4_saveDict(stream, saveBuffer, 0);   /* drop history, without reset */
    ASSERT_EQ(LZ4_compress_fast_continue(stream, block, dst, BLOCK_64K, capacity, 1), refSize);
    ASSERT_MEMEQ(dst, ref, (size_t)refSize);
}

/* a dictionary below HASH_UNIT on all platforms is ignored by LZ4_loadDict() :
 * a large first block gives the same output as a new stream */
TEST(loadDict_tooSmall)
{
    const char* const block = clearedTableBlock();
    const char* const dict = (const char*)UT_sample(4);
    int const capacity = LZ4_COMPRESSBOUND(BLOCK_64K);
    char* const ref = (char*)UT_malloc((size_t)capacity);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    LZ4_stream_t* const stream = newStream();
    LZ4_stream_t* const dictStream = newStream();
    int const refSize = LZ4_compress_fast_continue(stream, block, ref, BLOCK_64K, capacity, 1);
    int dictSize;
    ASSERT(refSize > 0);
    for (dictSize = 0; dictSize < 4; dictSize++) {
        LZ4_loadDict(dictStream, dict, dictSize);
        ASSERT_MSG(LZ4_compress_fast_continue(dictStream, block, dst, BLOCK_64K, capacity, 1) == refSize
                   && !memcmp(dst, ref, (size_t)refSize),
                   "after LZ4_loadDict() (%i bytes) : output differs from a new stream", dictSize);

        LZ4_loadDict(dictStream, dict, dictSize);
        LZ4_initStream(stream, sizeof(*stream));
        LZ4_attach_dictionary(stream, dictStream);
        ASSERT_MSG(LZ4_compress_fast_continue(stream, block, dst, BLOCK_64K, capacity, 1) == refSize
                   && !memcmp(dst, ref, (size_t)refSize),
                   "after LZ4_attach_dictionary() (%i bytes) : output differs from a new stream", dictSize);
    }
}

TEST(saveDict_afterInit)
{
    LZ4_stream_t* const stream = newStream();
    char saveBuffer[240];
    ASSERT_EQ(LZ4_saveDict(stream, NULL, 0), 0);
    ASSERT_EQ(LZ4_saveDict(stream, saveBuffer, sizeof(saveBuffer)), 0);   /* compression hasn't started */
}

/* a negative size saves nothing (instead of wrapping to 64 KB),
 * and the next block no longer references the previous dictionary :
 * it copies the end of the dictionary, so it would match it */
TEST(saveDict_negativeSize)
{
    const char* const dict = (const char*)UT_sample(64 KB);
    char* const block = (char*)UT_malloc(4 KB);
    int const capacity = LZ4_COMPRESSBOUND(4 KB);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    char* const decoded = (char*)UT_malloc(4 KB);
    LZ4_stream_t* const stream = newStream();
    char saveBuffer[16];
    int cSize;
    memcpy(block, dict + 60 KB, 4 KB);
    ASSERT(LZ4_loadDict(stream, dict, 64 KB) > 0);
    ASSERT_EQ(LZ4_saveDict(stream, saveBuffer, -1), 0);
    cSize = LZ4_compress_fast_continue(stream, block, dst, 4 KB, capacity, 1);
    ASSERT(cSize > 0);
    ASSERT_EQ(LZ4_decompress_safe(dst, decoded, cSize, 4 KB), 4 KB);   /* no reference to the dictionary */
    ASSERT_MEMEQ(decoded, block, 4 KB);
}

/* LZ4_saveDict() saves up to 64 KB, or less when asked, into a buffer of that size :
 * next blocks only reference the saved part, though the hash table still points further back */
TEST(saveDict_partial)
{
    static const int saveSizes[] = { 1 KB, 63 KB + 512, 64 KB };
    int const firstSize = 100 KB, blockSize = 16 KB;
    char* const first = (char*)UT_sample((size_t)firstSize);
    char* const tail = first + firstSize - 64 KB;   /* within reach of next block */
    char* const block = (char*)UT_noise((size_t)blockSize);
    int const capacity = LZ4_COMPRESSBOUND(firstSize);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    char* const decoded = (char*)UT_malloc((size_t)blockSize);
    LZ4_stream_t* const stream = newStream();
    size_t i;
    UT_fillNoise(tail, 1 KB);   /* unique content : its positions are the only candidates */
    memcpy(block, tail + 100, 400);   /* only saved within 64 KB */
    memcpy(block + 1 KB, tail + 64 KB - 900, 800);   /* always saved */
    for (i = 0; i < ARRAY_SIZE(saveSizes); i++) {
        int const saveSize = saveSizes[i];
        char* const saved = (char*)UT_malloc((size_t)saveSize);
        int cSize;
        LZ4_resetStream_fast(stream);
        ASSERT(LZ4_compress_fast_continue(stream, first, dst, firstSize, capacity, 1) > 0);
        ASSERT_EQ(LZ4_saveDict(stream, saved, saveSize), saveSize);
        cSize = LZ4_compress_fast_continue(stream, block, dst, blockSize, capacity, 1);
        ASSERT(cSize > 0);
        ASSERT_MSG(LZ4_decompress_safe_usingDict(dst, decoded, cSize, blockSize, saved, saveSize) == blockSize
                   && !memcmp(decoded, block, (size_t)blockSize),
                   "%i bytes saved : the block references data beyond them", saveSize);
        UT_release(saved);
    }

    /* larger sizes save 64 KB */
    {   char* const saved = (char*)UT_malloc((size_t)firstSize);
        LZ4_resetStream_fast(stream);
        ASSERT(LZ4_compress_fast_continue(stream, first, dst, firstSize, capacity, 1) > 0);
        ASSERT_EQ(LZ4_saveDict(stream, saved, 64 KB + 512), 64 KB);
        ASSERT_EQ(LZ4_saveDict(stream, saved, firstSize), 64 KB);
        ASSERT_MEMEQ(saved, tail, 64 KB);
    }
}

/* chains of short matches must not hide the period (#1805) */
TEST(compress_periodicRuns)
{
    static const char dict[8] = { 0, 0, 0, 0, 0, 0, 0, 4 };
    unsigned char page[8 + 4096];   /* dict, then a 4 KB page alternating runs of 16 x 0xFF and 16 x 0x00 */
    int const capacity = LZ4_COMPRESSBOUND(sizeof(page));
    char* const dst = (char*)UT_malloc((size_t)capacity);
    LZ4_stream_t* const stream = newStream();
    LZ4_stream_t* const dictStream = newStream();
    int r;
    memcpy(page, dict, sizeof(dict));
    for (r = 0; r < 32; r++) {   /* several rotations compressed to ~1.5 KB, instead of < 60 bytes */
        int i, c;
        for (i = 0; i < 4096; i++)
            page[8 + i] = (((i + r) % 4096 + 8) / 16) % 2 == 0 ? 0xFF : 0;

        c = LZ4_compress_default((const char*)page, dst, (int)sizeof(page), capacity);
        ASSERT_MSG(c > 0 && c <= 100, "rotation %i, dictionary bytes in the same block : %i bytes", r, c);

        LZ4_initStream(stream, sizeof(*stream));
        LZ4_loadDict(stream, dict, (int)sizeof(dict));
        c = LZ4_compress_fast_continue(stream, (const char*)page + 8, dst, 4096, capacity, 1);
        ASSERT_MSG(c > 0 && c <= 100, "rotation %i, LZ4_loadDict() : %i bytes", r, c);

        LZ4_initStream(dictStream, sizeof(*dictStream));
        LZ4_loadDict(dictStream, dict, (int)sizeof(dict));
        LZ4_initStream(stream, sizeof(*stream));
        LZ4_attach_dictionary(stream, dictStream);
        c = LZ4_compress_fast_continue(stream, (const char*)page + 8, dst, 4096, capacity, 1);
        ASSERT_MSG(c > 0 && c <= 100, "rotation %i, LZ4_attach_dictionary() : %i bytes", r, c);
    }
}

/* the new block can overwrite the previous one (its dictionary) : any overlap must be handled */
TEST(compress_continue_inputOverwritesPreviousBlock)
{
    /* block 1 : 1000 bytes at position 1000 ; block 2 : { position, size } */
    static const int overlaps[4][2] = {
        {  500, 1000 },   /* covers the beginning of block 1 */
        { 1000, 1000 },   /* same place */
        {  500, 2000 },   /* covers block 1 entirely */
        { 1500, 1000 } }; /* covers the end of block 1 */
    int const pos1 = 1000, size1 = 1000;
    int const capacity = LZ4_COMPRESSBOUND(4 KB);
    char* const ring = (char*)UT_malloc(4 KB);
    char* const src = (char*)UT_malloc(4 KB);   /* content of both blocks */
    char* const cBlock1 = (char*)UT_malloc((size_t)capacity);
    char* const cBlock2 = (char*)UT_malloc((size_t)capacity);
    char* const decoded = (char*)UT_malloc(4 KB);
    LZ4_stream_t* const stream = newStream();
    LZ4_streamHC_t* const streamHC = (LZ4_streamHC_t*)UT_defer(freeStreamHC, LZ4_createStreamHC());
    int ov, hc, trial;
    ASSERT(streamHC != NULL);
    memset(ring, 0, 4 KB);
    for (ov = 0; ov < 4; ov++)
    for (hc = 0; hc < 2; hc++)
    for (trial = 0; trial < 64; trial++) {
        int const pos2 = overlaps[ov][0], size2 = overlaps[ov][1];
        LZ4_streamDecode_t sd;
        int c1, c2, i, r;
        for (i = 0; i < size1 + size2; i++) src[i] = "ACGT"[UT_rand() & 3];   /* many matches */
        LZ4_initStream(stream, sizeof(*stream));
        LZ4_initStreamHC(streamHC, sizeof(*streamHC));
        memcpy(ring + pos1, src, (size_t)size1);
        c1 = hc ? LZ4_compress_HC_continue(streamHC, ring + pos1, cBlock1, size1, capacity)
                : LZ4_compress_fast_continue(stream, ring + pos1, cBlock1, size1, capacity, 1);
        memcpy(ring + pos2, src + size1, (size_t)size2);
        c2 = hc ? LZ4_compress_HC_continue(streamHC, ring + pos2, cBlock2, size2, capacity)
                : LZ4_compress_fast_continue(stream, ring + pos2, cBlock2, size2, capacity, 1);
        ASSERT(c1 > 0 && c2 > 0);
        LZ4_setStreamDecode(&sd, NULL, 0);
        r = LZ4_decompress_safe_continue(&sd, cBlock1, decoded, c1, size1);
        r += LZ4_decompress_safe_continue(&sd, cBlock2, decoded + size1, c2, size2);
        ASSERT_MSG(r == size1 + size2 && !memcmp(decoded, src, (size_t)(size1 + size2)),
                   "%s : corrupted output, block 2 (position %i, size %i) overwrites block 1 (position %i, size %i)",
                   hc ? "LZ4_compress_HC_continue()" : "LZ4_compress_fast_continue()", pos2, size2, pos1, size1);
    }
}

/* partial decoding of a block referencing its dictionary, located before dst (prefix) or elsewhere (external) */
TEST(decompress_safe_partial_usingDict)
{
    static const int dictSizes[] = { 1 KB, 64 KB };
    int const srcSize = 8 KB, target = 5 KB;
    char* const src = (char*)UT_noise((size_t)srcSize);
    const char* const dict = (const char*)UT_sample(64 KB);
    int const capacity = LZ4_COMPRESSBOUND(srcSize);
    char* const cBlock = (char*)UT_malloc((size_t)capacity);
    char* const buffer = (char*)UT_malloc(64 KB + (size_t)srcSize);   /* prefix, then decoded data */
    char* const decoded = buffer + 64 KB;
    LZ4_stream_t* const stream = newStream();
    size_t i;
    int pos;
    for (pos = 0; pos < srcSize; pos += 1 KB)   /* the block references the end of the dictionary */
        memcpy(src + pos, dict + 64 KB - 1 KB + pos / 16, 200);
    for (i = 0; i < ARRAY_SIZE(dictSizes); i++) {
        int const dictSize = dictSizes[i];
        const char* const dictStart = dict + 64 KB - dictSize;
        int cSize, prefix;
        LZ4_loadDict(stream, dictStart, dictSize);
        cSize = LZ4_compress_fast_continue(stream, src, cBlock, srcSize, capacity, 1);
        ASSERT(cSize > 0);
        for (prefix = 0; prefix < 2; prefix++) {
            int r;
            if (prefix) memcpy(decoded - dictSize, dictStart, (size_t)dictSize);
            else memset(buffer, 0, 64 KB);
            r = LZ4_decompress_safe_partial_usingDict(cBlock, decoded, cSize, target, srcSize,
                                                      prefix ? decoded - dictSize : dictStart, dictSize);
            ASSERT_MSG(r == target && !memcmp(decoded, src, (size_t)target),
                       "%s dictionary of %i bytes : %i bytes decoded", prefix ? "prefix" : "external", dictSize, r);
        }
    }
}

/* beyond 2 GB, a stream renormalizes its indexes (LZ4_renormDictT()) :
 * identical blocks keep referencing previous ones, and decode correctly.
 * Fixed content : for some content, fast levels periodically miss the previous block */
TEST(compress_fast_continue_beyond2GB)
{
    enum { blockSize = 60 KB, ringSize = 16 };
    int const nbBlocks = (int)(((2U << 30) + 10 * blockSize) / blockSize);
    int const firstSample = nbBlocks - 64;   /* zero blocks before : fast */
    int const renormBlock = (int)((2U << 30) / blockSize);   /* crosses 2 GB : indexes renormalized */
    char* const sample = (char*)UT_malloc(blockSize);
    char* const ring = (char*)UT_malloc((size_t)ringSize * blockSize);
    int const capacity = LZ4_COMPRESSBOUND(blockSize);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    char* const history = (char*)UT_malloc(3 * blockSize);   /* 2 previous blocks, then decoded block */
    LZ4_stream_t* const stream = newStream();
    unsigned seed = 1;
    int n;
    for (n = 0; n < blockSize; n++) {   /* text, with 1 byte in 8 random */
        seed = seed * 1103515245u + 12345u;
        sample[n] = ((seed >> 16) % 8 == 0) ? (char)(seed >> 8) : "lorem ipsum dolor sit amet "[n % 27];
    }
    memset(ring, 0, (size_t)ringSize * blockSize);
    memcpy(history, sample, blockSize);
    memcpy(history + blockSize, sample, blockSize);
    for (n = 0; n < nbBlocks; n++) {
        char* const block = ring + (n % ringSize) * blockSize;
        int cSize;
        if (n >= firstSample) memcpy(block, sample, blockSize);
        cSize = LZ4_compress_fast_continue(stream, block, dst, blockSize, capacity, 1);
        ASSERT(cSize > 0);
        if (n >= firstSample + 2)
            ASSERT_MSG(cSize < blockSize / 16, "block %i (%u MB) : %i bytes, previous identical blocks not referenced",
                       n, (unsigned)((size_t)n * blockSize >> 20), cSize);
        if (n >= renormBlock - 2) {
            int const r = LZ4_decompress_safe_usingDict(dst, history + 2 * blockSize, cSize, blockSize, history, 2 * blockSize);
            ASSERT_MSG(r == blockSize && !memcmp(history + 2 * blockSize, sample, blockSize),
                       "block %i (%u MB) : corrupted", n, (unsigned)((size_t)n * blockSize >> 20));
        }
    }
}



/*-************************************
*  Address space overflow (32-bit)
**************************************/
#define AO_NB_BUFFERS_MAX 150
#define AO_BUFFER_SIZE (32 MB)
typedef struct { char* buffers[AO_NB_BUFFERS_MAX]; int nb; } ao_buffers;
static void ao_free(void* p)
{
    ao_buffers* const b = (ao_buffers*)p;
    if (b == NULL) return;
    while (b->nb) free(b->buffers[--b->nb]);
    free(b);
}

/* Aggressively pushes memory allocation limits,
 * and generates patterns which create address space overflow.
 * Only possible in 32-bit mode */
TEST(decompress_safe_addressSpaceOverflow)
{
    ao_buffers* b;
    int n;
    if (sizeof(void*) == 8) return;   /* 64-bit : no overflow */
    b = (ao_buffers*)UT_defer(ao_free, calloc(1, sizeof(ao_buffers)));   /* all buffers released at the end */
    ASSERT(b != NULL);
    for (n = 0; n < AO_NB_BUFFERS_MAX; n++) {
        b->buffers[n] = (char*)malloc(AO_BUFFER_SIZE);
        if (b->buffers[n] == NULL) return;   /* no more memory : end of test */
        b->nb++;
        if (n < 2) continue;
        {   size_t const sizeToGenerateOverflow = (size_t)(0 - (uintptr_t)b->buffers[n-1] + 512);
            int const nbOf255 = (int)((sizeToGenerateOverflow / 255) + 1);
            unsigned char* const input = (unsigned char*)b->buffers[n-1];
            int u;
            input[0] = 0xF0;   /* Literal length overflow */
            input[1] = 0xFF;
            input[2] = 0xFF;
            input[3] = 0xFF;
            for (u = 4; u <= nbOf255+4; u++) input[u] = 0xFF;
            ASSERT(LZ4_decompress_safe((const char*)input, b->buffers[n], nbOf255+64, AO_BUFFER_SIZE) <= 0);
            input[0] = 0x1F;   /* Match length overflow */
            input[1] = 0x01;
            input[2] = 0x01;
            input[3] = 0x00;
            ASSERT(LZ4_decompress_safe((const char*)input, b->buffers[n], nbOf255+64, AO_BUFFER_SIZE) <= 0);

            /* reverse in/out pointer order */
            input[0] = 0xF0;   /* Literal length overflow */
            input[1] = 0xFF;
            input[2] = 0xFF;
            input[3] = 0xFF;
            ASSERT(LZ4_decompress_safe((const char*)input, b->buffers[n-2], nbOf255+64, AO_BUFFER_SIZE) <= 0);
            input[0] = 0x1F;   /* Match length overflow */
            input[1] = 0x01;
            input[2] = 0x01;
            input[3] = 0x00;
            ASSERT(LZ4_decompress_safe((const char*)input, b->buffers[n-2], nbOf255+64, AO_BUFFER_SIZE) <= 0);
    }   }
}


/*-************************************
*  In-place decompression
**************************************/
/* worst case : compressed size == original size - 1 */
TEST(decompress_safe_inPlace_limitCase)
{
    int const srcSize = 65 KB;
    int const capacity = LZ4_COMPRESSBOUND(srcSize);
    int const bufferSize = LZ4_DECOMPRESS_INPLACE_BUFFER_SIZE(srcSize);
    char* const src = (char*)UT_malloc((size_t)srcSize);
    char* const cBuffer = (char*)UT_malloc((size_t)capacity);
    char* const buffer = (char*)UT_malloc((size_t)bufferSize);
    int cSize = 0, attempt, nbZeroes;
    /* incompressible data, starting with zeroes : each zero shrinks the compressed size by about 1 byte */
    for (attempt = 0; attempt < 16 && cSize != srcSize - 1; attempt++) {
        UT_fillNoise(src, (size_t)srcSize);
        for (nbZeroes = 0; nbZeroes < 2000; nbZeroes++) {
            src[nbZeroes] = 0;
            cSize = LZ4_compress_default(src, cBuffer, srcSize, capacity);
            if (cSize <= srcSize - 1) break;
    }   }
    ASSERT_EQ(cSize, srcSize - 1);

    /* compressed data at the end of the buffer, decoded from its beginning */
    memcpy(buffer + bufferSize - cSize, cBuffer, (size_t)cSize);
    ASSERT_EQ(LZ4_decompress_safe(buffer + bufferSize - cSize, buffer, cSize, srcSize), srcSize);
    ASSERT_MEMEQ(buffer, src, (size_t)srcSize);
}


/*-************************************
*  Ring buffers
**************************************/
#define RB_INPUT_SIZE (196 KB)
#define RB_RING_SIZE (8 KB)

/* the encoder's ring buffer is also used by the decoders */
TEST(compress_fast_continue_ringBuffer)
{
    unsigned const maxMessageSizeMask = (1 << 10) - 1;
    unsigned const srcSize = 130 KB;
    unsigned const dBufferSize = RB_RING_SIZE + maxMessageSizeMask;
    const char* const src = (const char*)UT_sample(srcSize);
    char* const ring = (char*)UT_malloc(RB_RING_SIZE);
    int const capacity = LZ4_COMPRESSBOUND(1 KB);
    char* const cBuffer = (char*)UT_malloc((size_t)capacity);
    char* const dSafe = (char*)UT_malloc(dBufferSize);
    char* const dFast = (char*)UT_malloc(dBufferSize);
    LZ4_stream_t* const stream = newStream();
    LZ4_streamDecode_t sdSafe, sdFast;
    unsigned messageSize = (UT_rand() & maxMessageSizeMask) + 1;
    unsigned iNext = 0, rNext = 0, dNext = 0;
    LZ4_setStreamDecode(&sdSafe, NULL, 0);
    LZ4_setStreamDecode(&sdFast, NULL, 0);

    while (iNext + messageSize < srcSize) {
        int cSize;
        memcpy(ring + rNext, src + iNext, messageSize);
        cSize = LZ4_compress_fast_continue(stream, ring + rNext, cBuffer, (int)messageSize, capacity, 1);
        ASSERT(cSize > 0);
        ASSERT_EQ(LZ4_decompress_safe_continue(&sdSafe, cBuffer, dSafe + dNext, cSize, (int)messageSize), messageSize);
        ASSERT_MEMEQ(dSafe + dNext, src + iNext, messageSize);
        ASSERT_EQ(LZ4_decompress_fast_continue(&sdFast, cBuffer, dFast + dNext, (int)messageSize), cSize);
        ASSERT_MEMEQ(dFast + dNext, src + iNext, messageSize);

        /* prepare next message */
        iNext += messageSize;
        rNext += messageSize;
        dNext += messageSize;
        messageSize = (UT_rand() & maxMessageSizeMask) + 1;
        if (rNext + messageSize > RB_RING_SIZE) rNext = 0;
        if (dNext + messageSize > dBufferSize) dNext = 0;
    }
}

/* writeSequence() :
 * writes a sequence of @litLength literals, followed by a match,
 * or only literals if @matchLength == 0 (last sequence).
 * @return : nb of bytes written */
static size_t writeSequence(unsigned char* op, const unsigned char* literals, size_t litLength, unsigned offset, size_t matchLength)
{
    unsigned char* const ostart = op;
    size_t const mlCode = matchLength ? matchLength - 4 : 0;   /* minimum match length is 4 */
    size_t len;
    *op++ = (unsigned char)(((litLength < 15 ? litLength : 15) << 4) | (mlCode < 15 ? mlCode : 15));
    if (litLength >= 15) {
        for (len = litLength - 15; len >= 255; len -= 255) *op++ = 255;
        *op++ = (unsigned char)len;
    }
    memcpy(op, literals, litLength); op += litLength;
    if (matchLength == 0) return (size_t)(op - ostart);
    *op++ = (unsigned char)offset; *op++ = (unsigned char)(offset >> 8);
    if (mlCode >= 15) {
        for (len = mlCode - 15; len >= 255; len -= 255) *op++ = 255;
        *op++ = (unsigned char)len;
    }
    return (size_t)(op - ostart);
}

/* Ring buffer of LZ4_decoderRingBufferSize(), or of the former LZ4_DECODER_RING_BUFFER_SIZE() (still supported) :
 * any valid block must decode correctly.
 * Worst case : decoding restarts at the beginning of the ring with maxBlockSize-1 bytes left at its end,
 * then a wide copy (long literals or long match) is followed by a match at maximum distance */
#define FORMER_DECODER_RING_BUFFER_SIZE(maxBlockSize) (65536 + 14 + (maxBlockSize))   /* up to v1.10.0 */
/* 0 for invalid sizes, at least LZ4_DECODER_RING_BUFFER_SIZE(16) */
TEST(decoderRingBufferSize_values)
{
    ASSERT_EQ(LZ4_decoderRingBufferSize(-1), 0);
    ASSERT_EQ(LZ4_decoderRingBufferSize(LZ4_MAX_INPUT_SIZE + 1), 0);
    ASSERT_EQ(LZ4_decoderRingBufferSize(0), LZ4_DECODER_RING_BUFFER_SIZE(16));
    ASSERT_EQ(LZ4_decoderRingBufferSize(15), LZ4_DECODER_RING_BUFFER_SIZE(16));
    ASSERT_EQ(LZ4_decoderRingBufferSize(16), LZ4_DECODER_RING_BUFFER_SIZE(16));
    ASSERT_EQ(LZ4_decoderRingBufferSize(17), LZ4_DECODER_RING_BUFFER_SIZE(17));
    ASSERT_EQ(LZ4_decoderRingBufferSize(LZ4_MAX_INPUT_SIZE), LZ4_DECODER_RING_BUFFER_SIZE(LZ4_MAX_INPUT_SIZE));
}

TEST(decompress_safe_continue_decoderRingBufferSize)
{
    const char* const src = (const char*)UT_sample(RB_INPUT_SIZE);
    const unsigned char* const literals = (const unsigned char*)src + 100 KB;   /* must differ from the ring's content */
    int const capacity = LZ4_COMPRESSBOUND(RB_INPUT_SIZE);
    char* const cBuffer = (char*)UT_malloc((size_t)capacity);
    char* const work = (char*)UT_malloc(RB_INPUT_SIZE);
    int former;
    for (former = 0; former < 2; former++) {
        int const maxBlockSize = 1 KB;
        int const ringSize = former ? FORMER_DECODER_RING_BUFFER_SIZE(maxBlockSize) : LZ4_decoderRingBufferSize(maxBlockSize);
        int const firstPassSize = ringSize - (maxBlockSize - 1);   /* next block restarts at the beginning */
        char* const ring = work;
        char* const dict = work + ringSize;   /* copy of the last 64 KB of the first pass */
        int const cSize = LZ4_compress_default(src, cBuffer, firstPassSize, capacity);
        size_t len;
        int pattern;
        ASSERT(cSize > 0);
        ASSERT((size_t)ringSize + 64 KB <= RB_INPUT_SIZE);
        for (pattern = 0; pattern < 2; pattern++)
        for (len = 0; len < 80; len++) {
            LZ4_streamDecode_t sd;
            unsigned char block[256];
            char expected[256];
            size_t n = 0;
            int outSize;
            LZ4_setStreamDecode(&sd, NULL, 0);
            ASSERT_EQ(LZ4_decompress_safe_continue(&sd, cBuffer, ring, cSize, firstPassSize), firstPassSize);
            memcpy(dict, ring + firstPassSize - 64 KB, 64 KB);
            if (pattern == 0) {   /* long literals */
                n += writeSequence(block + n, literals, len, 65535, 4);
                outSize = (int)len + 4;
            } else {   /* long match */
                n += writeSequence(block + n, literals, 16, 16, 4 + len);
                n += writeSequence(block + n, literals, 0, 65535, 4);
                outSize = 16 + 4 + (int)len + 4;
            }
            n += writeSequence(block + n, literals, 100, 0, 0);   /* the fast loop needs input margin */
            outSize += 100;
            ASSERT_EQ(LZ4_decompress_safe_usingDict((const char*)block, expected, (int)n, outSize, dict, 64 KB), outSize);   /* reference */
            ASSERT_MSG(LZ4_decompress_safe_continue(&sd, (const char*)block, ring, (int)n, maxBlockSize) == outSize
                       && !memcmp(ring, expected, (size_t)outSize),
                       "former size %i, pattern %i, length %u : decoding failed or corrupted", former, pattern, (unsigned)len);
    }   }
}

/* Ring buffer larger than the encoder's ring by maxBlockSize + 64 bytes, decoded with dstCapacity = maxBlockSize.
 * Worst case : decoding restarts at the beginning of the ring, a block ends with a wide copy writing beyond its end,
 * a 1-byte block follows, then a block references the oldest byte allowed by the encoder's ring */
TEST(decompress_safe_continue_ringLargerThanEncoders)
{
    int const maxBlockSize = 1 KB;
    int const encRingSize = 4 KB;
    int const ringSize = encRingSize + maxBlockSize + 64;
    int const firstPassSize = ringSize - (maxBlockSize - 1);   /* next block restarts at the beginning */
    int const dist = encRingSize - 13;   /* max distance, from the start of a 13-byte block */
    const unsigned char* const literals = (const unsigned char*)UT_sample((size_t)firstPassSize + 2 KB);
    char* const ring = (char*)UT_malloc((size_t)ringSize);
    char* const stream = (char*)UT_malloc((size_t)firstPassSize + 2 KB);   /* contiguous copy of decoded data */
    unsigned char* const cBuffer = (unsigned char*)UT_malloc((size_t)LZ4_COMPRESSBOUND(maxBlockSize));
    unsigned offset;
    size_t ml;
    for (offset = 32; offset <= 64; offset += 32)
    for (ml = 100; ml < 164; ml++) {
        LZ4_streamDecode_t sd;
        unsigned char block[64];
        int dpos = 0, spos = 0, n, r;
        LZ4_setStreamDecode(&sd, NULL, 0);
        while (dpos < firstPassSize) {   /* first pass : literals only */
            int const bs = MIN(maxBlockSize, firstPassSize - dpos);
            n = (int)writeSequence(cBuffer, literals + dpos, (size_t)bs, 0, 0);
            ASSERT_EQ(LZ4_decompress_safe_continue(&sd, (const char*)cBuffer, ring + dpos, n, maxBlockSize), bs);
            memcpy(stream + spos, ring + dpos, (size_t)bs);
            spos += bs; dpos += bs;
        }
        dpos = 0;   /* less than maxBlockSize left : restart at the beginning */
        /* block A : ends with a wide copy (offset, ml), then 14 literals */
        n = (int)writeSequence(block, literals, 14, 14, 70);
        n += (int)writeSequence(block + n, literals, 0, offset, ml);
        n += (int)writeSequence(block + n, literals + 14, 14, 0, 0);
        r = LZ4_decompress_safe_continue(&sd, (const char*)block, ring + dpos, n, maxBlockSize);
        ASSERT_EQ(r, 14 + 70 + (int)ml + 14);
        memcpy(stream + spos, ring + dpos, (size_t)r);
        spos += r; dpos += r;
        /* block B : 1 literal */
        n = (int)writeSequence(block, literals, 1, 0, 0);
        r = LZ4_decompress_safe_continue(&sd, (const char*)block, ring + dpos, n, maxBlockSize);
        ASSERT_EQ(r, 1);
        memcpy(stream + spos, ring + dpos, (size_t)r);
        spos += r; dpos += r;
        /* block C : match at maximum distance, 9 literals */
        n = (int)writeSequence(block, literals, 0, (unsigned)dist, 4);
        n += (int)writeSequence(block + n, literals, 9, 0, 0);
        ASSERT_EQ(LZ4_decompress_safe_continue(&sd, (const char*)block, ring + dpos, n, maxBlockSize), 13);
        ASSERT_MSG(!memcmp(ring + dpos, stream + spos - dist, 4), "offset %u, length %u : corrupted output", offset, (unsigned)ml);
    }
}
