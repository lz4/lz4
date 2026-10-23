/*
    unittest_lz4frame - unit tests of lz4frame
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
#include <stdlib.h>     /* malloc, calloc, free */
#include <limits.h>     /* INT_MIN */
#define LZ4F_STATIC_LINKING_ONLY
#include "lz4frame.h"
#define LZ4_STATIC_LINKING_ONLY  /* LZ4_ACCELERATION_MAX */
#include "lz4.h"
#include "datagen.h"    /* RDG_genBuffer */

#define KB *(1U<<10)
#define MB *(1U<<20)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))


/*-************************************
*  Helpers
**************************************/
/* LZ4F_OK() : fails the test if @r is an error code
 * @return : @r */
#define LZ4F_OK(r) checkLZ4F((r), #r, __FILE__, __LINE__)
static size_t checkLZ4F(size_t r, const char* expr, const char* file, int line)
{
    if (LZ4F_isError(r)) UT_fail(file, line, "%s \n  error : %s", expr, LZ4F_getErrorName(r));
    return r;
}

/* ASSERT_LZ4F_ERROR() : fails the test unless @r is the error code @code */
#define ASSERT_LZ4F_ERROR(r, code) checkLZ4FError((r), code, #r, #code, __FILE__, __LINE__)
static void checkLZ4FError(size_t r, LZ4F_errorCodes code,
                           const char* expr, const char* codeName, const char* file, int line)
{
    if (LZ4F_getErrorCode(r) != code)
        UT_fail(file, line, "%s \n  expected : %s \n  result : %s", expr, codeName,
                LZ4F_isError(r) ? LZ4F_getErrorName(r) : "no error");
}

/* @return : @size bytes of compressible data, released at the end of the test */
static void* sample(size_t size)
{
    void* const buffer = UT_malloc(size);
    RDG_genBuffer(buffer, size, 0.5, 0.0, UT_rand());
    return buffer;
}

static void freeCctx(void* cctx) { LZ4F_freeCompressionContext((LZ4F_cctx*)cctx); }
static void freeDctx(void* dctx) { LZ4F_freeDecompressionContext((LZ4F_dctx*)dctx); }
static void freeCDict(void* cdict) { LZ4F_freeCDict((LZ4F_CDict*)cdict); }

/* contexts, released at the end of the test */
static LZ4F_cctx* newCctx(void)
{
    LZ4F_cctx* cctx = NULL;
    LZ4F_OK(LZ4F_createCompressionContext(&cctx, LZ4F_VERSION));
    return (LZ4F_cctx*)UT_defer(freeCctx, cctx);
}

static LZ4F_dctx* newDctx(void)
{
    LZ4F_dctx* dctx = NULL;
    LZ4F_OK(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION));
    return (LZ4F_dctx*)UT_defer(freeDctx, dctx);
}

/* allocator which fails while *state != 0 */
static void* failableMalloc(void* state, size_t s) { return *(const int*)state ? NULL : malloc(s); }
static void* failableCalloc(void* state, size_t s) { return *(const int*)state ? NULL : calloc(1, s); }
static void failableFree(void* state, void* p) { (void)state; free(p); }

/* compresses @src into a frame, with LZ4F_compressBegin(), or LZ4F_compressBegin_usingDict() when @dict != NULL,
 * followed by LZ4F_compressUpdate() and LZ4F_compressEnd().
 * @return : frame size, or an error code */
static size_t streamFrame(LZ4F_cctx* cctx, void* dst, size_t dstCapacity,
                          const void* src, size_t srcSize,
                          const void* dict, size_t dictSize, const LZ4F_preferences_t* prefs)
{
    char* const ostart = (char*)dst;
    size_t pos, r;
    pos = dict ? LZ4F_compressBegin_usingDict(cctx, dst, dstCapacity, dict, dictSize, prefs)
               : LZ4F_compressBegin(cctx, dst, dstCapacity, prefs);
    if (LZ4F_isError(pos)) return pos;
    r = LZ4F_compressUpdate(cctx, ostart + pos, dstCapacity - pos, src, srcSize, NULL);
    if (LZ4F_isError(r)) return r;
    pos += r;
    r = LZ4F_compressEnd(cctx, ostart + pos, dstCapacity - pos, NULL);
    if (LZ4F_isError(r)) return r;
    return pos + r;
}

/* fails the test unless @frame decodes entirely into @expected, using @dict */
static void assertDecodesTo(const void* frame, size_t frameSize,
                            const void* dict, size_t dictSize,
                            const void* expected, size_t expectedSize)
{
    void* const decoded = UT_malloc(expectedSize);
    size_t dSize = expectedSize;
    size_t cSize = frameSize;
    ASSERT_EQ(LZ4F_OK(LZ4F_decompress_usingDict(newDctx(), decoded, &dSize, frame, &cSize, dict, dictSize, NULL)), 0);
    ASSERT_EQ(cSize, frameSize);
    ASSERT_EQ(dSize, expectedSize);
    ASSERT_MEMEQ(decoded, expected, expectedSize);
}

/* fails the test unless decoding @frame requires a dictionary */
static void assertRequiresDict(const void* frame, size_t frameSize, size_t decodedSize)
{
    void* const decoded = UT_malloc(decodedSize);
    size_t dSize = decodedSize;
    size_t cSize = frameSize;
    ASSERT(LZ4F_isError(LZ4F_decompress(newDctx(), decoded, &dSize, frame, &cSize, NULL)));
}

static const int g_invalidBlockSizeIDs[] = { 1, 2, 3, 8, 9 };
static const int g_validBlockSizeIDs[] = { 0 /* == default */, 4, 5, 6, 7 };


/*-************************************
*  Tests
**************************************/
TEST(compressBound_emptyInput)
{
    ASSERT(LZ4F_compressBound(0, NULL) >= 64 KB);   /* may have to flush a full block */
}

TEST(compressBound_autoFlush)
{
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    prefs.autoFlush = 1;
    ASSERT(LZ4F_compressBound(1 KB, &prefs) <= 64 KB);
}

TEST(compressBound_noAutoFlush)
{
    LZ4F_preferences_t const prefs = LZ4F_INIT_PREFERENCES;
    ASSERT(LZ4F_compressBound(1 KB, &prefs) >= 64 KB);
}

TEST(getBlockSize)
{
    unsigned id;
    for (id = LZ4F_max64KB; id <= LZ4F_max4MB; id++)
        ASSERT_EQ(LZ4F_OK(LZ4F_getBlockSize((LZ4F_blockSizeID_t)id)), (size_t)1 << (2*id + 8));
    ASSERT_LZ4F_ERROR(LZ4F_getBlockSize((LZ4F_blockSizeID_t)3), LZ4F_ERROR_maxBlockSize_invalid);
    ASSERT_LZ4F_ERROR(LZ4F_getBlockSize((LZ4F_blockSizeID_t)8), LZ4F_ERROR_maxBlockSize_invalid);
}

/* #157 */
TEST(getFrameInfo_emptyFrame)
{
    char const src[1] = { 0 };
    size_t const capacity = LZ4F_compressFrameBound(0, NULL);
    char* const frame = (char*)UT_malloc(capacity);
    size_t const frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, 0, NULL));
    size_t headerSize, consumed;
    LZ4F_frameInfo_t frameInfo;
    ASSERT(frameSize >= LZ4F_MIN_SIZE_TO_KNOW_HEADER_LENGTH);
    headerSize = LZ4F_OK(LZ4F_headerSize(frame, LZ4F_MIN_SIZE_TO_KNOW_HEADER_LENGTH));
    consumed = headerSize;
    LZ4F_OK(LZ4F_getFrameInfo(newDctx(), &frameInfo, frame, &consumed));
    ASSERT_EQ(consumed, headerSize);   /* header size is exact */
}

TEST(frameHeader_dictID)
{
    char header[LZ4F_HEADER_SIZE_MAX];
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    LZ4F_frameInfo_t frameInfo;
    size_t headerSize;
    prefs.frameInfo.dictID = 0x99;
    headerSize = LZ4F_OK(LZ4F_compressBegin(newCctx(), header, sizeof(header), &prefs));
    LZ4F_OK(LZ4F_getFrameInfo(newDctx(), &frameInfo, header, &headerSize));
    ASSERT_EQ(frameInfo.dictID, 0x99);
}

/* acceleration (-level + 1) is capped at LZ4_ACCELERATION_MAX,
 * so all levels below the cap must produce the same frame, including INT_MIN and INT_MIN+1 */
TEST(compressFrame_veryNegativeLevels)
{
    static const int levels[] = { -LZ4_ACCELERATION_MAX /* cap - 1 */, INT_MIN + 1, INT_MIN };
    size_t const srcSize = 2 MB;
    const void* const src = sample(srcSize);
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    void* const ref = UT_malloc(capacity);
    void* const frame = UT_malloc(capacity);
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t refSize, n;
    prefs.compressionLevel = 1 - LZ4_ACCELERATION_MAX;
    refSize = LZ4F_OK(LZ4F_compressFrame(ref, capacity, src, srcSize, &prefs));
    for (n = 0; n < ARRAY_SIZE(levels); n++) {
        prefs.compressionLevel = levels[n];
        ASSERT_EQ(LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, srcSize, &prefs)), refSize);
        ASSERT_MEMEQ(frame, ref, refSize);
    }
}

TEST(compressFrame_validBlockSizeIDs)
{
    size_t const srcSize = 256 KB;
    const void* const src = sample(srcSize);
    size_t n;
    for (n = 0; n < ARRAY_SIZE(g_validBlockSizeIDs); n++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        size_t capacity, frameSize;
        void* frame;
        prefs.frameInfo.blockSizeID = (LZ4F_blockSizeID_t)g_validBlockSizeIDs[n];
        capacity = LZ4F_compressFrameBound(srcSize, &prefs);
        frame = UT_malloc(capacity);
        frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, srcSize, &prefs));
        assertDecodesTo(frame, frameSize, NULL, 0, src, srcSize);
    }
}

TEST(compressBegin_invalidBlockSizeIDs)
{
    LZ4F_cctx* const cctx = newCctx();
    char header[LZ4F_HEADER_SIZE_MAX];
    size_t n;
    for (n = 0; n < ARRAY_SIZE(g_invalidBlockSizeIDs); n++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        prefs.frameInfo.blockSizeID = (LZ4F_blockSizeID_t)g_invalidBlockSizeIDs[n];
        ASSERT_LZ4F_ERROR(LZ4F_compressBegin(cctx, header, sizeof(header), &prefs), LZ4F_ERROR_maxBlockSize_invalid);
    }
}

/* a failed LZ4F_compressBegin() doesn't affect the frame in progress */
TEST(compressBegin_failurePreservesFrame)
{
    size_t const srcSize = 256 KB;
    const void* const src = sample(srcSize);
    LZ4F_cctx* const cctx = newCctx();
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    LZ4F_preferences_t invalidPrefs = LZ4F_INIT_PREFERENCES;
    size_t capacity, pos;
    char* frame;
    prefs.frameInfo.blockSizeID = LZ4F_max64KB;
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    invalidPrefs.frameInfo.blockSizeID = (LZ4F_blockSizeID_t)3;   /* also differs by contentChecksumFlag */
    capacity = LZ4F_compressFrameBound(srcSize, &prefs);
    frame = (char*)UT_malloc(capacity);

    pos = LZ4F_OK(LZ4F_compressBegin(cctx, frame, capacity, &prefs));
    ASSERT_LZ4F_ERROR(LZ4F_compressBegin(cctx, frame + pos, capacity - pos, &invalidPrefs), LZ4F_ERROR_maxBlockSize_invalid);
    /* resume the frame started above */
    pos += LZ4F_OK(LZ4F_compressUpdate(cctx, frame + pos, capacity - pos, src, srcSize, NULL));
    pos += LZ4F_OK(LZ4F_compressEnd(cctx, frame + pos, capacity - pos, NULL));
    assertDecodesTo(frame, pos, NULL, 0, src, srcSize);
}

TEST(compressBegin_afterFailedStateAllocation)
{
    int failAlloc = 0;
    LZ4F_CustomMem const failableMem = { failableMalloc, failableCalloc, failableFree, &failAlloc };
    LZ4F_cctx* const cctx = (LZ4F_cctx*)UT_defer(freeCctx, LZ4F_createCompressionContext_advanced(failableMem, LZ4F_VERSION));
    size_t const srcSize = 64 KB;
    const void* const src = sample(srcSize);
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    void* const frame = UT_malloc(capacity);
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    ASSERT(cctx != NULL);
    prefs.compressionLevel = 1;   /* fast state */
    LZ4F_OK(LZ4F_compressFrame_usingCDict(cctx, frame, capacity, src, srcSize, NULL, &prefs));
    failAlloc = 1;
    prefs.compressionLevel = 9;   /* requires a larger HC state, which can't be allocated */
    ASSERT(LZ4F_isError(LZ4F_compressFrame_usingCDict(cctx, frame, capacity, src, srcSize, NULL, &prefs)));
    failAlloc = 0;
    prefs.compressionLevel = 1;   /* the fast state is gone : must be allocated again */
    LZ4F_OK(LZ4F_compressFrame_usingCDict(cctx, frame, capacity, src, srcSize, NULL, &prefs));
}

TEST(compressEnd_contentSize)
{
    size_t const srcSize = 2 MB;
    const void* const src = sample(srcSize);
    LZ4F_cctx* const cctx = newCctx();
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t const capacity = LZ4F_compressFrameBound(srcSize, &prefs);
    void* const frame = UT_malloc(capacity);
    size_t frameSize;
    frameSize = LZ4F_OK(streamFrame(cctx, frame, capacity, src, srcSize, NULL, 0, &prefs));
    assertDecodesTo(frame, frameSize, NULL, 0, src, srcSize);
    prefs.frameInfo.contentSize = srcSize;
    frameSize = LZ4F_OK(streamFrame(cctx, frame, capacity, src, srcSize, NULL, 0, &prefs));
    assertDecodesTo(frame, frameSize, NULL, 0, src, srcSize);
}

TEST(compressEnd_wrongContentSize)
{
    size_t const srcSize = 2 MB;
    const void* const src = sample(srcSize);
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t const capacity = LZ4F_compressFrameBound(srcSize, &prefs);
    void* const frame = UT_malloc(capacity);
    prefs.frameInfo.contentSize = srcSize + 1;
    ASSERT_LZ4F_ERROR(streamFrame(newCctx(), frame, capacity, src, srcSize, NULL, 0, &prefs), LZ4F_ERROR_frameSize_wrong);
}

TEST(compressBegin_usingDict)
{
    size_t const dictSize = 7 KB;   /* small enough for LZ4_MEMORY_USAGE == 10 */
    size_t const srcSize = 66 KB;   /* must be > 64 KB to avoid short-size optimizations */
    const char* const dict = (const char*)sample(dictSize + srcSize);
    const char* const src = dict + dictSize;
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);   /* NULL preferences : 64 KB linked blocks */
    void* const frame = UT_malloc(capacity);
    size_t const sizeNoDict = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, srcSize, NULL));
    size_t const sizeWithDict = LZ4F_OK(streamFrame(newCctx(), frame, capacity, src, srcSize, dict, dictSize, NULL));
    ASSERT_MSG(sizeWithDict < sizeNoDict, "with dictionary : %u bytes, without : %u bytes", (unsigned)sizeWithDict, (unsigned)sizeNoDict);
    assertDecodesTo(frame, sizeWithDict, dict, dictSize, src, srcSize);
}

TEST(createCDict_advanced)
{
    int failAlloc = 1;
    LZ4F_CustomMem const failableMem = { failableMalloc, failableCalloc, failableFree, &failAlloc };
    size_t const dictSize = 7 KB;
    const void* const dict = sample(dictSize);
    ASSERT(UT_defer(freeCDict, LZ4F_createCDict_advanced(failableMem, dict, dictSize)) == NULL);   /* uses failableMem */
    failAlloc = 0;
    ASSERT(UT_defer(freeCDict, LZ4F_createCDict_advanced(failableMem, dict, dictSize)) != NULL);
}

/* the dictionary is the beginning of the input */
#define CDICT_DICT_SIZE (7 KB)   /* small enough for LZ4_MEMORY_USAGE == 10 */
static LZ4F_CDict* newCDict(const void* dict)
{
    LZ4F_CDict* const cdict = (LZ4F_CDict*)UT_defer(freeCDict, LZ4F_createCDict(dict, CDICT_DICT_SIZE));
    ASSERT(cdict != NULL);
    return cdict;
}

TEST(compressFrame_usingCDict)
{
    size_t const srcSize = 65 KB;   /* must be > 64 KB to avoid short-size optimizations */
    const void* const src = sample(srcSize);
    LZ4F_CDict* const cdict = newCDict(src);
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    void* const frame = UT_malloc(capacity);
    size_t const sizeNoDict = LZ4F_OK(LZ4F_compressFrame_usingCDict(newCctx(), frame, capacity, src, srcSize, NULL, NULL));
    size_t const sizeWithDict = LZ4F_OK(LZ4F_compressFrame_usingCDict(newCctx(), frame, capacity, src, srcSize, cdict, NULL));
    ASSERT_MSG(sizeWithDict < sizeNoDict, "with dictionary : %u bytes, without : %u bytes", (unsigned)sizeWithDict, (unsigned)sizeNoDict);
    assertDecodesTo(frame, sizeWithDict, src, CDICT_DICT_SIZE, src, srcSize);
}

TEST(compressFrame_usingCDict_extremeLevels)
{
    int const levels[] = { -3, LZ4F_compressionLevel_max() };
    size_t const srcSize = CDICT_DICT_SIZE;
    const void* const src = sample(srcSize);
    LZ4F_CDict* const cdict = newCDict(src);
    LZ4F_cctx* const cctx = newCctx();
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    void* const frame = UT_malloc(capacity);
    size_t n;
    for (n = 0; n < ARRAY_SIZE(levels); n++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        size_t frameSize;
        prefs.compressionLevel = levels[n];
        frameSize = LZ4F_OK(LZ4F_compressFrame_usingCDict(cctx, frame, capacity, src, srcSize, cdict, &prefs));
        assertDecodesTo(frame, frameSize, src, CDICT_DICT_SIZE, src, srcSize);
    }
}

TEST(compressFrame_usingCDict_blockModes)
{
    size_t const srcSize = 3 * 64 KB;   /* a single block would always be independent */
    const void* const src = sample(srcSize);
    LZ4F_CDict* const cdict = newCDict(src);
    LZ4F_cctx* const cctx = newCctx();
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    void* const frame = UT_malloc(capacity);
    int mode;
    for (mode = LZ4F_blockLinked; mode <= LZ4F_blockIndependent; mode++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        size_t frameSize;
        prefs.frameInfo.blockMode = (LZ4F_blockMode_t)mode;
        prefs.frameInfo.blockSizeID = LZ4F_max64KB;
        frameSize = LZ4F_OK(LZ4F_compressFrame_usingCDict(cctx, frame, capacity, src, srcSize, cdict, &prefs));
        assertDecodesTo(frame, frameSize, src, CDICT_DICT_SIZE, src, srcSize);
        assertRequiresDict(frame, frameSize, srcSize);
    }
}


static const UT_test tests[] = {
    UT_TEST(compressBound_emptyInput),
    UT_TEST(compressBound_autoFlush),
    UT_TEST(compressBound_noAutoFlush),
    UT_TEST(getBlockSize),
    UT_TEST(getFrameInfo_emptyFrame),
    UT_TEST(frameHeader_dictID),
    UT_TEST(compressFrame_veryNegativeLevels),
    UT_TEST(compressFrame_validBlockSizeIDs),
    UT_TEST(compressBegin_invalidBlockSizeIDs),
    UT_TEST(compressBegin_failurePreservesFrame),
    UT_TEST(compressBegin_afterFailedStateAllocation),
    UT_TEST(compressEnd_contentSize),
    UT_TEST(compressEnd_wrongContentSize),
    UT_TEST(compressBegin_usingDict),
    UT_TEST(createCDict_advanced),
    UT_TEST(compressFrame_usingCDict),
    UT_TEST(compressFrame_usingCDict_extremeLevels),
    UT_TEST(compressFrame_usingCDict_blockModes),
};
UT_SUITE(lz4frame, tests);
