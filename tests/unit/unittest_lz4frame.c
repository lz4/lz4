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
#include <stdio.h>      /* FILE, tmpfile, fopen, fclose */
#include <string.h>     /* memcpy, memset */
#include <limits.h>     /* INT_MIN */
#define LZ4F_STATIC_LINKING_ONLY
#include "lz4frame.h"
#include "lz4file.h"
#define LZ4_STATIC_LINKING_ONLY  /* LZ4_ACCELERATION_MAX */
#include "lz4.h"

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

/* decodes @src by segments of @segmentSize bytes, or of random sizes when @segmentSize == 0,
 * until the end of the frame
 * @return : decoded size */
static size_t decompressBySegments(LZ4F_dctx* dctx, void* dst, size_t dstCapacity,
                                   const void* src, size_t srcSize, size_t segmentSize)
{
    char* const ostart = (char*)dst;
    const char* ip = (const char*)src;
    const char* const iend = ip + srcSize;
    size_t pos = 0, r = 1;
    while (ip < iend) {
        size_t iSize = segmentSize;
        size_t oSize = dstCapacity - pos;
        if (iSize == 0) {
            unsigned const nbBits = UT_rand() % 22;
            iSize = (UT_rand() & ((1U << nbBits) - 1)) + 1;
        }
        if (iSize > (size_t)(iend - ip)) iSize = (size_t)(iend - ip);
        r = LZ4F_OK(LZ4F_decompress(dctx, ostart + pos, &oSize, ip, &iSize, NULL));
        ip += iSize;
        pos += oSize;
    }
    ASSERT_EQ(r, 0);   /* end of frame */
    return pos;
}

static void writeLE32(void* dst, unsigned value)
{
    unsigned char* const p = (unsigned char*)dst;
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static void closeFile(void* file) { if (file != NULL) fclose((FILE*)file); }

/* @return : a temporary file, closed at the end of the test */
static FILE* newTmpFile(void)
{
    FILE* const file = (FILE*)UT_defer(closeFile, tmpfile());
    ASSERT(file != NULL);
    return file;
}

/* allocator counting the bytes currently allocated, in *(size_t*)state */
#define COUNTED_HEADER 16   /* keeps the alignment of malloc() */
static void* countedMalloc(void* state, size_t s)
{
    char* const p = (char*)malloc(COUNTED_HEADER + s);
    if (p == NULL) return NULL;
    memcpy(p, &s, sizeof(s));
    *(size_t*)state += s;
    return p + COUNTED_HEADER;
}
static void* countedCalloc(void* state, size_t s)
{
    void* const p = countedMalloc(state, s);
    if (p != NULL) memset(p, 0, s);
    return p;
}
static void countedFree(void* state, void* p)
{
    size_t s;
    if (p == NULL) return;
    memcpy(&s, (char*)p - COUNTED_HEADER, sizeof(s));
    *(size_t*)state -= s;
    free((char*)p - COUNTED_HEADER);
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
    const void* const src = UT_sample(srcSize);
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
    const void* const src = UT_sample(srcSize);
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
    const void* const src = UT_sample(srcSize);
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
    const void* const src = UT_sample(srcSize);
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
    const void* const src = UT_sample(srcSize);
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
    const void* const src = UT_sample(srcSize);
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
    const char* const dict = (const char*)UT_sample(dictSize + srcSize);
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
    const void* const dict = UT_sample(dictSize);
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
    const void* const src = UT_sample(srcSize);
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
    const void* const src = UT_sample(srcSize);
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
    const void* const src = UT_sample(srcSize);
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


TEST(compressFrame_defaultPreferences)
{
    size_t const srcSize = 2 MB;
    const void* const src = UT_sample(srcSize);
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    void* const frame = UT_malloc(capacity);
    size_t const frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, srcSize, NULL));
    assertDecodesTo(frame, frameSize, NULL, 0, src, srcSize);
}

TEST(compressFrame_negativeLevel)
{
    size_t const srcSize = 2 MB;
    const void* const src = UT_sample(srcSize);
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    void* const frame = UT_malloc(capacity);
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t frameSize;
    prefs.compressionLevel = -3;
    frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, srcSize, &prefs));
    assertDecodesTo(frame, frameSize, NULL, 0, src, srcSize);
}

/* all block sizes, with and without content checksum */
TEST(compressFrame_blockSizes)
{
    size_t const srcSize = 2 MB;
    const void* const src = UT_sample(srcSize);
    unsigned id;
    int checksum;
    for (id = LZ4F_max64KB; id <= LZ4F_max4MB; id++)
    for (checksum = 0; checksum <= 1; checksum++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        size_t capacity, frameSize;
        void* frame;
        prefs.frameInfo.blockSizeID = (LZ4F_blockSizeID_t)id;
        prefs.frameInfo.contentChecksumFlag = (LZ4F_contentChecksum_t)checksum;
        capacity = LZ4F_compressFrameBound(srcSize, &prefs);
        frame = UT_malloc(capacity);
        frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, srcSize, &prefs));
        assertDecodesTo(frame, frameSize, NULL, 0, src, srcSize);
    }
}

TEST(compressFrame_blockChecksum)
{
    size_t const srcSize = 2 MB;
    const void* const src = UT_sample(srcSize);
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t capacity, frameSize;
    void* frame;
    prefs.frameInfo.blockChecksumFlag = LZ4F_blockChecksumEnabled;
    capacity = LZ4F_compressFrameBound(srcSize, &prefs);
    frame = UT_malloc(capacity);
    frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, srcSize, &prefs));
    assertDecodesTo(frame, frameSize, NULL, 0, src, srcSize);
}

TEST(decompress_randomSegments)
{
    size_t const srcSize = 2 MB;
    const void* const src = UT_sample(srcSize);
    char* const decoded = (char*)UT_malloc(srcSize);
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t capacity, frameSize;
    void* frame;
    prefs.frameInfo.blockSizeID = LZ4F_max256KB;
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    capacity = LZ4F_compressFrameBound(srcSize, &prefs);
    frame = UT_malloc(capacity);
    frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, srcSize, &prefs));
    ASSERT_EQ(decompressBySegments(newDctx(), decoded, srcSize, frame, frameSize, 0), srcSize);
    ASSERT_MEMEQ(decoded, src, srcSize);
}

/* when the end of the frame is missing, LZ4F_decompress() requests exactly the missing bytes */
TEST(decompress_missingBytes)
{
    size_t const srcSize = 2 MB;
    size_t const missing = 4;
    const void* const src = UT_sample(srcSize);
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    char* const frame = (char*)UT_malloc(capacity);
    char* const decoded = (char*)UT_malloc(srcSize);
    LZ4F_dctx* const dctx = newDctx();
    size_t const frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, srcSize, NULL));
    size_t cSize = frameSize - missing;
    size_t dSize = srcSize;
    size_t requested, pos;
    requested = LZ4F_OK(LZ4F_decompress(dctx, decoded, &dSize, frame, &cSize, NULL));
    ASSERT_EQ(requested, missing);
    pos = dSize;
    dSize = srcSize - pos;
    ASSERT_EQ(LZ4F_OK(LZ4F_decompress(dctx, decoded + pos, &dSize, frame + cSize, &requested, NULL)), 0);
    ASSERT_EQ(pos + dSize, srcSize);
    ASSERT_MEMEQ(decoded, src, srcSize);
}

/* fed with 0 bytes, LZ4F_decompress() tells how many bytes to provide next */
TEST(decompress_emptyInput)
{
    char const src[1] = { 0 };
    size_t dSize = 0, cSize = 0;
    ASSERT(LZ4F_OK(LZ4F_decompress(newDctx(), NULL, &dSize, src, &cSize, NULL)) > 0);
}

TEST(getFrameInfo_incompleteHeader)
{
    size_t const srcSize = 64 KB;
    const void* const src = UT_sample(srcSize);
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    char* const frame = (char*)UT_malloc(capacity);
    LZ4F_dctx* const dctx = newDctx();
    LZ4F_frameInfo_t frameInfo;
    size_t size = 0;
    LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, srcSize, NULL));
    ASSERT_LZ4F_ERROR(LZ4F_getFrameInfo(dctx, &frameInfo, frame, &size), LZ4F_ERROR_frameHeader_incomplete);
    size = 6;
    ASSERT_LZ4F_ERROR(LZ4F_getFrameInfo(dctx, &frameInfo, frame, &size), LZ4F_ERROR_frameHeader_incomplete);
    size = LZ4F_OK(LZ4F_headerSize(frame, LZ4F_MIN_SIZE_TO_KNOW_HEADER_LENGTH));
    LZ4F_OK(LZ4F_getFrameInfo(dctx, &frameInfo, frame, &size));
}

/* after a decoding error, LZ4F_resetDecompressionContext() makes the context usable again */
TEST(decompress_resetAfterError)
{
    size_t const srcSize = 2 MB;
    const char* const src = (const char*)UT_sample(srcSize);
    char* const decoded = (char*)UT_malloc(srcSize);
    LZ4F_dctx* const dctx = newDctx();
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t capacity, frameSize, dSize = srcSize, cSize;
    char* frame;
    char* corrupted;
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;   /* any corruption is detected */
    capacity = LZ4F_compressFrameBound(srcSize, &prefs);
    frame = (char*)UT_malloc(capacity);
    corrupted = (char*)UT_malloc(capacity);
    frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, srcSize, &prefs));
    ASSERT(frameSize > 48);
    memcpy(corrupted, frame, frameSize);
    memcpy(corrupted + 16, src + 32, 32);   /* noise : can't match the literals stored at this position */
    cSize = frameSize;
    ASSERT(LZ4F_isError(LZ4F_decompress(dctx, decoded, &dSize, corrupted, &cSize, NULL)));

    LZ4F_resetDecompressionContext(dctx);
    ASSERT_EQ(decompressBySegments(dctx, decoded, srcSize, frame, frameSize, 1 /* byte after byte */), srcSize);
    ASSERT_MEMEQ(decoded, src, srcSize);
}

/* #1227 : after a reset, an unfinished session doesn't affect the next frame */
TEST(decompress_resetUnfinishedFrame)
{
    char const src[9] = { 0 };
    char decoded[sizeof(src)];
    size_t const capacity = LZ4F_compressFrameBound(sizeof(src), NULL);
    char* const frame = (char*)UT_malloc(capacity);
    LZ4F_dctx* const dctx = newDctx();
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t frameSize, dSize, cSize;

    /* first session : not finished */
    prefs.frameInfo.contentSize = sizeof(src);
    frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, sizeof(src), &prefs));
    ASSERT(frameSize > 15);
    dSize = sizeof(decoded);
    cSize = 15;
    LZ4F_OK(LZ4F_decompress(dctx, decoded, &dSize, frame, &cSize, NULL));
    ASSERT(cSize < frameSize);   /* not entirely consumed */
    LZ4F_resetDecompressionContext(dctx);

    /* second session : empty frame, without content size */
    frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, NULL, 0, NULL));
    dSize = 1;
    cSize = frameSize;
    LZ4F_OK(LZ4F_decompress(dctx, decoded, &dSize, frame, &cSize, NULL));
    ASSERT_EQ(dSize, 0);
    ASSERT_EQ(cSize, frameSize);
}

/* successive skippable frames, decoded with the same context */
TEST(decompress_skippableFrames)
{
    size_t const payloadSize = 1500 KB;
    size_t const dstCapacity = 64 KB;
    char* const frame = (char*)UT_sample(8 + payloadSize);   /* payload content doesn't matter */
    char* const decoded = (char*)UT_malloc(dstCapacity);
    LZ4F_dctx* const dctx = newDctx();

    writeLE32(frame, LZ4F_MAGIC_SKIPPABLE_START);
    writeLE32(frame + 4, (unsigned)payloadSize);
    ASSERT_EQ(decompressBySegments(dctx, decoded, dstCapacity, frame, 8 + payloadSize, 0), 0);

    /* zero-size skippable frame */
    writeLE32(frame, LZ4F_MAGIC_SKIPPABLE_START + 1);
    writeLE32(frame + 4, 0);
    ASSERT_EQ(decompressBySegments(dctx, decoded, dstCapacity, frame, 8, 0), 0);

    /* header complete in first call */
    writeLE32(frame, LZ4F_MAGIC_SKIPPABLE_START + 2);
    writeLE32(frame + 4, 10);
    ASSERT_EQ(decompressBySegments(dctx, decoded, 10, frame, 18, 10), 0);
}

/* LZ4F_compressBound() is enough for LZ4F_write(), even without checksums */
TEST(write_noChecksums)
{
    size_t const blockSize = LZ4F_getBlockSize(LZ4F_default);
    char* const data = (char*)UT_malloc(blockSize);
    LZ4_writeFile_t* writeCtx = NULL;
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    memset(data, 0, blockSize);
    prefs.frameInfo.contentChecksumFlag = LZ4F_noContentChecksum;
    prefs.frameInfo.blockChecksumFlag = LZ4F_noBlockChecksum;
    LZ4F_OK(LZ4F_writeOpen(&writeCtx, newTmpFile(), &prefs));
    LZ4F_OK(LZ4F_write(writeCtx, data, blockSize - 1));
    LZ4F_OK(LZ4F_write(writeCtx, data, 1));
    LZ4F_OK(LZ4F_writeClose(writeCtx));
}

/* LZ4F_writeOpen() rejects an invalid blockSizeID,
 * instead of sizing its buffers from the error code returned by LZ4F_getBlockSize() */
TEST(writeOpen_blockSizeIDs)
{
    FILE* const file = newTmpFile();
    size_t n;
    for (n = 0; n < ARRAY_SIZE(g_invalidBlockSizeIDs); n++) {
        LZ4_writeFile_t* writeCtx = NULL;
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;   /* linked blocks : small buffers, allocation succeeds */
        prefs.frameInfo.blockSizeID = (LZ4F_blockSizeID_t)g_invalidBlockSizeIDs[n];
        ASSERT_LZ4F_ERROR(LZ4F_writeOpen(&writeCtx, file, &prefs), LZ4F_ERROR_maxBlockSize_invalid);
    }
    for (n = 0; n < ARRAY_SIZE(g_validBlockSizeIDs); n++) {
        LZ4_writeFile_t* writeCtx = NULL;
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        prefs.frameInfo.blockSizeID = (LZ4F_blockSizeID_t)g_validBlockSizeIDs[n];
        LZ4F_OK(LZ4F_writeOpen(&writeCtx, file, &prefs));
        LZ4F_OK(LZ4F_writeClose(writeCtx));
    }
}

/* a failed open sets the handle to NULL,
 * LZ4F_writeClose() reports an error from a previous LZ4F_write() */
TEST(lz4file_errors)
{
    void* const stale = UT_malloc(1);   /* initial value of the handles */
    LZ4_writeFile_t* writeCtx = (LZ4_writeFile_t*)stale;
    LZ4_readFile_t* readCtx = (LZ4_readFile_t*)stale;
    ASSERT(LZ4F_isError(LZ4F_writeOpen(&writeCtx, NULL, NULL)));
    ASSERT(writeCtx == NULL);
    ASSERT(LZ4F_isError(LZ4F_readOpen(&readCtx, NULL)));
    ASSERT(readCtx == NULL);
    LZ4F_OK(LZ4F_writeOpen(&writeCtx, newTmpFile(), NULL));
    ASSERT_EQ(LZ4F_writeClose(writeCtx), LZ4F_OK_NoError);
#if defined(__linux__)
    {   FILE* const fullFile = (FILE*)UT_defer(closeFile, fopen("/dev/full", "wb"));   /* writes fail when stdio flushes its buffer */
        if (fullFile != NULL) {
            size_t const srcSize = 2 MB;
            const void* const src = UT_sample(srcSize);
            LZ4F_OK(LZ4F_writeOpen(&writeCtx, fullFile, NULL));   /* header remains buffered */
            ASSERT(LZ4F_isError(LZ4F_write(writeCtx, src, srcSize)));
            ASSERT(LZ4F_isError(LZ4F_writeClose(writeCtx)));
    }   }
#endif
}

/* LZ4F_cctx_size() and LZ4F_dctx_size() report the allocated memory */
static size_t g_cctxAllocated, g_dctxAllocated;   /* not on the stack : contexts are released after the test */
#define ASSERT_CTX_SIZES(cctx, dctx) do {                          \
        ASSERT_EQ(LZ4F_cctx_size(cctx), g_cctxAllocated);          \
        ASSERT_EQ(LZ4F_dctx_size(dctx), g_dctxAllocated);          \
    } while (0)
TEST(cctx_dctx_sizes)
{
    /* sizes remain accurate when contexts are reused with different parameters,
     * since buffers are only reallocated when too small */
    static const struct { LZ4F_blockSizeID_t bsid; LZ4F_blockMode_t mode; int level; } frames[] = {
        { LZ4F_max4MB,   LZ4F_blockIndependent, 1 },
        { LZ4F_max64KB,  LZ4F_blockIndependent, 1 },   /* smaller blocks : buffers are reused */
        { LZ4F_max256KB, LZ4F_blockLinked,      9 },
        { LZ4F_max64KB,  LZ4F_blockLinked,      1 },
        { LZ4F_max1MB,   LZ4F_blockIndependent, 9 },
        { LZ4F_max64KB,  LZ4F_blockIndependent, 1 } };
    LZ4F_CustomMem const cMem = { countedMalloc, countedCalloc, countedFree, &g_cctxAllocated };
    LZ4F_CustomMem const dMem = { countedMalloc, countedCalloc, countedFree, &g_dctxAllocated };
    LZ4F_cctx* cctx;
    LZ4F_dctx* dctx;
    size_t const srcSize = 2 MB;
    const void* const src = UT_sample(srcSize);
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    void* const frame = UT_malloc(capacity);
    void* const decoded = UT_malloc(srcSize + 1);
    size_t frameSize, dSize, f;
    g_cctxAllocated = g_dctxAllocated = 0;
    ASSERT_EQ(LZ4F_cctx_size(NULL), 0);
    ASSERT_EQ(LZ4F_dctx_size(NULL), 0);
    cctx = (LZ4F_cctx*)UT_defer(freeCctx, LZ4F_createCompressionContext_advanced(cMem, LZ4F_VERSION));
    dctx = (LZ4F_dctx*)UT_defer(freeDctx, LZ4F_createDecompressionContext_advanced(dMem, LZ4F_VERSION));
    ASSERT(cctx != NULL && dctx != NULL);
    ASSERT_CTX_SIZES(cctx, dctx);

    frameSize = LZ4F_OK(LZ4F_compressFrame_usingCDict(cctx, frame, capacity, src, srcSize, NULL, NULL));
    dSize = srcSize + 1;
    LZ4F_OK(LZ4F_decompress(dctx, decoded, &dSize, frame, &frameSize, NULL));
    ASSERT_EQ(dSize, srcSize);
    ASSERT_CTX_SIZES(cctx, dctx);

    for (f = 0; f < ARRAY_SIZE(frames); f++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        prefs.frameInfo.blockSizeID = frames[f].bsid;
        prefs.frameInfo.blockMode = frames[f].mode;
        prefs.compressionLevel = frames[f].level;
        frameSize = LZ4F_OK(streamFrame(cctx, frame, capacity, src, 1 KB, NULL, 0, &prefs));
        dSize = 1 KB;
        ASSERT_EQ(LZ4F_OK(LZ4F_decompress(dctx, decoded, &dSize, frame, &frameSize, NULL)), 0);
        ASSERT_EQ(dSize, 1 KB);
        ASSERT_CTX_SIZES(cctx, dctx);
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
    UT_TEST(compressFrame_defaultPreferences),
    UT_TEST(compressFrame_negativeLevel),
    UT_TEST(compressFrame_blockSizes),
    UT_TEST(compressFrame_blockChecksum),
    UT_TEST(decompress_randomSegments),
    UT_TEST(decompress_missingBytes),
    UT_TEST(decompress_emptyInput),
    UT_TEST(getFrameInfo_incompleteHeader),
    UT_TEST(decompress_resetAfterError),
    UT_TEST(decompress_resetUnfinishedFrame),
    UT_TEST(decompress_skippableFrames),
    UT_TEST(write_noChecksums),
    UT_TEST(writeOpen_blockSizeIDs),
    UT_TEST(lz4file_errors),
    UT_TEST(cctx_dctx_sizes),
};
UT_SUITE(lz4frame, tests);
