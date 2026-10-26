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
#include <string.h>     /* memset, memcpy, memcmp */
#define LZ4_DISABLE_DEPRECATE_WARNINGS   /* LZ4_createHC, LZ4_compressHC2_continue */
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

/* a successful compression leaves the stream clean */
#define ASSERT_CLEAN(stream) ASSERT(!(stream)->internal_donotuse.dirty)


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
        ASSERT_CLEAN(stream);
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

/* level 2 : the match tested at ip+1 must not be ip itself, inside runs */
TEST(compress_HC_level2_runs)
{
    char runs[2 + 4096];   /* 0xFF 0x00, then runs of 10 x 0xFF and 10 x 0x00 */
    int const capacity = LZ4_COMPRESSBOUND(sizeof(runs));
    char* const dst = (char*)UT_malloc((size_t)capacity);
    int i, cSize;
    runs[0] = (char)0xFF; runs[1] = 0;
    for (i = 0; i < 4096; i++) runs[2 + i] = (i % 20) < 10 ? (char)0xFF : 0;
    cSize = LZ4_compress_HC(runs, dst, (int)sizeof(runs), capacity, 2);
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

/* a failed compression marks the state as dirty, which is fully re-initialized at next reset */
TEST(favorDecompressionSpeed_afterFailedCompression)
{
    int const srcSize = 64 KB;
    int const capacity = LZ4_COMPRESSBOUND(srcSize);
    int const level = LZ4HC_CLEVEL_OPT_MIN;
    const char* const src = (const char*)UT_sample((size_t)srcSize);
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
        ASSERT_CLEAN(stream);
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
        ASSERT_CLEAN(stream);
        c2 = LZ4_compress_HC_continue(stream, input + 2 * segSize, dst + c1, segSize, segSize - 1);
        ASSERT(c2 > 0);
        ASSERT_CLEAN(stream);
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
        ASSERT_CLEAN(stream);
        ASSERT_EQ(LZ4_decompress_safe_usingDict(dst, decoded, cSize, srcSize, dict, dictSize), srcSize);
        ASSERT_MEMEQ(decoded, src, (size_t)srcSize);
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
    char const endMark = (char)0x88;
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

