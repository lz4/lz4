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

#include "unittest.h"
#include <limits.h>     /* INT_MIN, INT_MAX */
#include <string.h>     /* memset, memcpy */
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
    char const src[1] = { (char)0xFF };
    char* const decoded = (char*)UT_malloc(DECODED_CAPACITY);
    ASSERT(LZ4_decompress_safe(src, decoded, 1, DECODED_CAPACITY) < 0);
}

/* 14 bytes of literals, followed by a 14 byte match :
 * must not read beyond the end of the input (#508) */
TEST(decompress_safe_shortcutEdgeCase)
{
    char block[17];
    char* const decoded = (char*)UT_malloc(DECODED_CAPACITY);
    block[0] = (char)0xEE;
    memset(block + 1, 0, 14);
    block[15] = 14;
    block[16] = 0;
    ASSERT(LZ4_decompress_safe(block, decoded, sizeof(block), DECODED_CAPACITY) < 0);
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
    char block[18];
    char decoded[65];
    size_t n;
    block[0] = (char)0xEF;          /* 14 literals, ML_MASK match */
    memset(block + 1, 'A', 14);     /* 14 literal bytes */
    block[15] = 0x0E; block[16] = 0x00;  /* offset = 14 */
    block[17] = 0x00;               /* match length extension = 0 */
    for (n = 0; n < ARRAY_SIZE(dstCapacities); n++)
        ASSERT(LZ4_decompress_safe(block, decoded, sizeof(block), dstCapacities[n]) < 0);
}

/* Invalid offset 0 must not leave previous output buffer content in decoded data
 * (see doc/lz4_Block_format.md). 1 literal, then a match at offset 0,
 * partially decoded into a 20-byte target which ends within the match. */
TEST(decompress_safe_partial_offset0)
{
    char block[32];
    char decoded[20];
    int r, i;
    memset(block, 0, sizeof(block));
    block[0] = 0x1F;                /* 1 literal, ML_MASK match */
    block[1] = 'A';                 /* literal */
    block[2] = 0x00; block[3] = 0x00;  /* offset = 0 (invalid) */
    block[4] = 64;                  /* match length extension */
    memset(decoded, 0xAA, sizeof(decoded));
    r = LZ4_decompress_safe_partial(block, decoded, sizeof(block), sizeof(decoded), sizeof(decoded));
    for (i = 1; i < r; i++)
        ASSERT_MSG(decoded[i] != (char)0xAA, "previous content kept at position %i", i);
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

/* chains of short matches must not hide the period (#1805) */
TEST(compress_periodicRuns)
{
    static const char dict[8] = { 0, 0, 0, 0, 0, 0, 0, 4 };
    char page[8 + 4096];   /* dict, then a 4 KB page alternating runs of 16 x 0xFF and 16 x 0x00 */
    int const capacity = LZ4_COMPRESSBOUND(sizeof(page));
    char* const dst = (char*)UT_malloc((size_t)capacity);
    LZ4_stream_t* const stream = newStream();
    LZ4_stream_t* const dictStream = newStream();
    int r;
    memcpy(page, dict, sizeof(dict));
    for (r = 0; r < 32; r++) {   /* several rotations compressed to ~1.5 KB, instead of < 60 bytes */
        int i, c;
        for (i = 0; i < 4096; i++)
            page[8 + i] = (((i + r) % 4096 + 8) / 16) % 2 == 0 ? (char)0xFF : 0;

        c = LZ4_compress_default(page, dst, (int)sizeof(page), capacity);
        ASSERT_MSG(c > 0 && c <= 100, "rotation %i, dictionary bytes in the same block : %i bytes", r, c);

        LZ4_initStream(stream, sizeof(*stream));
        LZ4_loadDict(stream, dict, (int)sizeof(dict));
        c = LZ4_compress_fast_continue(stream, page + 8, dst, 4096, capacity, 1);
        ASSERT_MSG(c > 0 && c <= 100, "rotation %i, LZ4_loadDict() : %i bytes", r, c);

        LZ4_initStream(dictStream, sizeof(*dictStream));
        LZ4_loadDict(dictStream, dict, (int)sizeof(dict));
        LZ4_initStream(stream, sizeof(*stream));
        LZ4_attach_dictionary(stream, dictStream);
        c = LZ4_compress_fast_continue(stream, page + 8, dst, 4096, capacity, 1);
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


static const UT_test tests[] = {
    UT_TEST(decompress_safe_emptyInput),
    UT_TEST(decompress_safe_oneByteInput),
    UT_TEST(decompress_safe_shortcutEdgeCase),
    UT_TEST(decompress_safe_all0xFF),
    UT_TEST(decompress_safe_ipAtIend),
    UT_TEST(decompress_safe_partial_offset0),
    UT_TEST(compress_default_nullInput),
    UT_TEST(compress_default_nullInputAndOutput),
    UT_TEST(compress_destSize_extState_outOfRangeAcceleration),
    UT_TEST(compress_default_inPlace),
    UT_TEST(initStream_alignment),
    UT_TEST(createStream),
    UT_TEST(compress_fast_continue_roundTrip),
    UT_TEST(resetStream_fast_largeFirstBlock),
    UT_TEST(loadDict_tooSmall),
    UT_TEST(saveDict_afterInit),
    UT_TEST(saveDict_negativeSize),
    UT_TEST(compress_periodicRuns),
    UT_TEST(compress_continue_inputOverwritesPreviousBlock),
};
UT_SUITE(lz4, tests);
