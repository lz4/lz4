/*
    unittest_lz4hc - unit tests of lz4hc (block format, high compression levels)
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
#include <string.h>     /* memset, memcpy, memcmp */
#ifndef LZ4_DISABLE_DEPRECATE_WARNINGS
#  define LZ4_DISABLE_DEPRECATE_WARNINGS   /* LZ4_createHC, LZ4_compressHC2_continue */
#endif
#define LZ4_STATIC_LINKING_ONLY
#include "lz4.h"
#define LZ4_HC_STATIC_LINKING_ONLY   /* LZ4_favorDecompressionSpeed */
#include "lz4hc.h"


/*-************************************
*  Helpers
**************************************/
/* the default level, and the first level of the optimal parser */
static const int g_levels[] = { LZ4HC_CLEVEL_DEFAULT, LZ4HC_CLEVEL_OPT_MIN };

static void freeStreamHC(void* stream) { LZ4_freeStreamHC((LZ4_streamHC_t*)stream); }

/* stream, released at the end of the test */
static LZ4_streamHC_t* newStreamHC(int level)
{
    LZ4_streamHC_t* const stream = (LZ4_streamHC_t*)UT_defer(freeStreamHC, LZ4_createStreamHC());
    ASSERT(stream != NULL);
    LZ4_resetStreamHC_fast(stream, level);
    return stream;
}


/*-************************************
*  Tests
**************************************/
/* to be tested with UBSan */
TEST(compress_HC_nullInput)
{
    int level;
    for (level = LZ4HC_CLEVEL_MIN; level <= LZ4HC_CLEVEL_MAX; level++) {
        char dst[16];
        int const cSize = LZ4_compress_HC(NULL, dst, 0, LZ4_compressBound(0), level);
        ASSERT_MSG(cSize == 1 && dst[0] == 0, "level %i : compressing empty input should give byte 0", level);
    }
}

TEST(compress_HC_nullInputAndOutput)
{
    int level;
    for (level = LZ4HC_CLEVEL_MIN; level <= LZ4HC_CLEVEL_MAX; level++)
        ASSERT_MSG(LZ4_compress_HC(NULL, NULL, 0, 0, level) == 0, "level %i : compressing into NULL must fail", level);
}

TEST(initStreamHC_alignment)
{
    typedef struct {
        LZ4_streamHC_t hc1;
        LZ4_streamHC_t hc2;
        char           c;
        LZ4_streamHC_t hc3;
    } shct;
    shct* const shc = (shct*)UT_malloc(sizeof(shct));
    memset(shc, 0, sizeof(*shc));
    ASSERT(LZ4_initStreamHC(&shc->hc1, sizeof(shc->hc1)) != NULL);
    ASSERT(LZ4_initStreamHC(&shc->hc2, sizeof(shc->hc2)) != NULL);
    ASSERT(LZ4_initStreamHC(&shc->hc3, sizeof(shc->hc3)) != NULL);
    ASSERT(LZ4_initStreamHC((char*)&shc->hc1 + 1, sizeof(shc->hc1)) == NULL);   /* bad alignment */
}

TEST(createStreamHC)
{
    newStreamHC(LZ4HC_CLEVEL_DEFAULT);
}

TEST(streamHC_returnValues)
{
    char buffer[16] = { 0 };
    ASSERT_EQ(LZ4_freeStreamHC(NULL), 0);
    ASSERT_EQ(LZ4_freeStreamHC(LZ4_createStreamHC()), 0);   /* released : checked by LeakSanitizer */
    ASSERT_EQ(LZ4_freeHC(NULL), 0);
    ASSERT_EQ(LZ4_freeHC(LZ4_createHC(buffer)), 0);
}

/* functions initializing a state refuse a misaligned one */
TEST(compress_HC_misalignedState)
{
    int const srcSize = 1 KB;
    int const capacity = LZ4_COMPRESSBOUND(srcSize);
    const char* const src = (const char*)UT_sample((size_t)srcSize);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    char* const state = (char*)UT_malloc(sizeof(LZ4_streamHC_t) + 1);
    int consumed = srcSize;
    ASSERT_EQ(LZ4_compress_HC_extStateHC(state + 1, src, dst, srcSize, capacity, LZ4HC_CLEVEL_DEFAULT), 0);
    ASSERT_EQ(LZ4_compress_HC_destSize(state + 1, src, dst, &consumed, capacity, LZ4HC_CLEVEL_DEFAULT), 0);
    ASSERT_EQ(LZ4_resetStreamStateHC(state + 1, (char*)dst), 1);
    ASSERT_EQ(LZ4_resetStreamStateHC(state, (char*)dst), 0);
}

/* levels < 1 are replaced by LZ4HC_CLEVEL_DEFAULT, levels > LZ4HC_CLEVEL_MAX by LZ4HC_CLEVEL_MAX */
TEST(compress_HC_outOfRangeLevels)
{
    static const int levels[][2] = { { 0, LZ4HC_CLEVEL_DEFAULT }, { -1, LZ4HC_CLEVEL_DEFAULT }, { INT_MIN, LZ4HC_CLEVEL_DEFAULT },
                                     { LZ4HC_CLEVEL_MAX + 1, LZ4HC_CLEVEL_MAX }, { INT_MAX, LZ4HC_CLEVEL_MAX } };
    int const srcSize = 32 KB;
    int const capacity = LZ4_COMPRESSBOUND(srcSize);
    const char* const src = (const char*)UT_sample((size_t)srcSize);
    char* const ref = (char*)UT_malloc((size_t)capacity);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    LZ4_streamHC_t* const stream = newStreamHC(LZ4HC_CLEVEL_DEFAULT);
    size_t i;
    for (i = 0; i < ARRAY_SIZE(levels); i++) {
        int const refSize = LZ4_compress_HC(src, ref, srcSize, capacity, levels[i][1]);
        int cSize = LZ4_compress_HC(src, dst, srcSize, capacity, levels[i][0]);
        ASSERT(refSize > 0);
        ASSERT_MSG(cSize == refSize && !memcmp(dst, ref, (size_t)cSize),
                   "LZ4_compress_HC(), level %i : output differs from level %i", levels[i][0], levels[i][1]);
        LZ4_resetStreamHC_fast(stream, levels[i][0]);
        cSize = LZ4_compress_HC_continue(stream, src, dst, srcSize, capacity);
        ASSERT_MSG(cSize == refSize && !memcmp(dst, ref, (size_t)cSize),
                   "LZ4_compress_HC_continue(), level %i : output differs from level %i", levels[i][0], levels[i][1]);
    }
}

/* a stream compresses its first block as LZ4_compress_HC() does at its level,
 * set by LZ4_resetStreamHC_fast(), LZ4_setCompressionLevel(), or LZ4_resetStreamHC(), which also forgets the history */
TEST(compress_HC_continue_levels)
{
    int const srcSize = 32 KB;
    int const capacity = LZ4_COMPRESSBOUND(srcSize);
    const char* const src = (const char*)UT_sample((size_t)srcSize);
    char* const copy = (char*)UT_malloc((size_t)srcSize);
    char* const ref = (char*)UT_malloc((size_t)capacity);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    LZ4_streamHC_t* const stream = newStreamHC(LZ4HC_CLEVEL_DEFAULT);
    LZ4_streamHC_t* const state = (LZ4_streamHC_t*)UT_malloc(sizeof(LZ4_streamHC_t));
    int level;
    memcpy(copy, src, (size_t)srcSize);
    for (level = 1; level <= LZ4HC_CLEVEL_MAX; level++) {
        int const refSize = LZ4_compress_HC(src, ref, srcSize, capacity, level);
        ASSERT(refSize > 0);
        LZ4_resetStreamHC_fast(stream, level);
        ASSERT_MSG(LZ4_compress_HC_continue(stream, src, dst, srcSize, capacity) == refSize && !memcmp(dst, ref, (size_t)refSize),
                   "level %i, LZ4_resetStreamHC_fast() : output differs from LZ4_compress_HC()", level);
        LZ4_resetStreamHC(stream, level);   /* the stream has a history : the same content, at another place */
        ASSERT_MSG(LZ4_compress_HC_continue(stream, copy, dst, srcSize, capacity) == refSize && !memcmp(dst, ref, (size_t)refSize),
                   "level %i, LZ4_resetStreamHC() : output differs from LZ4_compress_HC()", level);
        ASSERT(LZ4_initStreamHC(state, sizeof(*state)) != NULL);
        LZ4_setCompressionLevel(state, level);
        ASSERT_MSG(LZ4_compress_HC_continue(state, src, dst, srcSize, capacity) == refSize && !memcmp(dst, ref, (size_t)refSize),
                   "level %i, LZ4_setCompressionLevel() : output differs from LZ4_compress_HC()", level);
    }
}

/* LZ4_compress_HC_extStateHC_fastReset() starts a new stream :
 * it ignores the history, the level, the attached dictionary of the state, and a previous failure */
TEST(compress_HC_extStateHC_fastReset_newStream)
{
    int const srcSize = 32 KB;
    int const capacity = LZ4_COMPRESSBOUND(srcSize);
    int const level = LZ4HC_CLEVEL_DEFAULT;
    const char* const src = (const char*)UT_sample((size_t)srcSize);
    char* const ref = (char*)UT_malloc((size_t)capacity);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    LZ4_streamHC_t* const state = newStreamHC(LZ4HC_CLEVEL_MAX);
    LZ4_streamHC_t* const dictStream = newStreamHC(level);
    int const refSize = LZ4_compress_HC(src, ref, srcSize, capacity, level);
    ASSERT(refSize > 0);
    ASSERT(LZ4_compress_HC_continue(state, src, dst, srcSize, capacity) > 0);
    ASSERT_EQ(LZ4_compress_HC_extStateHC_fastReset(state, src, dst, srcSize, capacity, level), refSize);
    ASSERT_MEMEQ(dst, ref, (size_t)refSize);

    ASSERT_EQ(LZ4_loadDictHC(dictStream, src, srcSize), srcSize);
    LZ4_attach_HC_dictionary(state, dictStream);
    ASSERT_EQ(LZ4_compress_HC_extStateHC_fastReset(state, src, dst, srcSize, capacity, level), refSize);
    ASSERT_MEMEQ(dst, ref, (size_t)refSize);

    ASSERT_EQ(LZ4_compress_HC_extStateHC_fastReset(state, src, dst, srcSize, refSize - 1, level), 0);
    ASSERT_EQ(LZ4_compress_HC_extStateHC_fastReset(state, src, dst, srcSize, capacity, level), refSize);
    ASSERT_MEMEQ(dst, ref, (size_t)refSize);
}

/* LZ4_resetStreamStateHC() starts a stream at @inputBuffer */
TEST(resetStreamStateHC_compressHC2_continue)
{
    int const srcSize = 32 KB;
    int const capacity = LZ4_COMPRESSBOUND(srcSize);
    int const level = LZ4HC_CLEVEL_OPT_MIN;
    const char* const src = (const char*)UT_sample((size_t)srcSize);
    char* const ref = (char*)UT_malloc((size_t)capacity);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    LZ4_streamHC_t* const state = (LZ4_streamHC_t*)UT_malloc(sizeof(LZ4_streamHC_t));
    int const refSize = LZ4_compress_HC(src, ref, srcSize, capacity, level);
    ASSERT(refSize > 0);
    ASSERT_EQ(LZ4_resetStreamStateHC(state, (char*)(size_t)src), 0);
    ASSERT_EQ(LZ4_compressHC2_continue(state, src, dst, srcSize, level), refSize);
    ASSERT_MEMEQ(dst, ref, (size_t)refSize);
}

TEST(compress_HC_continue_roundTrip)
{
    int const srcSize = 130 KB;
    const char* const src = (const char*)UT_sample((size_t)srcSize);
    char* const dst = (char*)UT_malloc((size_t)srcSize);
    char* const decoded = (char*)UT_malloc((size_t)srcSize);
    LZ4_streamHC_t* const stream = (LZ4_streamHC_t*)UT_malloc(sizeof(LZ4_streamHC_t));   /* statically allocated */
    size_t l;
    for (l = 0; l < ARRAY_SIZE(g_levels); l++) {
        int cSize;
        LZ4_initStreamHC(stream, sizeof(*stream));
        LZ4_setCompressionLevel(stream, g_levels[l]);
        cSize = LZ4_compress_HC_continue(stream, src, dst, srcSize, srcSize - 1);
        ASSERT(cSize > 0);
        ASSERT_EQ(LZ4_decompress_safe(dst, decoded, cSize, srcSize), srcSize);
        ASSERT_MEMEQ(decoded, src, (size_t)srcSize);
    }
}

/* deprecated streaming API */
TEST(compressHC2_continue_roundTrip)
{
    int const srcSize = 64 KB;
    const char* const src = (const char*)UT_sample((size_t)srcSize);
    char* const dst = (char*)UT_malloc((size_t)LZ4_compressBound(srcSize));
    char* const decoded = (char*)UT_malloc((size_t)srcSize);
    size_t l;
    for (l = 0; l < ARRAY_SIZE(g_levels); l++) {
        void* const ctx = LZ4_createHC(src);
        int cSize;
        ASSERT(ctx != NULL);
        cSize = LZ4_compressHC2_continue(ctx, src, dst, srcSize, g_levels[l]);
        LZ4_freeHC(ctx);
        ASSERT(cSize > 0);
        ASSERT_EQ(LZ4_decompress_safe(dst, decoded, cSize, srcSize), srcSize);
        ASSERT_MEMEQ(decoded, src, (size_t)srcSize);
    }
}

/* deprecated streaming API : LZ4_slideInputBufferHC() returns the beginning of the input buffer,
 * where the next block is compressed without history */
TEST(slideInputBufferHC)
{
    int const blockSize = 32 KB;
    int const capacity = LZ4_COMPRESSBOUND(blockSize);
    const char* const src = (const char*)UT_sample((size_t)(3 * blockSize));
    char* const buffer = (char*)UT_malloc((size_t)(2 * blockSize));
    char* const ref = (char*)UT_malloc((size_t)capacity);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    char* const decoded = (char*)UT_malloc((size_t)(2 * blockSize));
    size_t l;
    for (l = 0; l < ARRAY_SIZE(g_levels); l++) {
        LZ4_streamHC_t* const ctx = (LZ4_streamHC_t*)UT_defer(freeStreamHC, LZ4_createHC(buffer));
        LZ4_streamDecode_t sd;
        int b, cSize, refSize;
        ASSERT(ctx != NULL);
        LZ4_setStreamDecode(&sd, NULL, 0);
        memcpy(buffer, src, (size_t)(2 * blockSize));
        for (b = 0; b < 2; b++) {   /* the second block references the first one */
            cSize = LZ4_compressHC2_continue(ctx, buffer + b * blockSize, dst, blockSize, g_levels[l]);
            ASSERT(cSize > 0);
            ASSERT_EQ(LZ4_decompress_safe_continue(&sd, dst, decoded + b * blockSize, cSize, blockSize), blockSize);
        }
        ASSERT_MEMEQ(decoded, src, (size_t)(2 * blockSize));

        ASSERT(LZ4_slideInputBufferHC(ctx) == buffer);
        memcpy(buffer, src + 2 * blockSize, (size_t)blockSize);
        refSize = LZ4_compress_HC(buffer, ref, blockSize, capacity, g_levels[l]);
        ASSERT(refSize > 0);
        ASSERT_EQ(LZ4_compressHC2_continue(ctx, buffer, dst, blockSize, g_levels[l]), refSize);
        ASSERT_MEMEQ(dst, ref, (size_t)refSize);
        UT_release(ctx);
    }
}

/* level 2 : the match tested at ip+1 must not be ip itself, inside runs */
TEST(compress_HC_level2_runs)
{
    unsigned char runs[2 + 4096];   /* 0xFF 0x00, then runs of 10 x 0xFF and 10 x 0x00 */
    int const capacity = LZ4_COMPRESSBOUND(sizeof(runs));
    char* const dst = (char*)UT_malloc((size_t)capacity);
    int i, cSize;
    runs[0] = 0xFF; runs[1] = 0;
    for (i = 0; i < 4096; i++) runs[2 + i] = (i % 20) < 10 ? 0xFF : 0;
    cSize = LZ4_compress_HC((const char*)runs, dst, (int)sizeof(runs), capacity, 2);
    /* ~1.6 KB (one literal and one offset 1 per run), instead of < 50 bytes */
    ASSERT_MSG(cSize > 0 && cSize <= 100, "%i bytes", cSize);
}

/* #926 */
TEST(saveDictHC_beforeCompression)
{
    /* first : level of LZ4_initStreamHC() */
    static const int levels[] = { LZ4HC_CLEVEL_DEFAULT, 3, LZ4HC_CLEVEL_OPT_MIN, LZ4HC_CLEVEL_MAX };
    const char* const dict = (const char*)UT_sample(128 KB);
    const char* const src = dict + 64 KB;
    int const capacity = LZ4_COMPRESSBOUND(64 KB);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    char* const decoded = (char*)UT_malloc(64 KB);
    LZ4_streamHC_t* const stream = (LZ4_streamHC_t*)UT_malloc(sizeof(LZ4_streamHC_t));
    LZ4_streamHC_t* const dictStream = newStreamHC(LZ4HC_CLEVEL_DEFAULT);
    char saveBuffer[240];
    size_t l;
    int attach;
    ASSERT(LZ4_initStreamHC(stream, sizeof(*stream)) != NULL);
    ASSERT_EQ(LZ4_saveDictHC(stream, NULL, 0), 0);
    ASSERT_EQ(LZ4_saveDictHC(stream, saveBuffer, sizeof(saveBuffer)), 0);   /* compression hasn't started */

    /* next block is compressed as the first one of the stream,
     * without (attach==0) or with (attach==1) an attached dictionary */
    for (l = 0; l < ARRAY_SIZE(levels); l++) {
        for (attach = 0; attach <= 1; attach++) {
            int cSize;
            if (l || attach) LZ4_resetStreamHC_fast(stream, levels[l]);   /* first test : right after init */
            if (attach) {
                LZ4_resetStreamHC_fast(dictStream, levels[l]);
                ASSERT_EQ(LZ4_loadDictHC(dictStream, dict, 64 KB), 64 KB);
                LZ4_attach_HC_dictionary(stream, dictStream);
            }
            ASSERT_EQ(LZ4_saveDictHC(stream, saveBuffer, sizeof(saveBuffer)), 0);
            cSize = LZ4_compress_HC_continue(stream, src, dst, 64 KB, capacity);
            ASSERT(cSize > 0);
            ASSERT_MSG(LZ4_decompress_safe_usingDict(dst, decoded, cSize, 64 KB, dict, attach ? 64 KB : 0) == 64 KB
                       && !memcmp(decoded, src, 64 KB),
                       "level %i, dictionary %i : block compressed after LZ4_saveDictHC() is corrupted", levels[l], attach);
    }   }
}

TEST(compress_HC_destSize_longSequence)
{
    size_t const srcSize = 1 MB;
    size_t const targetSize = 4116;   /* size carefully selected to trigger an overflow */
    char* const src = (char*)UT_malloc(srcSize);
    char* const dst = (char*)UT_malloc(targetSize + 1);
    char const sentinel = 101;
    LZ4_streamHC_t* const stream = newStreamHC(3);
    int readSize = (int)srcSize;
    int cSize;
    memset(src, 0, srcSize);
    dst[targetSize] = sentinel;
    cSize = LZ4_compress_HC_destSize(stream, src, dst, &readSize, (int)targetSize, 3);
    ASSERT_EQ(cSize, targetSize);   /* fills dst exactly */
    ASSERT_EQ(dst[targetSize], sentinel);
    ASSERT_MSG(readSize >= 1045000, "doesn't compress enough (%i -> %i)", readSize, cSize);
}

/* a failed compression doesn't lose parameters */
TEST(favorDecompressionSpeed_afterFailedCompression)
{
    int const srcSize = 64 KB;
    int const capacity = LZ4_COMPRESSBOUND(srcSize);
    int const level = LZ4HC_CLEVEL_OPT_MIN;
    const char* const src = (const char*)UT_fixedSample((size_t)srcSize);   /* favorDecSpeed must change the output */
    char* const ref = (char*)UT_malloc((size_t)capacity);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    LZ4_streamHC_t* const stream = newStreamHC(level);
    int refSize, cSize;

    /* reference : favor decompression speed */
    LZ4_favorDecompressionSpeed(stream, 1);
    refSize = LZ4_compress_HC_continue(stream, src, ref, srcSize, capacity);
    ASSERT(refSize > 0);

    /* this test is only meaningful if favorDecSpeed changes the output */
    LZ4_resetStreamHC_fast(stream, level);
    LZ4_favorDecompressionSpeed(stream, 0);
    cSize = LZ4_compress_HC_continue(stream, src, dst, srcSize, capacity);
    ASSERT(cSize != refSize || memcmp(dst, ref, (size_t)cSize));

    /* failed compression */
    LZ4_resetStreamHC_fast(stream, level);
    LZ4_favorDecompressionSpeed(stream, 1);
    ASSERT_EQ(LZ4_compress_HC_continue(stream, src, dst, srcSize, refSize - 1), 0);

    /* next stream : must still favor decompression speed */
    LZ4_resetStreamHC_fast(stream, level);
    ASSERT_EQ(LZ4_compress_HC_continue(stream, src, dst, srcSize, capacity), refSize);
    ASSERT_MEMEQ(dst, ref, (size_t)refSize);
}

/* after a failed compression, the next stream compresses as from a new state */
TEST(resetStreamHC_fast_afterFailedCompression)
{
    static const int levels[] = { 2, LZ4HC_CLEVEL_DEFAULT, LZ4HC_CLEVEL_OPT_MIN, LZ4HC_CLEVEL_MAX };
    int const srcSize = 128 KB;
    int const half = srcSize / 2;
    int const capacity = LZ4_COMPRESSBOUND(srcSize);
    const char* const src = (const char*)UT_fixedSample((size_t)srcSize);
    char* const ref = (char*)UT_malloc((size_t)capacity);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    size_t l;
    for (l = 0; l < ARRAY_SIZE(levels); l++) {
        int const level = levels[l];
        LZ4_streamHC_t* const stream = newStreamHC(level);
        LZ4_streamHC_t* const twoBlocks = newStreamHC(level);
        int const refSize = LZ4_compress_HC(src, ref, srcSize, capacity, level);
        int secondSize;
        ASSERT(refSize > 0);
        ASSERT(LZ4_compress_HC_continue(twoBlocks, src, dst, half, capacity) > 0);
        secondSize = LZ4_compress_HC_continue(twoBlocks, src + half, dst, half, capacity);
        ASSERT(secondSize > 0);

        /* the second block of a stream fails at its end : its positions are inserted */
        ASSERT(LZ4_compress_HC_continue(stream, src, dst, half, capacity) > 0);
        ASSERT_EQ(LZ4_compress_HC_continue(stream, src + half, dst, half, secondSize - 1), 0);
        LZ4_resetStreamHC_fast(stream, level);
        ASSERT_EQ(LZ4_compress_HC_continue(stream, src, dst, srcSize, capacity), refSize);
        ASSERT_MEMEQ(dst, ref, (size_t)refSize);

        /* one-shot */
        ASSERT_EQ(LZ4_compress_HC_extStateHC_fastReset(stream, src, dst, srcSize, refSize - 1, level), 0);
        ASSERT_EQ(LZ4_compress_HC_extStateHC_fastReset(stream, src, dst, srcSize, capacity, level), refSize);
        ASSERT_MEMEQ(dst, ref, (size_t)refSize);
    }
}

TEST(compress_HC_continue_dictionary)
{
    int const dictSize = 64 KB;
    int const srcSize = 130 KB;
    const char* const dict = (const char*)UT_sample((size_t)(dictSize + srcSize));
    const char* const src = dict + dictSize;
    char* const dst = (char*)UT_malloc((size_t)srcSize);
    char* const decoded = (char*)UT_malloc((size_t)srcSize);
    size_t l;
    for (l = 0; l < ARRAY_SIZE(g_levels); l++) {
        LZ4_streamHC_t* const stream = newStreamHC(g_levels[l]);
        int cSize;
        LZ4_loadDictHC(stream, dict, dictSize);
        cSize = LZ4_compress_HC_continue(stream, src, dst, srcSize, srcSize - 1);
        ASSERT(cSize > 0);
        ASSERT_EQ(LZ4_decompress_safe_usingDict(dst, decoded, cSize, srcSize, dict, dictSize), srcSize);
        ASSERT_MEMEQ(decoded, src, (size_t)srcSize);
    }
}

/* 2 blocks : the 2nd one also references the 1st one */
TEST(compress_HC_continue_dictionary_twoBlocks)
{
    int const segSize = 65 KB;
    const char* const input = (const char*)UT_sample((size_t)(3 * segSize));   /* dictionary, block 1, block 2 */
    char* const dst = (char*)UT_malloc((size_t)(2 * segSize));
    char* const decoded = (char*)UT_malloc((size_t)(2 * segSize));
    size_t l;
    for (l = 0; l < ARRAY_SIZE(g_levels); l++) {
        LZ4_streamHC_t* const stream = newStreamHC(g_levels[l]);
        int c1, c2;
        LZ4_loadDictHC(stream, input, segSize);
        c1 = LZ4_compress_HC_continue(stream, input + segSize, dst, segSize, segSize - 1);
        ASSERT(c1 > 0);
        c2 = LZ4_compress_HC_continue(stream, input + 2 * segSize, dst + c1, segSize, segSize - 1);
        ASSERT(c2 > 0);
        ASSERT_EQ(LZ4_decompress_safe_usingDict(dst, decoded, c1, segSize, input, segSize), segSize);
        ASSERT_EQ(LZ4_decompress_safe_usingDict(dst + c1, decoded + segSize, c2, segSize, input, 2 * segSize), segSize);
        ASSERT_MEMEQ(decoded, input + segSize, (size_t)(2 * segSize));
    }
}

/* the dictionary doesn't immediately precede the input */
TEST(compress_HC_continue_remoteDictionary)
{
    int const dictSize = 32 KB;
    int const srcSize = 130 KB;
    const char* const dict = (const char*)UT_sample((size_t)(64 KB + srcSize));
    const char* const src = dict + 64 KB;
    char* const dst = (char*)UT_malloc((size_t)srcSize);
    char* const decoded = (char*)UT_malloc((size_t)srcSize);
    size_t l;
    for (l = 0; l < ARRAY_SIZE(g_levels); l++) {
        LZ4_streamHC_t* const stream = newStreamHC(g_levels[l]);
        int cSize;
        LZ4_loadDictHC(stream, dict, dictSize);
        cSize = LZ4_compress_HC_continue(stream, src, dst, srcSize, srcSize - 1);
        ASSERT(cSize > 0);
        ASSERT_EQ(LZ4_decompress_safe_usingDict(dst, decoded, cSize, srcSize, dict, dictSize), srcSize);
        ASSERT_MEMEQ(decoded, src, (size_t)srcSize);
    }
}

/* LZ4_loadDictHC() loads the last 64 KB at most, and returns their size.
 * The next block copies the end of the dictionary : it's decoded with the expected bytes as dictionary */
TEST(loadDictHC_returnedSize)
{
    static const int sizes[][2] = { { -1, 0 }, { 0, 0 }, { 4, 4 }, { 1 KB, 1 KB }, { 63 KB + 512, 63 KB + 512 },
                                    { 64 KB, 64 KB }, { 64 KB + 512, 64 KB }, { 100 KB, 64 KB } };
    static const int levels[] = { LZ4HC_CLEVEL_MIN, LZ4HC_CLEVEL_DEFAULT };   /* lz4mid, hash chain */
    int const srcSize = 16 KB;
    int const capacity = LZ4_COMPRESSBOUND(srcSize);
    const char* const dict = (const char*)UT_sample(100 KB);
    char* const src = (char*)UT_malloc((size_t)srcSize);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    char* const decoded = (char*)UT_malloc((size_t)srcSize);
    LZ4_streamHC_t* const stream = newStreamHC(LZ4HC_CLEVEL_DEFAULT);
    size_t l, i;
    for (l = 0; l < ARRAY_SIZE(levels); l++)
    for (i = 0; i < ARRAY_SIZE(sizes); i++) {
        int const dictEnd = MAX(sizes[i][0], 0);
        int loaded, cSize;
        memcpy(src, dict + MAX(dictEnd - srcSize, 0), (size_t)srcSize);
        LZ4_resetStreamHC_fast(stream, levels[l]);
        loaded = LZ4_loadDictHC(stream, dict, sizes[i][0]);
        ASSERT_MSG(loaded == sizes[i][1], "level %i, dictionary of %i bytes : %i loaded, instead of %i",
                   levels[l], sizes[i][0], loaded, sizes[i][1]);
        cSize = LZ4_compress_HC_continue(stream, src, dst, srcSize, capacity);
        ASSERT(cSize > 0);
        ASSERT_MSG(LZ4_decompress_safe_usingDict(dst, decoded, cSize, srcSize, dict + dictEnd - loaded, loaded) == srcSize
                   && !memcmp(decoded, src, (size_t)srcSize),
                   "level %i, dictionary of %i bytes : corrupted block", levels[l], sizes[i][0]);
    }
}

/* LZ4_saveDictHC() saves the last 64 KB of history at most, into a buffer of the requested size, and returns the saved size.
 * The next block, following the saved history or not, uses it as dictionary */
TEST(saveDictHC_sizes)
{
    static const int sizes[][2] = { { 3, 0 }, { 4, 4 }, { 1 KB, 1 KB }, { 63 KB + 512, 63 KB + 512 },
                                    { 64 KB, 64 KB }, { 64 KB + 512, 64 KB } };
    static const int levels[] = { LZ4HC_CLEVEL_MIN, LZ4HC_CLEVEL_DEFAULT, LZ4HC_CLEVEL_OPT_MIN };   /* lz4mid, hash chain, optimal parser */
    int const histSize = 100 KB;
    int const srcSize = 16 KB;
    int const capacity = LZ4_COMPRESSBOUND(histSize);
    const char* const input = (const char*)UT_sample((size_t)(histSize + srcSize));   /* history, then next block */
    char* const hist = (char*)UT_malloc((size_t)histSize);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    char* const decoded = (char*)UT_malloc((size_t)srcSize);
    LZ4_streamHC_t* const stream = newStreamHC(LZ4HC_CLEVEL_DEFAULT);
    size_t l, i;
    int contiguous;
    for (l = 0; l < ARRAY_SIZE(levels); l++)
    for (i = 0; i < ARRAY_SIZE(sizes); i++)
    for (contiguous = 0; contiguous <= 1; contiguous++) {
        /* saved history, followed by the next block if contiguous */
        char* const buffer = (char*)UT_malloc((size_t)(sizes[i][0] + (contiguous ? srcSize : 0)));
        char* src;
        int saved, cSize;
        memcpy(hist, input, (size_t)histSize);
        LZ4_resetStreamHC_fast(stream, levels[l]);
        ASSERT(LZ4_compress_HC_continue(stream, hist, dst, histSize, capacity) > 0);
        saved = LZ4_saveDictHC(stream, buffer, sizes[i][0]);
        ASSERT_MSG(saved == sizes[i][1], "level %i : %i bytes saved instead of %i, for %i requested",
                   levels[l], saved, sizes[i][1], sizes[i][0]);
        ASSERT_MEMEQ(buffer, input + histSize - saved, (size_t)saved);
        memset(hist, 0, (size_t)histSize);   /* former history is gone */
        src = contiguous ? buffer + saved : (char*)UT_malloc((size_t)srcSize);
        memcpy(src, input + histSize, (size_t)srcSize);
        cSize = LZ4_compress_HC_continue(stream, src, dst, srcSize, capacity);
        ASSERT(cSize > 0);
        ASSERT_MSG(LZ4_decompress_safe_usingDict(dst, decoded, cSize, srcSize, input + histSize - saved, saved) == srcSize
                   && !memcmp(decoded, input + histSize, (size_t)srcSize),
                   "level %i, %i bytes saved, contiguous %i : corrupted block", levels[l], saved, contiguous);
    }
}

/* on long sequences of zeros, LZ4_compress_HC_destSize() and LZ4_compress_destSize() produce the same output */
TEST(compress_HC_destSize_sameAsFast)
{
    int const srcCapacity = 3 MB;
    int const dstCapacity = 6 KB;
    int const dstStep = 43;
    int const dstMinLen = 33 + (int)(UT_rand() % (unsigned)dstStep);
    int const dstMaxLen = 5000;
    char const endMark = 0x5A;
    char* const src1 = (char*)UT_malloc((size_t)srcCapacity + 1);
    char* const src2 = (char*)UT_malloc((size_t)srcCapacity + 1);
    char* const dst1 = (char*)UT_malloc((size_t)dstCapacity + 1);
    char* const dst2 = (char*)UT_malloc((size_t)dstCapacity + 1);
    LZ4_streamHC_t* const stream = newStreamHC(LZ4HC_CLEVEL_DEFAULT);
    size_t l;
    int dlen, slen;
    ASSERT(dstCapacity > dstMaxLen);
    for (l = 0; l < ARRAY_SIZE(g_levels); l++)
    for (dlen = dstMinLen; dlen <= dstMaxLen; dlen += dstStep) {
        int const srcLen = (dlen - 10) * 255 + 24;
        if (srcLen + 10 >= srcCapacity) break;
        for (slen = srcLen - 3; slen <= srcLen + 3; slen++) {
            int srcSize1 = slen, srcSize2 = slen;
            int dSize1, dSize2;
            memset(src1, 0, (size_t)slen);
            memset(dst1, 0, (size_t)dlen);
            dst1[dlen] = endMark;
            dSize1 = LZ4_compress_destSize(src1, dst1, &srcSize1, dlen);
            ASSERT_EQ(dst1[dlen], endMark);   /* no overwrite beyond dst */
            ASSERT(dSize1 > 0 && dSize1 <= dlen);
            ASSERT(srcSize1 <= slen);
            ASSERT_EQ(LZ4_decompress_safe(dst1, src1, dSize1, srcCapacity), srcSize1);

            memset(src2, 0, (size_t)slen);
            memset(dst2, 0, (size_t)dlen);
            dst2[dlen] = endMark;
            LZ4_resetStreamHC(stream, g_levels[l]);
            dSize2 = LZ4_compress_HC_destSize(stream, src2, dst2, &srcSize2, dlen, g_levels[l]);
            ASSERT_EQ(dst2[dlen], endMark);
            ASSERT_EQ(dSize2, dSize1);
            ASSERT_EQ(srcSize2, srcSize1);
            ASSERT_MEMEQ(dst2, dst1, (size_t)dSize2);
            ASSERT_EQ(LZ4_decompress_safe(dst2, src1, dSize2, srcCapacity), srcSize1);
            ASSERT_MEMEQ(src1, src2, (size_t)srcSize1);
    }   }
}

/* LZ4_compress_HC_continue_destSize() can stop before the end of input :
 * the next block resumes from there, either in place, or from a refilled buffer
 * after saving history with LZ4_saveDictHC().
 * Each block is decoded as the continuation of the previous ones. */
TEST(compress_HC_continue_destSize_resume)
{
    static const int levels[] = { LZ4HC_CLEVEL_MIN, LZ4HC_CLEVEL_DEFAULT, LZ4HC_CLEVEL_OPT_MIN };   /* lz4mid, hash chain, optimal parser */
    int const srcSize = 256 KB;
    int const bufSize = 64 KB;
    int const dstBudget = 2 KB;
    const char* const src = (const char*)UT_sample((size_t)srcSize);
    char* const buf = (char*)UT_malloc((size_t)bufSize);
    char* const dict = (char*)UT_malloc(64 KB);
    char* const dst = (char*)UT_malloc((size_t)dstBudget);
    char* const decoded = (char*)UT_malloc((size_t)srcSize);
    size_t l;
    int refill;
    for (l = 0; l < ARRAY_SIZE(levels); l++)
    for (refill = 0; refill <= 1; refill++) {
        LZ4_streamHC_t* const stream = newStreamHC(levels[l]);
        LZ4_streamDecode_t dStream;
        int pos = 0, readPos = 0, bufFill = 0;
        LZ4_setStreamDecode(&dStream, NULL, 0);
        while (pos < srcSize) {
            const char* input = src + pos;
            int consumed = srcSize - pos;
            int cSize, dSize;
            if (refill) {
                int const toRead = (bufSize - bufFill < srcSize - readPos) ? bufSize - bufFill : srcSize - readPos;
                memcpy(buf + bufFill, src + readPos, (size_t)toRead);
                bufFill += toRead;
                readPos += toRead;
                input = buf;
                consumed = bufFill;
            }
            cSize = LZ4_compress_HC_continue_destSize(stream, input, dst, &consumed, dstBudget);
            ASSERT(cSize > 0 && consumed > 0);
            dSize = LZ4_decompress_safe_continue(&dStream, dst, decoded + pos, cSize, srcSize - pos);
            ASSERT_MSG(dSize == consumed && !memcmp(decoded + pos, src + pos, (size_t)consumed),
                       "level %i, refill %i : block at position %i is corrupted", levels[l], refill, pos);
            pos += consumed;
            if (refill) {
                LZ4_saveDictHC(stream, dict, 64 KB);
                memmove(buf, buf + consumed, (size_t)(bufFill - consumed));
                bufFill -= consumed;
    }   }   }
}

/* same, stopping within a long match (zeros) : the positions it referenced beyond the stop are dropped,
 * the next block doesn't find them as matches */
TEST(compress_HC_continue_destSize_resumeInLongMatch)
{
    static const int levels[] = { LZ4HC_CLEVEL_MIN, LZ4HC_CLEVEL_DEFAULT, LZ4HC_CLEVEL_OPT_MIN };   /* lz4mid, hash chain, optimal parser */
    int const srcSize = 1 MB;
    int const dstBudget = 2 KB;   /* a match of 1 MB takes ~4 KB */
    char* const src = (char*)UT_sample((size_t)srcSize);
    char* const dst = (char*)UT_malloc((size_t)dstBudget);
    char* const decoded = (char*)UT_malloc((size_t)srcSize);
    size_t l;
    memset(src + 4 KB, 0, (size_t)srcSize - 8 KB);
    for (l = 0; l < ARRAY_SIZE(levels); l++) {
        LZ4_streamHC_t* const stream = newStreamHC(levels[l]);
        LZ4_streamDecode_t dStream;
        int pos = 0;
        LZ4_setStreamDecode(&dStream, NULL, 0);
        while (pos < srcSize) {
            int consumed = srcSize - pos;
            int const cSize = LZ4_compress_HC_continue_destSize(stream, src + pos, dst, &consumed, dstBudget);
            ASSERT(cSize > 0 && consumed > 0);
            ASSERT_MSG(LZ4_decompress_safe_continue(&dStream, dst, decoded + pos, cSize, srcSize - pos) == consumed
                       && !memcmp(decoded + pos, src + pos, (size_t)consumed),
                       "level %i : block at position %i is corrupted", levels[l], pos);
            pos += consumed;
    }   }
}

/* a stream reset after more than 1 GB clears its tables : the next stream compresses as a new one */
TEST(resetStreamHC_fast_beyond1GB)
{
    enum { blockSize = 64 KB, ringSize = 16 };
    int const nbBlocks = (int)((1U << 30) / blockSize) + 1;
    int const capacity = LZ4_COMPRESSBOUND(blockSize);
    int const level = LZ4HC_CLEVEL_MIN + 1;   /* hash chain */
    char* const ring = (char*)UT_malloc((size_t)ringSize * blockSize);
    char* const src = (char*)UT_sample(blockSize);
    char* const ref = (char*)UT_malloc((size_t)capacity);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    LZ4_streamHC_t* const stream = newStreamHC(LZ4HC_CLEVEL_MIN);   /* lz4mid : fast on zeros */
    int n, refSize;
    memset(ring, 0, (size_t)ringSize * blockSize);
    for (n = 0; n < nbBlocks; n++)
        ASSERT(LZ4_compress_HC_continue(stream, ring + (n % ringSize) * blockSize, dst, blockSize, capacity) > 0);
    memset(src + 16 KB, 0, 16 KB);   /* zeros were referenced before the reset */
    refSize = LZ4_compress_HC(src, ref, blockSize, capacity, level);
    ASSERT(refSize > 0);
    LZ4_resetStreamHC_fast(stream, level);
    ASSERT_EQ(LZ4_compress_HC_continue(stream, src, dst, blockSize, capacity), refSize);
    ASSERT_MEMEQ(dst, ref, (size_t)refSize);
}

/* beyond 2 GB, a stream reloads its last 64 KB of history as dictionary, and its indexes restart.
 * Separate blocks only reference the previous block (63.5 KB) : each one starts with the end of the one before,
 * which must not be referenced (decoding fails).
 * Zero blocks at level lz4mid (fast), then blocks of text at a hash chain level, from shortly before 2 GB */
TEST(compress_HC_continue_beyond2GB)
{
    enum { blockSize = 63 KB + 512, ringSize = 16, copySize = 500 };
    int const nbBlocks = (int)((2U << 30) / blockSize) + 4;
    int const firstSample = nbBlocks - 8;
    int const capacity = LZ4_COMPRESSBOUND(blockSize);
    char* const ring = (char*)UT_malloc((size_t)ringSize * (blockSize + 1));   /* separate blocks : 1 byte apart */
    char* const dst = (char*)UT_malloc((size_t)capacity);
    char* const history = (char*)UT_malloc(3 * blockSize);   /* contents of the 2 previous blocks, then of the current one */
    char* const decoded = (char*)UT_malloc(blockSize);
    LZ4_streamHC_t* const stream = newStreamHC(LZ4HC_CLEVEL_MIN);
    unsigned seed = 1;
    int n, i;
    memset(ring, 0, (size_t)ringSize * (blockSize + 1));
    memset(history, 0, 3 * blockSize);
    for (n = 0; n < nbBlocks; n++) {
        char* const block = ring + (n % ringSize) * (blockSize + 1);
        char* const current = history + 2 * blockSize;
        int cSize;
        if (n == firstSample) LZ4_setCompressionLevel(stream, LZ4HC_CLEVEL_MIN + 1);
        if (n >= firstSample) {   /* end of the block before the previous one, then text with 1 byte in 8 random */
            memcpy(current, history + blockSize - copySize, copySize);
            for (i = copySize; i < blockSize; i++) {
                seed = seed * 1103515245u + 12345u;
                current[i] = ((seed >> 16) % 8 == 0) ? (char)(seed >> 8) : "lorem ipsum dolor sit amet "[i % 27];
            }
            memcpy(block, current, blockSize);
        }
        cSize = LZ4_compress_HC_continue(stream, block, dst, blockSize, capacity);
        ASSERT(cSize > 0);
        if (n >= firstSample) {
            int const r = LZ4_decompress_safe_usingDict(dst, decoded, cSize, blockSize, history + blockSize, blockSize);
            ASSERT_MSG(r == blockSize && !memcmp(decoded, current, blockSize),
                       "block %i (%u MB) : corrupted", n, (unsigned)((size_t)n * blockSize >> 20));
            memmove(history, history + blockSize, 2 * blockSize);
        }
    }
}



/*-************************************
*  Dictionaries and ring buffers
**************************************/
#define HC_INPUT_SIZE (196 KB)
#define HC_RING_SIZE (8 KB)

/* attached dictionary, with a first block smaller than its copy threshold : the dictionary is referenced.
 * Next blocks, smaller or larger than this threshold :
 * contiguous blocks reference the dictionary and the previous blocks (until 64 KB of input),
 * separate blocks only reference the previous block.
 * The dictionary is prepared at the level of the stream, or at a level of the other kind (lz4mid / hash chain) */
TEST(attach_HC_dictionary_smallBlocks)
{
    static const int levels[][2] = { { 2, 2 }, { 2, 9 }, { 9, 9 }, { 9, 2 }, { 10, 10 }, { 12, 12 } };   /* stream, dictionary */
    int const dictSize = 64 KB;
    int const smallSize = 1000;   /* smaller than the copy threshold of all levels */
    int const largeSize = 5000;   /* larger */
    int const nbBlocks = 40;   /* beyond 64 KB */
    int const capacity = LZ4_COMPRESSBOUND(largeSize);
    const char* const dict = (const char*)UT_sample((size_t)dictSize);
    char* const blocks = (char*)UT_malloc((size_t)(nbBlocks * (largeSize + 1)));   /* separate blocks : 1 byte apart */
    char* const decoded = (char*)UT_malloc((size_t)(nbBlocks * (largeSize + 1)));
    char* const dst = (char*)UT_malloc((size_t)capacity);
    LZ4_streamHC_t* const dictStream = newStreamHC(LZ4HC_CLEVEL_DEFAULT);
    LZ4_streamHC_t* const stream = newStreamHC(LZ4HC_CLEVEL_DEFAULT);
    size_t l;
    int separate, b, p, pos;
    for (l = 0; l < ARRAY_SIZE(levels); l++)
    for (separate = 0; separate <= 1; separate++) {
        LZ4_streamDecode_t sd;
        LZ4_resetStreamHC_fast(dictStream, levels[l][1]);
        ASSERT_EQ(LZ4_loadDictHC(dictStream, dict, dictSize), dictSize);
        LZ4_resetStreamHC_fast(stream, levels[l][0]);
        LZ4_attach_HC_dictionary(stream, dictStream);
        LZ4_setStreamDecode(&sd, dict, dictSize);
        for (b = 0, pos = 0; b < nbBlocks; b++) {
            int const blockSize = (b % 4 == 3) ? largeSize : smallSize;
            int const pieceSize = blockSize / 4;
            char* const block = blocks + pos;
            int cSize;
            for (p = 0; p < 4; p++)   /* pieces of the dictionary */
                memcpy(block + p * pieceSize, dict + ((b * 4 + p) * 7919) % (dictSize - pieceSize), (size_t)pieceSize);
            cSize = LZ4_compress_HC_continue(stream, block, dst, blockSize, capacity);
            ASSERT(cSize > 0);
            ASSERT_MSG(LZ4_decompress_safe_continue(&sd, dst, decoded + pos, cSize, blockSize) == blockSize
                       && !memcmp(decoded + pos, block, (size_t)blockSize),
                       "level %i, dictionary level %i, separate %i : block %i is corrupted", levels[l][0], levels[l][1], separate, b);
            pos += blockSize + separate;
    }   }
}

/* runs of a byte, measured by the pattern analysis of levels >= 9, at the boundary between dictionary and input :
 * runs of the dictionary, one of them continuing into the input, and runs of the input, one of them continuing back into the dictionary.
 * The input follows the dictionary (prefix), or not (external dictionary) */
TEST(compress_HC_continue_runsAcrossDictionary)
{
    static const int levels[] = { LZ4HC_CLEVEL_DEFAULT, LZ4HC_CLEVEL_OPT_MIN, LZ4HC_CLEVEL_MAX };
    static const int dictRuns[][2] = { { 30 KB, 6000 }, { 56 KB, 8 KB } };   /* position, length */
    static const int srcRuns[][2] = { { 0, 3000 }, { 8000, 7000 }, { 20000, 300 }, { 24300, 12000 }, { 40000, 5000 } };
    int const dictSize = 64 KB;
    int const srcSize = 64 KB;
    int const capacity = LZ4_COMPRESSBOUND(srcSize);
    const char* const sample = (const char*)UT_sample((size_t)(dictSize + srcSize));
    char* const buffer = (char*)UT_malloc((size_t)(dictSize + srcSize));   /* dictionary, then input if it's a prefix */
    char* const separateSrc = (char*)UT_malloc((size_t)srcSize);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    char* const decoded = (char*)UT_malloc((size_t)srcSize);
    LZ4_streamHC_t* const stream = newStreamHC(LZ4HC_CLEVEL_DEFAULT);
    size_t l, r;
    int prefix;
    memcpy(buffer, sample, (size_t)(dictSize + srcSize));
    for (r = 0; r < ARRAY_SIZE(dictRuns); r++) memset(buffer + dictRuns[r][0], 'a', (size_t)dictRuns[r][1]);
    for (r = 0; r < ARRAY_SIZE(srcRuns); r++) memset(buffer + dictSize + srcRuns[r][0], 'a', (size_t)srcRuns[r][1]);
    memcpy(separateSrc, buffer + dictSize, (size_t)srcSize);
    for (l = 0; l < ARRAY_SIZE(levels); l++)
    for (prefix = 0; prefix <= 1; prefix++) {
        const char* const src = prefix ? buffer + dictSize : separateSrc;
        int cSize;
        LZ4_resetStreamHC_fast(stream, levels[l]);
        ASSERT_EQ(LZ4_loadDictHC(stream, buffer, dictSize), dictSize);
        cSize = LZ4_compress_HC_continue(stream, src, dst, srcSize, capacity);
        ASSERT(cSize > 0);
        ASSERT_MSG(LZ4_decompress_safe_usingDict(dst, decoded, cSize, srcSize, buffer, dictSize) == srcSize
                   && !memcmp(decoded, src, (size_t)srcSize),
                   "level %i, prefix %i : corrupted block", levels[l], prefix);
    }
}

/* successive segments, each one using the previous one as dictionary, at another place */
TEST(compress_HC_continue_successiveDictionaries)
{
    const char* const src = (const char*)UT_sample(HC_INPUT_SIZE);
    char* const decoded = (char*)UT_malloc(HC_INPUT_SIZE + HC_INPUT_SIZE / 16);   /* segments separated by 1 byte */
    char* const dst = (char*)UT_malloc((size_t)LZ4_COMPRESSBOUND(8 KB));
    size_t l;
    for (l = 0; l < ARRAY_SIZE(g_levels); l++) {
        LZ4_streamHC_t* const stream = newStreamHC(g_levels[l]);
        const char* dict = src + 3;
        size_t dictSize = UT_rand() & 8191;
        char* out = decoded;
        size_t segStart = dictSize + 7;
        size_t segSize = UT_rand() & 8191;
        int segNb = 1;
        LZ4_loadDictHC(stream, dict, (int)dictSize);
        while (segStart + segSize < HC_INPUT_SIZE) {
            int const cSize = LZ4_compress_HC_continue(stream, src + segStart, dst, (int)segSize, LZ4_compressBound((int)segSize));
            ASSERT_MSG(cSize > 0, "level %i, segment %i : compression failed", g_levels[l], segNb);
            ASSERT_MSG(LZ4_decompress_safe_usingDict(dst, out, cSize, (int)segSize, dict, (int)dictSize) == (int)segSize,
                       "level %i, segment %i : decompression failed", g_levels[l], segNb);
            ASSERT_MEMEQ(out, src + segStart, segSize);
            dict = out;
            dictSize = segSize;
            out += segSize + 1;
            segNb++;
            segStart += segSize + (UT_rand() & 0xF) + 1;
            segSize = UT_rand() & 8191;
            ASSERT((size_t)(out - decoded) + segSize <= HC_INPUT_SIZE + HC_INPUT_SIZE / 16);
    }   }
}

/* the encoder's ring buffer is also used by the decoders */
TEST(compress_HC_continue_ringBuffer)
{
    unsigned const maxMessageSizeMask = (1 << 10) - 1;
    unsigned const srcSize = 130 KB;
    unsigned const dBufferSize = HC_RING_SIZE + maxMessageSizeMask;
    const char* const src = (const char*)UT_sample(srcSize);
    char* const ring = (char*)UT_malloc(HC_RING_SIZE);
    int const capacity = LZ4_COMPRESSBOUND(1 KB);
    char* const cBuffer = (char*)UT_malloc((size_t)capacity);
    char* const dSafe = (char*)UT_malloc(dBufferSize);
    char* const dFast = (char*)UT_malloc(dBufferSize);
    size_t l;
    for (l = 0; l < ARRAY_SIZE(g_levels); l++) {
        LZ4_streamHC_t* const stream = newStreamHC(g_levels[l]);
        LZ4_streamDecode_t sdSafe, sdFast;
        unsigned messageSize = (UT_rand() & maxMessageSizeMask) + 1;
        unsigned iNext = 0, rNext = 0, dNext = 0;
        LZ4_setStreamDecode(&sdSafe, NULL, 0);
        LZ4_setStreamDecode(&sdFast, NULL, 0);
        while (iNext + messageSize < srcSize) {
            int cSize;
            memcpy(ring + rNext, src + iNext, messageSize);
            cSize = LZ4_compress_HC_continue(stream, ring + rNext, cBuffer, (int)messageSize, capacity);
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
            if (rNext + messageSize > HC_RING_SIZE) rNext = 0;
            if (dNext + messageSize > dBufferSize) dNext = 0;
        }
        UT_release(stream);
    }
}

/* Ring buffer test : non synchronized decoder.
 * This test uses the minimum amount of memory required to setup a decoding ring buffer
 * while being unsynchronized with the encoder
 * (no assumption done on how the data is encoded, it just follows the LZ4 format specification).
 * This size is documented in lz4.h, and is LZ4_decoderRingBufferSize(maxBlockSize). */
TEST(decompress_continue_unsynchronizedRing)
{
    int const maxMessageSize = 1 << 12;
    int const maxMessageSizeMask = maxMessageSize - 1;
    int const dBufferSize = LZ4_decoderRingBufferSize(maxMessageSize);
    int const firstSize = dBufferSize - (maxMessageSize-1);   /* fills the decoding ring buffer once ; larger than maxMessageSize */
    char* const src = (char*)UT_sample(HC_INPUT_SIZE);
    char* const ringSafe = (char*)UT_malloc((size_t)dBufferSize);
    char* const ringFast = (char*)UT_malloc((size_t)dBufferSize);   /* used by LZ4_decompress_fast_continue() */
    int const capacity = LZ4_COMPRESSBOUND(firstSize);
    char* const cBuffer = (char*)UT_malloc((size_t)capacity);
    size_t l;
    ASSERT(firstSize >= 65535);
    ASSERT((size_t)(firstSize + 1 + maxMessageSize) <= HC_INPUT_SIZE);
    for (l = 0; l < ARRAY_SIZE(g_levels); l++) {
        LZ4_streamHC_t* const stream = newStreamHC(g_levels[l]);
        LZ4_streamDecode_t sdSafe, sdFast;
        int messageSize = firstSize, iNext = 0, dNext = 0, cSize;
        unsigned totalMessageSize = 0;
        LZ4_setStreamDecode(&sdSafe, NULL, 0);
        LZ4_setStreamDecode(&sdFast, NULL, 0);

        /* first block */
        cSize = LZ4_compress_HC_continue(stream, src + iNext, cBuffer, messageSize, capacity);
        ASSERT(cSize > 0);
        ASSERT_EQ(LZ4_decompress_safe_continue(&sdSafe, cBuffer, ringSafe + dNext, cSize, messageSize), messageSize);
        ASSERT_MEMEQ(ringSafe + dNext, src + iNext, (size_t)messageSize);
        ASSERT_EQ(LZ4_decompress_fast_continue(&sdFast, cBuffer, ringFast + dNext, messageSize), cSize);
        ASSERT_MEMEQ(ringFast + dNext, src + iNext, (size_t)messageSize);

        /* second message : restarts from the beginning of the ring */
        dNext += messageSize;
        totalMessageSize += (unsigned)messageSize;
        messageSize = maxMessageSize;
        iNext = firstSize + 1;
        memcpy(src + iNext, src + (firstSize - 65535), (size_t)messageSize);  /* will generate a match at max distance == 65535 */
        ASSERT(dNext + messageSize > dBufferSize);
        dNext = 0;

        while (totalMessageSize < 9 MB) {
            cSize = LZ4_compress_HC_continue(stream, src + iNext, cBuffer, messageSize, capacity);
            ASSERT(cSize > 0);

            /* decoding works without knowing messageSize, under assumption that messageSize <= maxMessageSize */
            ASSERT(dBufferSize - dNext >= maxMessageSize);
            ASSERT_EQ(LZ4_decompress_safe_continue(&sdSafe, cBuffer, ringSafe + dNext, cSize, dBufferSize - dNext), messageSize);
            ASSERT_MEMEQ(ringSafe + dNext, src + iNext, (size_t)messageSize);
            /* LZ4_decompress_fast_continue(), in its own ring buffer */
            ASSERT_EQ(LZ4_decompress_fast_continue(&sdFast, cBuffer, ringFast + dNext, messageSize), cSize);
            ASSERT_MEMEQ(ringFast + dNext, src + iNext, (size_t)messageSize);

            /* prepare next message */
            dNext += messageSize;
            totalMessageSize += (unsigned)messageSize;
            messageSize = (int)(UT_rand() & (unsigned)maxMessageSizeMask) + 1;
            iNext = (int)(UT_rand() & 65535);
            if (dNext + maxMessageSize > dBufferSize) dNext = 0;
        }
        UT_release(stream);
    }
}

/* extended states (LZ4_STREAMHC_EXTSIZE) include the temporary buffer of levels >= LZ4HC_CLEVEL_OPT_MIN : same output.
 * The extension is preserved by resets and dictionary loading, and never taken from an attached dictionary.
 * @ext : the bytes beyond sizeof(LZ4_streamHC_t), poisoned to detect their use */
#define EXT_BYTES (LZ4_STREAMHC_EXTSIZE - sizeof(LZ4_streamHC_t))
static int extEmployed(const char* state) { size_t i; for (i = 0; i < EXT_BYTES; i++) if (state[sizeof(LZ4_streamHC_t) + i] != 0x5A) return 1; return 0; }
static void extPoison(char* state) { memset(state + sizeof(LZ4_streamHC_t), 0x5A, EXT_BYTES); }

TEST(compress_HC_extendedState)
{
    int const srcSize = 130 KB;
    int const capacity = LZ4_compressBound(srcSize);
    int const level = LZ4HC_CLEVEL_MAX;
    const char* const src = (const char*)UT_sample((size_t)srcSize);
    char* const ref = (char*)UT_malloc((size_t)capacity);
    char* const dst = (char*)UT_malloc((size_t)capacity);
    char* const ext = (char*)UT_malloc(LZ4_STREAMHC_EXTSIZE);
    char* const extDict = (char*)UT_malloc(LZ4_STREAMHC_EXTSIZE);
    char* const plain = (char*)UT_malloc(sizeof(LZ4_streamHC_t));   /* nothing beyond : ASan detects any use of an extension */
    LZ4_streamHC_t* const s = LZ4_initStreamHC(ext, LZ4_STREAMHC_EXTSIZE);
    int refSize;
    ASSERT(s != NULL);
    refSize = LZ4_compress_HC_extStateHC(plain, src, ref, srcSize, capacity, level);
    ASSERT(refSize > 0);

    /* employed at optimal levels only */
    extPoison(ext);
    ASSERT_EQ(LZ4_compress_HC_extStateHC_fastReset(s, src, dst, srcSize, capacity, level), refSize);
    ASSERT_MEMEQ(dst, ref, (size_t)refSize);
    ASSERT(extEmployed(ext));
    extPoison(ext);
    ASSERT(LZ4_compress_HC_extStateHC_fastReset(s, src, dst, srcSize, capacity, LZ4HC_CLEVEL_DEFAULT) > 0);
    ASSERT(!extEmployed(ext));

    /* preserved by LZ4_loadDictHC(), and by the reset after a failed compression */
    LZ4_resetStreamHC_fast(s, level);
    ASSERT_EQ(LZ4_loadDictHC(s, src, 64 KB), 64 KB);
    extPoison(ext);
    ASSERT(LZ4_compress_HC_continue(s, src + 64 KB, dst, srcSize - 64 KB, capacity) > 0);
    ASSERT(extEmployed(ext));
    ASSERT_EQ(LZ4_compress_HC_continue(s, src, dst, srcSize, 1), 0);   /* failure */
    LZ4_resetStreamHC_fast(s, level);
    extPoison(ext);
    ASSERT_EQ(LZ4_compress_HC_continue(s, src, dst, srcSize, capacity), refSize);
    ASSERT(extEmployed(ext));

    /* attached dictionary, copied at the first block : the extension belongs to the working state */
    {   LZ4_streamHC_t* const d = LZ4_initStreamHC(extDict, LZ4_STREAMHC_EXTSIZE);
        LZ4_streamHC_t* const p = LZ4_initStreamHC(plain, sizeof(LZ4_streamHC_t));
        int dSize;
        LZ4_setCompressionLevel(d, level);
        ASSERT_EQ(LZ4_loadDictHC(d, src + 32 KB, 64 KB), 64 KB);
        LZ4_resetStreamHC_fast(p, level);
        LZ4_attach_HC_dictionary(p, d);
        dSize = LZ4_compress_HC_continue(p, src, ref, srcSize, capacity);   /* plain working state */
        ASSERT(dSize > 0);
        LZ4_resetStreamHC_fast(s, level);
        LZ4_attach_HC_dictionary(s, d);
        extPoison(ext);
        ASSERT_EQ(LZ4_compress_HC_continue(s, src, dst, srcSize, capacity), dSize);   /* extended working state */
        ASSERT_MEMEQ(dst, ref, (size_t)dSize);
        ASSERT(extEmployed(ext));
    }

    /* LZ4_compress_HC_extStateHC() initializes a state of LZ4_sizeofStateHC() bytes */
    extPoison(ext);
    ASSERT(LZ4_compress_HC_extStateHC(ext, src, dst, srcSize, capacity, level) > 0);
    ASSERT(!extEmployed(ext));
}
