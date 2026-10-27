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
#include <time.h>       /* clock */
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
typedef unsigned char BYTE;

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

static LZ4F_CDict* newCDict(const void* dict, size_t dictSize)
{
    LZ4F_CDict* const cdict = (LZ4F_CDict*)UT_defer(freeCDict, LZ4F_createCDict(dict, dictSize));
    ASSERT(cdict != NULL);
    return cdict;
}

/* allocator which fails while *state != 0 */
static void* failableMalloc(void* state, size_t s) { return *(const int*)state ? NULL : malloc(s); }
static void* failableCalloc(void* state, size_t s) { return *(const int*)state ? NULL : calloc(1, s); }
static void failableFree(void* state, void* p) { (void)state; free(p); }

/* compresses @src into a frame, with LZ4F_compressBegin_usingCDict() when @cdict != NULL,
 * LZ4F_compressBegin_usingDict() when @dict != NULL, or LZ4F_compressBegin(),
 * followed by LZ4F_compressUpdate() and LZ4F_compressEnd().
 * @return : frame size, or an error code */
static size_t streamFrame(LZ4F_cctx* cctx, void* dst, size_t dstCapacity,
                          const void* src, size_t srcSize,
                          const void* dict, size_t dictSize, const LZ4F_CDict* cdict,
                          const LZ4F_preferences_t* prefs)
{
    char* const ostart = (char*)dst;
    size_t pos, r;
    pos = cdict ? LZ4F_compressBegin_usingCDict(cctx, dst, dstCapacity, cdict, prefs)
        : dict ? LZ4F_compressBegin_usingDict(cctx, dst, dstCapacity, dict, dictSize, prefs)
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
    LZ4F_dctx* const dctx = newDctx();
    size_t dSize = expectedSize;
    size_t cSize = frameSize;
    ASSERT_EQ(LZ4F_OK(LZ4F_decompress_usingDict(dctx, decoded, &dSize, frame, &cSize, dict, dictSize, NULL)), 0);
    ASSERT_EQ(cSize, frameSize);
    ASSERT_EQ(dSize, expectedSize);
    ASSERT_MEMEQ(decoded, expected, expectedSize);
    UT_release(dctx);
    UT_release(decoded);
}

/* fails the test unless decoding @frame requires a dictionary */
static void assertRequiresDict(const void* frame, size_t frameSize, size_t decodedSize)
{
    void* const decoded = UT_malloc(decodedSize);
    LZ4F_dctx* const dctx = newDctx();
    size_t dSize = decodedSize;
    size_t cSize = frameSize;
    ASSERT(LZ4F_isError(LZ4F_decompress(dctx, decoded, &dSize, frame, &cSize, NULL)));
    UT_release(dctx);
    UT_release(decoded);
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

static unsigned readLE32(const void* src)
{
    const unsigned char* const p = (const unsigned char*)src;
    return (unsigned)p[0] + ((unsigned)p[1] << 8) + ((unsigned)p[2] << 16) + ((unsigned)p[3] << 24);
}

/* frameElementEnds() :
 * lists the end positions of the elements of @frame :
 * header, then each block, then endMark, then optional content checksum.
 * @return : nb of elements, or 0 if @frame is not a well-formed frame */
static size_t frameElementEnds(size_t* ends, size_t maxEnds, const unsigned char* frame, size_t frameSize)
{
    size_t nb = 0;
    size_t pos;
    unsigned blockChecksum, contentChecksum;
    if (frameSize < LZ4F_HEADER_SIZE_MIN || maxEnds == 0) return 0;
    pos = LZ4F_headerSize(frame, frameSize);
    if (LZ4F_isError(pos) || pos > frameSize) return 0;
    blockChecksum = (frame[4] >> 4) & 1;
    contentChecksum = (frame[4] >> 2) & 1;
    ends[nb++] = pos;
    for (;;) {
        unsigned header;
        if (pos + 4 > frameSize || nb == maxEnds) return 0;
        header = readLE32(frame + pos);
        pos += 4;
        if (header == 0) {   /* endMark */
            ends[nb++] = pos;
            if (contentChecksum) {
                if (nb == maxEnds) return 0;
                pos += 4;
                ends[nb++] = pos;
            }
            return (pos == frameSize) ? nb : 0;
        }
        pos += (header & 0x7FFFFFFFU) + 4 * blockChecksum;
        ends[nb++] = pos;
    }
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
    frameSize = LZ4F_OK(streamFrame(cctx, frame, capacity, src, srcSize, NULL, 0, NULL, &prefs));
    assertDecodesTo(frame, frameSize, NULL, 0, src, srcSize);
    prefs.frameInfo.contentSize = srcSize;
    frameSize = LZ4F_OK(streamFrame(cctx, frame, capacity, src, srcSize, NULL, 0, NULL, &prefs));
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
    ASSERT_LZ4F_ERROR(streamFrame(newCctx(), frame, capacity, src, srcSize, NULL, 0, NULL, &prefs), LZ4F_ERROR_frameSize_wrong);
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
    size_t const sizeWithDict = LZ4F_OK(streamFrame(newCctx(), frame, capacity, src, srcSize, dict, dictSize, NULL, NULL));
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

TEST(compressFrame_usingCDict)
{
    size_t const srcSize = 65 KB;   /* must be > 64 KB to avoid short-size optimizations */
    const void* const src = UT_sample(srcSize);
    LZ4F_CDict* const cdict = newCDict(src, CDICT_DICT_SIZE);
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
    LZ4F_CDict* const cdict = newCDict(src, CDICT_DICT_SIZE);
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
    LZ4F_CDict* const cdict = newCDict(src, CDICT_DICT_SIZE);
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

/* LZ4F_compressBegin_usingDict() : with independent blocks, each block uses the dictionary, also when nbWorkers >= 1.
 * Each block starts with the end of the dictionary (noise) : it can't be decoded without the dictionary */
TEST(compressBegin_usingDict_eachBlock)
{
    static const int levels[] = { -3, 1, 2, 9 };   /* fast, then HC states, reused */
    size_t const blockSize = 64 KB;
    size_t const nbBlocks = 3;
    size_t const srcSize = nbBlocks * blockSize;
    size_t const tailSize = 512;   /* most recent positions : kept by small hash tables (LZ4_MEMORY_USAGE == 10) */
    const char* const dict = (const char*)UT_noise(CDICT_DICT_SIZE);
    char* const src = (char*)UT_sample(srcSize);
    char* const decoded = (char*)UT_malloc(blockSize);
    LZ4F_cctx* const cctx = newCctx();
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    char* const frame = (char*)UT_malloc(capacity);
    size_t n, l;

    for (n = 0; n < nbBlocks; n++) memcpy(src + n * blockSize, dict + CDICT_DICT_SIZE - tailSize, tailSize);
    for (l = 0; l < ARRAY_SIZE(levels); l++) {
        unsigned nbWorkers;
        for (nbWorkers = 0; nbWorkers <= 1; nbWorkers++) {
            LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
            size_t pos = 7;   /* frame header : no content size, no dictID */
            size_t frameSize;
            prefs.compressionLevel = levels[l];
            prefs.frameInfo.blockMode = LZ4F_blockIndependent;
            prefs.frameInfo.blockSizeID = LZ4F_max64KB;
            prefs.nbWorkers = nbWorkers;
            frameSize = LZ4F_OK(streamFrame(cctx, frame, capacity, src, srcSize, dict, CDICT_DICT_SIZE, NULL, &prefs));
            assertDecodesTo(frame, frameSize, dict, CDICT_DICT_SIZE, src, srcSize);
            for (n = 0; n < nbBlocks; n++) {
                unsigned const blockHeader = readLE32(frame + pos);
                int const cSize = (int)(blockHeader & 0x7FFFFFFFU);
                ASSERT_MSG(!(blockHeader >> 31), "level=%i, block %u : not compressed", levels[l], (unsigned)n);
                ASSERT_MSG(LZ4_decompress_safe(frame + pos + 4, decoded, cSize, (int)blockSize) < 0,
                           "level=%i, nbWorkers=%u, block %u : decodes without dictionary", levels[l], nbWorkers, (unsigned)n);
                pos += 4 + (size_t)cSize;
    }   }   }
}

/* LZ4F_compressBegin_usingDict() : with HC levels, same frame as with a CDict of this dictionary,
 * with independent blocks, or nbWorkers >= 1 (fast levels digest the dictionary faster than a CDict) */
TEST(compressBegin_usingDict_sameAsCDict_HC)
{
    static const int levels[] = { 2, 9 };
    static const size_t dictSizes[] = { CDICT_DICT_SIZE, 64 KB + 5 KB };   /* larger : only its last 64 KB */
    size_t const srcSize = 3 * 64 KB;   /* a single block would always be independent */
    const char* const sample = (const char*)UT_sample(64 KB + 5 KB + srcSize);
    const char* const src = sample + 64 KB + 5 KB;
    LZ4F_cctx* const cctx = newCctx();
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    char* const ref = (char*)UT_malloc(capacity);
    char* const frame = (char*)UT_malloc(capacity);
    size_t d, l;

    for (d = 0; d < ARRAY_SIZE(dictSizes); d++) {
        const char* const dict = src - dictSizes[d];
        LZ4F_CDict* const cdict = newCDict(dict, dictSizes[d]);
        for (l = 0; l < ARRAY_SIZE(levels); l++) {
            unsigned nbWorkers;
            int mode;
            for (nbWorkers = 0; nbWorkers <= 1; nbWorkers++)
            for (mode = LZ4F_blockLinked; mode <= LZ4F_blockIndependent; mode++) {
                LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
                size_t refSize, frameSize;
                if ((nbWorkers == 0) && (mode == LZ4F_blockLinked)) continue;   /* history of the first block */
                prefs.compressionLevel = levels[l];
                prefs.frameInfo.blockMode = (LZ4F_blockMode_t)mode;
                prefs.frameInfo.blockSizeID = LZ4F_max64KB;
                prefs.nbWorkers = nbWorkers;
                refSize = LZ4F_OK(streamFrame(cctx, ref, capacity, src, srcSize, NULL, 0, cdict, &prefs));
                frameSize = LZ4F_OK(streamFrame(cctx, frame, capacity, src, srcSize, dict, dictSizes[d], NULL, &prefs));
                ASSERT_MSG(frameSize == refSize && !memcmp(frame, ref, refSize),
                           "dictSize=%u, level=%i, %s blocks, nbWorkers=%u : differs from CDict",
                           (unsigned)dictSizes[d], levels[l], mode == LZ4F_blockLinked ? "linked" : "independent", nbWorkers);
        }   }
        UT_release(cdict);
    }
}

TEST(compressBegin_usingDict_allocationFailure)
{
    int failAlloc = 0;
    LZ4F_CustomMem const failableMem = { failableMalloc, failableCalloc, failableFree, &failAlloc };
    LZ4F_cctx* const cctx = (LZ4F_cctx*)UT_defer(freeCctx, LZ4F_createCompressionContext_advanced(failableMem, LZ4F_VERSION));
    size_t const srcSize = 2 * 64 KB;
    const char* const dict = (const char*)UT_sample(CDICT_DICT_SIZE + srcSize);
    const char* const src = dict + CDICT_DICT_SIZE;
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    char* const frame = (char*)UT_malloc(capacity);
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    int level;
    ASSERT(cctx != NULL);
    prefs.frameInfo.blockMode = LZ4F_blockIndependent;
    prefs.frameInfo.blockSizeID = LZ4F_max64KB;
    for (level = 1; level <= 9; level += 8) {
        size_t frameSize;
        prefs.compressionLevel = level;
        LZ4F_OK(streamFrame(cctx, frame, capacity, src, srcSize, NULL, 0, NULL, &prefs));   /* state and buffers */
        failAlloc = 1;   /* the dictionary requires its own state */
        ASSERT_LZ4F_ERROR(LZ4F_compressBegin_usingDict(cctx, frame, capacity, dict, CDICT_DICT_SIZE, &prefs),
                          LZ4F_ERROR_allocation_failed);
        failAlloc = 0;
        frameSize = LZ4F_OK(streamFrame(cctx, frame, capacity, src, srcSize, dict, CDICT_DICT_SIZE, NULL, &prefs));
        assertDecodesTo(frame, frameSize, dict, CDICT_DICT_SIZE, src, srcSize);
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
        frameSize = LZ4F_OK(streamFrame(cctx, frame, capacity, src, 1 KB, NULL, 0, NULL, &prefs));
        dSize = 1 KB;
        ASSERT_EQ(LZ4F_OK(LZ4F_decompress(dctx, decoded, &dSize, frame, &frameSize, NULL)), 0);
        ASSERT_EQ(dSize, 1 KB);
        ASSERT_CTX_SIZES(cctx, dctx);
    }
}


/* LZ4F_flush() and LZ4F_compressEnd() require a frame in progress */
TEST(flush_outsideFrame)
{
    char src[100];
    char dst[LZ4F_HEADER_SIZE_MAX + 200];
    LZ4F_cctx* const cctx = newCctx();
    memset(src, 'a', sizeof(src));
    /* fresh context */
    ASSERT_LZ4F_ERROR(LZ4F_flush(cctx, dst, sizeof(dst), NULL), LZ4F_ERROR_compressionState_uninitialized);
    ASSERT_LZ4F_ERROR(LZ4F_compressEnd(cctx, dst, sizeof(dst), NULL), LZ4F_ERROR_compressionState_uninitialized);

    LZ4F_OK(LZ4F_compressBegin(cctx, dst, sizeof(dst), NULL));
    LZ4F_OK(LZ4F_compressUpdate(cctx, dst, sizeof(dst), src, sizeof(src), NULL));
    LZ4F_OK(LZ4F_compressEnd(cctx, dst, sizeof(dst), NULL));

    /* after the end of the frame */
    ASSERT_LZ4F_ERROR(LZ4F_flush(cctx, dst, sizeof(dst), NULL), LZ4F_ERROR_compressionState_uninitialized);
    ASSERT_LZ4F_ERROR(LZ4F_compressEnd(cctx, dst, sizeof(dst), NULL), LZ4F_ERROR_compressionState_uninitialized);
}

/* LZ4F_compressUpdate() and LZ4F_uncompressedUpdate(), mixed, respect LZ4F_compressBound() */
TEST(compressUpdate_mixedWithUncompressed)
{
    size_t const blockSize = 64 KB;
    static const size_t bufferedSizes[] = { 1, 1 KB, (64 KB) - 1 };
    static const size_t inputSizes[] = { 0, 1, (64 KB) - 1, 64 KB, (64 KB) + 1, 128 KB };
    size_t const frameCapacity = 4 * blockSize + 64;
    size_t const dstCapacity = 2 * frameCapacity;
    const unsigned char* const src = (const unsigned char*)UT_noise(3 * blockSize);   /* largest possible block output */
    unsigned char* const frame = (unsigned char*)UT_malloc(frameCapacity);
    unsigned char* const dst = (unsigned char*)UT_malloc(dstCapacity + 1);
    unsigned char* const decoded = (unsigned char*)UT_malloc(3 * blockSize);
    LZ4F_cctx* const cctx = newCctx();
    LZ4F_dctx* const dctx = newDctx();
    unsigned linked, checksums, uncompressedFirst, autoFlush;
    size_t b, i;

    for (linked = 0; linked < 2; ++linked)
    for (checksums = 0; checksums < 4; ++checksums)
    for (uncompressedFirst = 0; uncompressedFirst < 2; ++uncompressedFirst)
    for (autoFlush = 0; autoFlush < 2; ++autoFlush)
    for (b = 0; b < ARRAY_SIZE(bufferedSizes); ++b)
    for (i = 0; i < ARRAY_SIZE(inputSizes); ++i) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        size_t const firstSize = bufferedSizes[b];
        size_t const secondSize = inputSizes[i];
        size_t const srcSize = firstSize + secondSize;
        size_t frameSize, cSize, bound;
        unsigned update;

        prefs.frameInfo.blockMode = linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
        prefs.frameInfo.blockChecksumFlag = (LZ4F_blockChecksum_t)(checksums & 1);
        prefs.frameInfo.contentChecksumFlag = (LZ4F_contentChecksum_t)(checksums >> 1);
        prefs.frameInfo.contentSize = srcSize;
        prefs.autoFlush = autoFlush;
        frameSize = LZ4F_OK(LZ4F_compressBegin(cctx, frame, frameCapacity, &prefs));

        for (update = 0; update < 2; ++update) {
            size_t const inputSize = update ? secondSize : firstSize;
            const unsigned char* const input = src + (update ? firstSize : 0);
            unsigned const uncompressed = uncompressedFirst ^ update;
            bound = LZ4F_compressBound(inputSize, &prefs);
            ASSERT(bound <= dstCapacity);
            dst[bound] = 0xA5;
            cSize = uncompressed
                  ? LZ4F_uncompressedUpdate(cctx, dst, bound, input, inputSize, NULL)
                  : LZ4F_compressUpdate(cctx, dst, bound, input, inputSize, NULL);
            ASSERT_MSG(!LZ4F_isError(cSize) && cSize <= bound && dst[bound] == 0xA5,
                       "mixed update exceeds its bound or fails : first=%u, second=%u, "
                       "uncompressed=%u, linked=%u, checksums=%u, autoFlush=%u",
                       (unsigned)firstSize, (unsigned)secondSize, uncompressed, linked, checksums, autoFlush);
            if (update == 0 && !autoFlush) ASSERT_EQ(cSize, 0);
            ASSERT(cSize <= frameCapacity - frameSize);
            memcpy(frame + frameSize, dst, cSize);
            frameSize += cSize;
        }

        bound = LZ4F_compressBound(0, &prefs);
        ASSERT(bound < frameCapacity - frameSize);
        frame[frameSize + bound] = 0xA5;
        cSize = LZ4F_OK(LZ4F_compressEnd(cctx, frame + frameSize, bound, NULL));
        ASSERT(cSize <= bound && frame[frameSize + bound] == 0xA5);
        frameSize += cSize;
        {   size_t consumed = frameSize;
            size_t decodedSize = 3 * blockSize;
            ASSERT_EQ(LZ4F_OK(LZ4F_decompress(dctx, decoded, &decodedSize, frame, &consumed, NULL)), 0);
            ASSERT_EQ(consumed, frameSize);
            ASSERT_EQ(decodedSize, srcSize);
            ASSERT_MEMEQ(decoded, src, srcSize);
        }

        /* A mode switch writes 2 blocks : the flushed one, then the new one.
         * Capacity is only required for what is actually written.
         * Insufficient capacity fails without writing beyond it, and invalidates the frame. */
        if (!autoFlush && firstSize == blockSize - 1 && secondSize == blockSize) {
            size_t const blockOverhead = 4 + 4 * prefs.frameInfo.blockChecksumFlag;
            size_t const flushSize = firstSize + blockOverhead;   /* incompressible => raw */
            size_t const exactSize = flushSize + secondSize + blockOverhead;
            size_t const capacities[] = { 0, flushSize - 1, flushSize, exactSize - 1, exactSize };
            size_t s;
            for (s = 0; s < ARRAY_SIZE(capacities); ++s) {
                size_t const capacity = capacities[s];
                LZ4F_OK(LZ4F_compressBegin(cctx, frame, frameCapacity, &prefs));
                bound = LZ4F_compressBound(firstSize, &prefs);
                ASSERT_EQ(LZ4F_OK(uncompressedFirst
                                  ? LZ4F_uncompressedUpdate(cctx, frame, bound, src, firstSize, NULL)
                                  : LZ4F_compressUpdate(cctx, frame, bound, src, firstSize, NULL)), 0);
                dst[capacity] = 0xA5;
                cSize = uncompressedFirst
                      ? LZ4F_compressUpdate(cctx, dst, capacity, src + firstSize, secondSize, NULL)
                      : LZ4F_uncompressedUpdate(cctx, dst, capacity, src + firstSize, secondSize, NULL);
                ASSERT_EQ(dst[capacity], 0xA5);
                if (capacity >= exactSize) {
                    ASSERT_EQ(cSize, exactSize);
                } else {
                    ASSERT_LZ4F_ERROR(cSize, LZ4F_ERROR_dstCapacity_tooSmall);
                    ASSERT_LZ4F_ERROR(LZ4F_compressEnd(cctx, frame, frameCapacity, NULL), LZ4F_ERROR_compressionState_uninitialized);
                }
            }
        }
    }
}

/* LZ4F_uncompressedUpdate() with linked blocks : the decoder's history contains uncompressed data,
 * which blocks compressed next must not mistake for previous content (nor for the dictionary).
 * Compressible input (copies within 32 KB), and a dictionary made of input, so that they would reference it.
 * Frames ending with uncompressed data are followed by frames starting with uncompressed data (same cctx). */
TEST(uncompressedUpdate_linkedBlocks)
{
    static const int levels[] = { 1, 9, 12 };
    /* segments : C compressed, U uncompressed ; lowercase : small segment */
    static const char* const scripts[] = { "UC", "uC", "CUC", "CuC", "CUUC", "UCUC", "CUCuC", "uCu", "UCU" };
    size_t const segSize = 150 KB, smallSize = 1000, dictSize = 64 KB;
    size_t const srcSize = 5 * segSize;
    size_t const frameCapacity = LZ4F_compressFrameBound(srcSize, NULL) + 5 * LZ4F_compressBound(0, NULL);
    const char* const src = (const char*)UT_sample(srcSize);
    char* const dict = (char*)UT_malloc(dictSize);
    char* const frame = (char*)UT_malloc(frameCapacity);
    char* const decoded = (char*)UT_malloc(srcSize);
    LZ4F_cctx* const cctx = newCctx();
    LZ4F_dctx* const dctx = newDctx();
    LZ4F_CDict* cdict;
    size_t l, sc;
    unsigned useDict, stable;
    memcpy(dict, src, dictSize);
    cdict = newCDict(dict, dictSize);

    for (l = 0; l < ARRAY_SIZE(levels); l++)
    for (sc = 0; sc < ARRAY_SIZE(scripts); sc++)
    for (useDict = 0; useDict < 2; useDict++)
    for (stable = 0; stable < 2; stable++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        LZ4F_compressOptions_t cOpts;
        const char* op;
        size_t pos, consumed = 0;
        memset(&cOpts, 0, sizeof(cOpts));
        cOpts.stableSrc = stable;
        prefs.compressionLevel = levels[l];
        prefs.frameInfo.blockMode = LZ4F_blockLinked;
        prefs.frameInfo.blockSizeID = LZ4F_max64KB;
        prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        pos = LZ4F_OK(LZ4F_compressBegin_usingCDict(cctx, frame, frameCapacity, useDict ? cdict : NULL, &prefs));
        for (op = scripts[sc]; *op; op++) {
            size_t const size = (*op == 'u' || *op == 'c') ? smallSize : segSize;
            pos += LZ4F_OK((*op == 'U' || *op == 'u')
                           ? LZ4F_uncompressedUpdate(cctx, frame + pos, frameCapacity - pos, src + consumed, size, &cOpts)
                           : LZ4F_compressUpdate(cctx, frame + pos, frameCapacity - pos, src + consumed, size, &cOpts));
            consumed += size;
        }
        pos += LZ4F_OK(LZ4F_compressEnd(cctx, frame + pos, frameCapacity - pos, &cOpts));
        {   size_t consumedFrame = pos, decodedSize = srcSize;
            size_t const dResult = LZ4F_decompress_usingDict(dctx, decoded, &decodedSize, frame, &consumedFrame,
                                                             useDict ? dict : NULL, useDict ? dictSize : 0, NULL);
            ASSERT_MSG(!LZ4F_isError(dResult) && dResult == 0 && consumedFrame == pos
                       && decodedSize == consumed && !memcmp(src, decoded, consumed),
                       "segments %s, level %i, dict %u, stableSrc %u : round trip failed (%s)",
                       scripts[sc], levels[l], useDict, stable,
                       LZ4F_isError(dResult) ? LZ4F_getErrorName(dResult) : "content differs");
    }   }
}

/* favorDecSpeed applies to all blocks of a frame, including after an incompressible block :
 * with independent blocks, a block doesn't depend on the previous ones */
TEST(favorDecSpeed_afterRawBlock)
{
    size_t const blockSize = 64 KB;
    size_t const headerSize = 7;   /* no content size, no dictID */
    char* const src = (char*)UT_noise(2 * blockSize);   /* incompressible block, then compressible block */
    size_t const capacity = LZ4F_compressFrameBound(2 * blockSize, NULL);
    char* const frame = (char*)UT_malloc(capacity);
    char* const alone = (char*)UT_malloc(capacity);
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t frameSize, aloneSize, cBlockSize;
    UT_fillSample(src + blockSize, blockSize);
    prefs.frameInfo.blockMode = LZ4F_blockIndependent;
    prefs.frameInfo.blockSizeID = LZ4F_max64KB;
    prefs.compressionLevel = 10;   /* optimal parser : favorDecSpeed only applies there */

    /* this test is only meaningful if favorDecSpeed changes the compressible block */
    frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src + blockSize, blockSize, &prefs));
    prefs.favorDecSpeed = 1;
    aloneSize = LZ4F_OK(LZ4F_compressFrame(alone, capacity, src + blockSize, blockSize, &prefs));
    ASSERT_MSG(frameSize != aloneSize || memcmp(frame, alone, aloneSize),
               "favorDecSpeed doesn't change the output of this test input");

    /* the same block, after an incompressible one, must be identical */
    frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, 2 * blockSize, &prefs));
    ASSERT_MSG(readLE32(frame + headerSize) == (0x80000000U | (unsigned)blockSize), "first block should be stored raw");
    cBlockSize = 4 + (readLE32(alone + headerSize) & 0x7FFFFFFFU);   /* block header included */
    ASSERT_EQ(frameSize, headerSize + (4 + blockSize) + cBlockSize + 4);
    ASSERT_MEMEQ(frame + headerSize + 4 + blockSize, alone + headerSize, cBlockSize);   /* favorDecSpeed lost ? */
}

/* favorDecSpeed applies to frames started with a dictionary :
 * neither loading a dictionary (LZ4F_compressBegin_usingDict())
 * nor attaching a CDict (LZ4F_compressBegin_usingCDict()) may reset it */
TEST(favorDecSpeed_withDict)
{
    size_t const dictSize = 64 KB;
    size_t const srcSize = 64 KB;
    const char* const dict = (const char*)UT_sample(dictSize);
    const char* const src = (const char*)UT_sample(srcSize);
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL) + LZ4F_HEADER_SIZE_MAX;
    char* const ref = (char*)UT_malloc(capacity);
    char* const fds = (char*)UT_malloc(capacity);
    LZ4F_CDict* const cdict = newCDict(dict, dictSize);
    LZ4F_cctx* const cctx = newCctx();
    unsigned blockMode, beginMode;

    /* beginMode : 0 = no dictionary (checks this test input is sensitive to favorDecSpeed), 1 = dict, 2 = CDict */
    for (blockMode = 0; blockMode < 2; blockMode++)
    for (beginMode = 0; beginMode < 3; beginMode++) {
        const char* const d = (beginMode == 1) ? dict : NULL;
        const LZ4F_CDict* const cd = (beginMode == 2) ? cdict : NULL;
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        size_t refSize, fdsSize;
        prefs.frameInfo.blockMode = blockMode ? LZ4F_blockIndependent : LZ4F_blockLinked;
        prefs.compressionLevel = 10;   /* optimal parser : favorDecSpeed only applies there */
        refSize = LZ4F_OK(streamFrame(cctx, ref, capacity, src, srcSize, d, dictSize, cd, &prefs));
        prefs.favorDecSpeed = 1;
        fdsSize = LZ4F_OK(streamFrame(cctx, fds, capacity, src, srcSize, d, dictSize, cd, &prefs));
        ASSERT_MSG(refSize != fdsSize || memcmp(ref, fds, fdsSize),
                   "blockMode=%u, beginMode=%u : favorDecSpeed doesn't change the output (lost ?)", blockMode, beginMode);
        assertDecodesTo(fds, fdsSize, beginMode ? dict : NULL, beginMode ? dictSize : 0, src, srcSize);
    }
}

/* skipChecksums disables verification of all checksums :
 * block checksums, of compressed and uncompressed blocks, and content checksum */
TEST(decompress_skipChecksums)
{
    size_t const blockSize = 64 KB;
    size_t const srcSize = 3 * blockSize;   /* 2nd block is stored uncompressed */
    char* const src = (char*)UT_sample(srcSize);
    char* const decoded = (char*)UT_malloc(srcSize);
    LZ4F_dctx* const dctx = newDctx();
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t ends[8] = { 0 };
    size_t capacity, frameSize;
    unsigned char* frame;
    unsigned corrupted;
    UT_fillNoise(src + blockSize, blockSize);
    prefs.frameInfo.blockSizeID = LZ4F_max64KB;
    prefs.frameInfo.blockChecksumFlag = LZ4F_blockChecksumEnabled;
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    capacity = LZ4F_compressFrameBound(srcSize, &prefs);
    frame = (unsigned char*)UT_malloc(capacity);
    frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, srcSize, &prefs));
    ASSERT_EQ(frameElementEnds(ends, ARRAY_SIZE(ends), frame, frameSize), 6);   /* header, 3 blocks, endMark, checksum */
    ASSERT_MSG(!(readLE32(frame + ends[0]) & 0x80000000U) && (readLE32(frame + ends[1]) & 0x80000000U),
               "1st block should be compressed, and 2nd block uncompressed");

    for (corrupted = 0; corrupted < 3; corrupted++) {   /* checksum of : compressed block, uncompressed block, content */
        size_t const crcPos = (corrupted == 2) ? frameSize - 1 : ends[corrupted + 1] - 1;
        LZ4F_errorCodes const expected = (corrupted == 2) ? LZ4F_ERROR_contentChecksum_invalid : LZ4F_ERROR_blockChecksum_invalid;
        LZ4F_decompressOptions_t dOpt;
        size_t srcLen = frameSize, dstLen = srcSize, r;
        frame[crcPos] = (unsigned char)(frame[crcPos] ^ 1);
        LZ4F_resetDecompressionContext(dctx);
        r = LZ4F_decompress(dctx, decoded, &dstLen, frame, &srcLen, NULL);
        ASSERT_MSG(LZ4F_getErrorCode(r) == expected, "checksum %u : %s", corrupted,
                   LZ4F_isError(r) ? LZ4F_getErrorName(r) : "corruption not detected");
        memset(&dOpt, 0, sizeof(dOpt));
        dOpt.skipChecksums = 1;
        srcLen = frameSize;
        dstLen = srcSize;
        LZ4F_resetDecompressionContext(dctx);
        r = LZ4F_decompress(dctx, decoded, &dstLen, frame, &srcLen, &dOpt);
        ASSERT_MSG(r == 0 && srcLen == frameSize && dstLen == srcSize && !memcmp(decoded, src, srcSize),
                   "checksum %u : %s", corrupted, LZ4F_isError(r) ? LZ4F_getErrorName(r) : "not skipped");
        frame[crcPos] = (unsigned char)(frame[crcPos] ^ 1);
    }
}

/* Level 12 with linked blocks, on short-period data (ABAB...) :
 * all candidates in the 64 KB history match up to the end of the block.
 * Comparing each of them in full (up to 16384 x 64 KB per block)
 * makes it ~40x slower than level 12 on regular data.
 * CPU time is compared to a reference of the same size,
 * so that the test doesn't depend on platform speed (valgrind, qemu, sanitizers). */
TEST(compressFrame_level12_periodicLinked)
{
    size_t const srcSize = 1 MB;
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    char* const periodic = (char*)UT_malloc(srcSize);
    const char* const regular = (const char*)UT_sample(srcSize);
    char* const dst = (char*)UT_malloc(capacity);
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    clock_t start, refTime, periodicTime;
    size_t n;
    for (n = 0; n < srcSize; n++) periodic[n] = (n & 1) ? 'B' : 'A';
    prefs.frameInfo.blockSizeID = LZ4F_max64KB;
    prefs.frameInfo.blockMode = LZ4F_blockLinked;
    prefs.compressionLevel = 12;

    start = clock();
    LZ4F_OK(LZ4F_compressFrame(dst, capacity, regular, srcSize, &prefs));
    refTime = clock() - start;
    start = clock();
    LZ4F_OK(LZ4F_compressFrame(dst, capacity, periodic, srcSize, &prefs));
    periodicTime = clock() - start;
    ASSERT_MSG(periodicTime <= refTime, "periodic data : %.3f s, reference : %.3f s",
               (double)periodicTime / CLOCKS_PER_SEC, (double)refTime / CLOCKS_PER_SEC);
}



/*-************************************
*  Compression into dst capacities < compressBound (#1817)
*  Bounds guarantee success, they are not requirements :
*  compression must succeed if and only if its output fits,
*  and then produce the same output as with a bound-sized buffer.
**************************************/
#define SD_CANARY 0xA5
#define SD_CANARY_SIZE 8
#define SD_ELTS_MAX 256

static int sd_isTooSmall(size_t r)
{
    return LZ4F_getErrorCode(r) == LZ4F_ERROR_dstCapacity_tooSmall;
}

static int sd_isUninitialized(size_t r)
{
    return LZ4F_getErrorCode(r) == LZ4F_ERROR_compressionState_uninitialized;
}

static void sd_setCanary(BYTE* p) { memset(p, SD_CANARY, SD_CANARY_SIZE); }

static int sd_canaryIntact(const BYTE* p)
{
    size_t n;
    for (n = 0; n < SD_CANARY_SIZE; n++) if (p[n] != SD_CANARY) return 0;
    return 1;
}

/* sd_pickSmallCapacity() :
 * @return a capacity < @size, for an output starting at @start within a frame.
 * Favors capacities ending at, or just before, the end of a frame element. */
static size_t sd_pickSmallCapacity(const size_t* ends, size_t nbEnds, size_t start, size_t size)
{
    ASSERT(size > 0);
    switch (UT_rand() % 5) {
    case 0: return size - 1;
    case 1: return 0;
    case 2: return UT_rand() % size;
    default:
        {   size_t first = 0, last, n;
            while (first < nbEnds && ends[first] <= start) first++;
            last = first;
            while (last < nbEnds && ends[last] <= start + size) last++;
            if (first == last) return size - 1;
            n = first + UT_rand() % (last - first);
            if ((UT_rand() & 1) && (ends[n] < start + size)) return ends[n] - start;
            return ends[n] - start - 1;
    }   }
}

/* LZ4F_compressBegin*() only needs the actual header size */
TEST(compressBegin_smallDst)
{
    size_t const dictSize = 16 KB;
    size_t const srcSize = 100 KB;
    const BYTE* const src = (const BYTE*)UT_sample(srcSize);
    size_t const frameCapacity = LZ4F_compressFrameBound(srcSize, NULL) + LZ4F_HEADER_SIZE_MAX;
    BYTE* const frame = (BYTE*)UT_malloc(frameCapacity);
    BYTE dst[LZ4F_HEADER_SIZE_MAX + SD_CANARY_SIZE];
    LZ4F_CDict* const cdict = newCDict(src, dictSize);
    LZ4F_cctx* const cctx = newCctx();
    unsigned options, beginMode;

    for (options = 0; options < 4; options++)
    for (beginMode = 0; beginMode < 3; beginMode++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        unsigned const hasContentSize = options & 1;
        unsigned const hasDictID = options >> 1;
        size_t const headerSize = 7 + 8 * hasContentSize + 4 * hasDictID;
        size_t const capacities[] = { headerSize, headerSize - 1, 1, 0 };
        size_t c;
        prefs.frameInfo.contentSize = hasContentSize ? 123456 : 0;
        prefs.frameInfo.dictID = hasDictID ? 0x1817 : 0;
        for (c = 0; c < ARRAY_SIZE(capacities); c++) {
            size_t const capacity = capacities[c];
            size_t r;
            memset(dst, SD_CANARY, sizeof(dst));
            switch (beginMode) {
            case 0: r = LZ4F_compressBegin(cctx, dst, capacity, &prefs); break;
            case 1: r = LZ4F_compressBegin_usingDict(cctx, dst, capacity, src, dictSize, &prefs); break;
            default: r = LZ4F_compressBegin_usingCDict(cctx, dst, capacity, cdict, &prefs); break;
            }
            if (capacity >= headerSize) {
                ASSERT_MSG(r == headerSize, "options=%u, beginMode=%u, capacity=%u : %s",
                           options, beginMode, (unsigned)capacity,
                           LZ4F_isError(r) ? LZ4F_getErrorName(r) : "wrong header size");
                ASSERT_MSG(sd_canaryIntact(dst + capacity), "options=%u, beginMode=%u : write beyond capacity",
                           options, beginMode);
            } else {
                size_t n;
                ASSERT_MSG(sd_isTooSmall(r), "options=%u, beginMode=%u, capacity=%u : %s",
                           options, beginMode, (unsigned)capacity,
                           LZ4F_isError(r) ? LZ4F_getErrorName(r) : "should have failed");
                for (n = 0; n < sizeof(dst); n++)   /* nothing written at all */
                    ASSERT_MSG(dst[n] == SD_CANARY, "options=%u, beginMode=%u, capacity=%u : dst modified",
                               options, beginMode, (unsigned)capacity);
    }   }   }

    /* A failed begin must not disturb a frame in progress */
    {   LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        LZ4F_preferences_t otherPrefs = LZ4F_INIT_PREFERENCES;
        size_t pos = 0;
        prefs.frameInfo.blockMode = LZ4F_blockLinked;
        prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        prefs.frameInfo.contentSize = srcSize;
        otherPrefs.frameInfo.dictID = 7;
        otherPrefs.compressionLevel = 9;
        pos += LZ4F_OK(LZ4F_compressBegin(cctx, frame, frameCapacity, &prefs));
        pos += LZ4F_OK(LZ4F_compressUpdate(cctx, frame + pos, frameCapacity - pos, src, srcSize / 2, NULL));
        /* note : also checks the former error name remains available */
        ASSERT_LZ4F_ERROR(LZ4F_compressBegin(cctx, dst, 7 + 4 - 1, &otherPrefs), LZ4F_ERROR_dstMaxSize_tooSmall);
        pos += LZ4F_OK(LZ4F_compressUpdate(cctx, frame + pos, frameCapacity - pos, src + srcSize / 2, srcSize - srcSize / 2, NULL));
        pos += LZ4F_OK(LZ4F_compressEnd(cctx, frame + pos, frameCapacity - pos, NULL));
        assertDecodesTo(frame, pos, NULL, 0, src, srcSize);
    }
}


/* One-shot : LZ4F_compressFrame() and LZ4F_compressFrame_usingCDict() */

typedef struct {
    int usingCDict;             /* 0 => LZ4F_compressFrame() */
    LZ4F_cctx* cctx;            /* used by LZ4F_compressFrame_usingCDict() */
    const LZ4F_CDict* cdict;
    const LZ4F_preferences_t* prefs;
    const BYTE* src;
    size_t srcSize;
} sd_frameJob;

static size_t sd_compressFrame(const sd_frameJob* job, void* dst, size_t dstCapacity)
{
    if (!job->usingCDict)
        return LZ4F_compressFrame(dst, dstCapacity, job->src, job->srcSize, job->prefs);
    return LZ4F_compressFrame_usingCDict(job->cctx, dst, dstCapacity,
                                         job->src, job->srcSize, job->cdict, job->prefs);
}

/* @return 1 if compression into @capacity behaves as expected, 0 otherwise */
static int sd_frameCapacityOK(const sd_frameJob* job, BYTE* dst, size_t capacity,
                              const BYTE* ref, size_t refSize)
{
    size_t r;
    sd_setCanary(dst + capacity);
    r = sd_compressFrame(job, dst, capacity);
    if (!sd_canaryIntact(dst + capacity)) {
        printf("capacity %u : write beyond capacity \n", (unsigned)capacity);
        return 0;
    }
    if (capacity >= refSize) {
        if (r != refSize) {
            printf("capacity %u >= frameSize %u : %s \n", (unsigned)capacity, (unsigned)refSize,
                   LZ4F_isError(r) ? LZ4F_getErrorName(r) : "different frame size");
            return 0;
        }
        if (memcmp(dst, ref, refSize)) {
            printf("capacity %u >= frameSize %u : different frame content \n",
                   (unsigned)capacity, (unsigned)refSize);
            return 0;
        }
    } else if (!sd_isTooSmall(r)) {
        printf("capacity %u < frameSize %u : %s \n", (unsigned)capacity, (unsigned)refSize,
               LZ4F_isError(r) ? LZ4F_getErrorName(r) : "should have failed");
        return 0;
    }
    return 1;
}

TEST(compressFrame_smallDst)
{
    /* source layout : zeroes, then compressible noise, then incompressible noise */
    size_t const zeroSize = 1 KB;
    size_t const noiseSize = 136 KB;
    size_t const randSize = 72 KB;
    size_t const srcBufSize = zeroSize + noiseSize + randSize;
    static const struct { size_t pos; size_t size; } inputs[] = {
        { 0, 0 },                       /* empty frame */
        { 0, 1 },                       /* single byte */
        { 1 KB, 12 },                   /* too short to be compressed */
        { 0, 300 },                     /* small, highly compressible */
        { 1 KB, 5000 },                 /* one compressed block */
        { 137 KB, 70 KB },              /* incompressible : raw blocks */
        { 1 KB, 136 KB },               /* several compressed blocks */
        { 105 KB, 100 KB }              /* compressed, then raw blocks */
    };
    static const int levels[] = { -1, 1, 2, 3, 10 };   /* fast, accelerated, mid, hash chain, optimal */
    size_t const dictSize = 32 KB;
    BYTE* const src = (BYTE*)UT_malloc(srcBufSize);
    const BYTE* const dict = src + zeroSize + 64 KB;
    size_t buffSize;
    BYTE* ref;
    BYTE* dst;
    LZ4F_CDict* cdict;
    LZ4F_cctx* const cctx = newCctx();
    size_t ends[SD_ELTS_MAX] = { 0 };
    size_t in, lv;
    unsigned blockMode;

    {   LZ4F_preferences_t worstPrefs = LZ4F_INIT_PREFERENCES;
        worstPrefs.frameInfo.blockSizeID = LZ4F_max64KB;
        worstPrefs.frameInfo.blockChecksumFlag = LZ4F_blockChecksumEnabled;
        worstPrefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        buffSize = LZ4F_compressFrameBound(srcBufSize, &worstPrefs) + SD_CANARY_SIZE;
    }
    ref = (BYTE*)UT_malloc(buffSize);
    dst = (BYTE*)UT_malloc(buffSize);
    memset(src, 0, zeroSize);
    UT_fillSample(src + zeroSize, noiseSize);
    UT_fillNoise(src + zeroSize + noiseSize, randSize);
    cdict = newCDict(dict, dictSize);

    for (in = 0; in < ARRAY_SIZE(inputs); in++)
    for (lv = 0; lv < ARRAY_SIZE(levels); lv++)
    for (blockMode = 0; blockMode < 2; blockMode++) {
        unsigned const configNb = (unsigned)((in * ARRAY_SIZE(levels) + lv) * 2 + blockMode);
        unsigned const flags = UT_rand();
        unsigned const api = configNb % 3;  /* 0: compressFrame; 1: usingCDict(NULL); 2: usingCDict(cdict) */
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        sd_frameJob job;
        size_t bound, refSize, nbEnds, n;

        ASSERT(inputs[in].pos + inputs[in].size <= srcBufSize);
        prefs.frameInfo.blockMode = (LZ4F_blockMode_t)blockMode;
        prefs.frameInfo.blockSizeID = ((flags >> 6) & 7) == 1 ? LZ4F_max256KB : LZ4F_max64KB;
        prefs.frameInfo.blockChecksumFlag = (LZ4F_blockChecksum_t)(flags & 1);
        prefs.frameInfo.contentChecksumFlag = (LZ4F_contentChecksum_t)((flags >> 1) & 1);
        prefs.frameInfo.contentSize = (flags >> 2) & 1;   /* any value != 0 => auto-corrected */
        prefs.frameInfo.dictID = ((flags >> 3) & 1) ? 0x1817 : 0;
        prefs.favorDecSpeed = (flags >> 4) & 1;
        prefs.compressionLevel = levels[lv];
        job.usingCDict = (api != 0);
        job.cctx = cctx;
        job.cdict = (api == 2) ? cdict : NULL;
        job.prefs = &prefs;
        job.src = src + inputs[in].pos;
        job.srcSize = inputs[in].size;

        /* reference, using a bound-sized buffer */
        bound = LZ4F_compressFrameBound(job.srcSize, &prefs);
        ASSERT(bound + SD_CANARY_SIZE <= buffSize);
        refSize = LZ4F_OK(sd_compressFrame(&job, ref, bound));
        assertDecodesTo(ref, refSize, job.cdict ? dict : NULL, job.cdict ? dictSize : 0, job.src, job.srcSize);
        nbEnds = frameElementEnds(ends, SD_ELTS_MAX, ref, refSize);
        ASSERT_MSG(nbEnds > 1, "input %u, level %i : reference frame is malformed", (unsigned)in, levels[lv]);   /* at least header and end mark */

        /* exact size, and anything larger, must succeed */
        ASSERT_MSG(sd_frameCapacityOK(&job, dst, refSize, ref, refSize), "exact capacity");
        if (bound > refSize)
            ASSERT_MSG(sd_frameCapacityOK(&job, dst, refSize + 1 + UT_rand() % (bound - refSize), ref, refSize),
                       "capacity > frameSize");

        /* anything smaller must fail */
        if (refSize <= 300) {
            for (n = 0; n < refSize; n++)
                ASSERT_MSG(sd_frameCapacityOK(&job, dst, n, ref, refSize), "small capacity");
        } else {
            ASSERT_MSG(sd_frameCapacityOK(&job, dst, 0, ref, refSize), "zero capacity");
            for (n = 0; n < nbEnds; n++)   /* just before the end of each element */
                ASSERT_MSG(sd_frameCapacityOK(&job, dst, ends[n] - 1, ref, refSize), "element %u", (unsigned)n);
            n = UT_rand() % (nbEnds - 1);   /* exactly at the end of an element (but the last one) */
            ASSERT_MSG(sd_frameCapacityOK(&job, dst, ends[n], ref, refSize), "end of element %u", (unsigned)n);
            ASSERT_MSG(sd_frameCapacityOK(&job, dst, UT_rand() % refSize, ref, refSize), "random capacity");
        }

        /* a context remains usable after a failure */
        if (job.usingCDict) {
            size_t r = LZ4F_compressFrame_usingCDict(cctx, dst, refSize / 2, job.src, job.srcSize, job.cdict, &prefs);
            ASSERT_MSG(sd_isTooSmall(r), "%s", LZ4F_isError(r) ? LZ4F_getErrorName(r) : "should have failed");
            r = LZ4F_OK(LZ4F_compressFrame_usingCDict(cctx, dst, bound, job.src, job.srcSize, job.cdict, &prefs));
            assertDecodesTo(dst, r, job.cdict ? dict : NULL, job.cdict ? dictSize : 0, job.src, job.srcSize);
        }
    }
}


/* Streaming : LZ4F_compressBegin*(), LZ4F_compressUpdate(),
 *             LZ4F_uncompressedUpdate(), LZ4F_flush(), LZ4F_compressEnd() */

typedef enum { sd_begin, sd_update, sd_uncompressedUpdate, sd_flush, sd_end } sd_opType_e;

typedef struct {
    sd_opType_e type;
    size_t srcPos;
    size_t srcSize;
} sd_op;

typedef struct {
    LZ4F_cctx* cctx;
    const LZ4F_preferences_t* prefs;
    LZ4F_compressOptions_t cOpts;
    const BYTE* src;
    unsigned beginMode;     /* 0: plain; 1: usingDict; 2: usingCDict */
    const void* dict;
    size_t dictSize;
    const LZ4F_CDict* cdict;
} sd_stream;

static size_t sd_runOp(const sd_stream* s, const sd_op* op, void* dst, size_t dstCapacity)
{
    switch (op->type) {
    case sd_begin:
        if (s->beginMode == 1)
            return LZ4F_compressBegin_usingDict(s->cctx, dst, dstCapacity, s->dict, s->dictSize, s->prefs);
        if (s->beginMode == 2)
            return LZ4F_compressBegin_usingCDict(s->cctx, dst, dstCapacity, s->cdict, s->prefs);
        return LZ4F_compressBegin(s->cctx, dst, dstCapacity, s->prefs);
    case sd_update:
        return LZ4F_compressUpdate(s->cctx, dst, dstCapacity, s->src + op->srcPos, op->srcSize, &s->cOpts);
    case sd_uncompressedUpdate:
        return LZ4F_uncompressedUpdate(s->cctx, dst, dstCapacity, s->src + op->srcPos, op->srcSize, &s->cOpts);
    case sd_flush:
        return LZ4F_flush(s->cctx, dst, dstCapacity, &s->cOpts);
    case sd_end:
        return LZ4F_compressEnd(s->cctx, dst, dstCapacity, &s->cOpts);
    }
    return (size_t)-1;   /* error */
}

static size_t sd_opBound(const sd_stream* s, const sd_op* op)
{
    switch (op->type) {
    case sd_begin: return LZ4F_HEADER_SIZE_MAX;
    case sd_update:
    case sd_uncompressedUpdate: return LZ4F_compressBound(op->srcSize, s->prefs);
    case sd_flush:
    case sd_end: return LZ4F_compressBound(0, s->prefs);
    }
    return 0;
}

static const char* sd_opName(sd_opType_e type)
{
    switch (type) {
    case sd_begin: return "begin";
    case sd_update: return "compressUpdate";
    case sd_uncompressedUpdate: return "uncompressedUpdate";
    case sd_flush: return "flush";
    case sd_end: return "compressEnd";
    }
    return "unknown";
}

/* sd_genScript() :
 * begin, then a random sequence of updates and flushes, then end.
 * Input is consumed sequentially from position 0.
 * @return : nb of operations */
static size_t sd_genScript(sd_op* ops, size_t maxOps, size_t blockSize, size_t srcCapacity, size_t* totalSrcSize)
{
    size_t const nbMiddle = UT_rand() % (maxOps - 1);
    size_t nbOps = 0, pos = 0, n;
    ASSERT(maxOps >= 2);
    ops[nbOps].type = sd_begin; ops[nbOps].srcPos = 0; ops[nbOps].srcSize = 0; nbOps++;
    for (n = 0; n < nbMiddle; n++) {
        unsigned const t = UT_rand() % 8;
        size_t size = 0;
        sd_op* const op = ops + nbOps++;
        op->type = (t == 0) ? sd_flush : (t <= 2) ? sd_uncompressedUpdate : sd_update;
        if (op->type != sd_flush) {
            switch (UT_rand() % 8) {
            case 0: size = 0; break;
            case 1: size = 1; break;
            case 2: size = 2 + UT_rand() % 100; break;
            case 3: size = blockSize - 1; break;
            case 4: size = blockSize; break;
            case 5: size = blockSize + 1; break;
            case 6: size = UT_rand() % blockSize; break;
            default: size = 2 * blockSize + UT_rand() % blockSize; break;
            }
            size = MIN(size, srcCapacity - pos);
        }
        op->srcPos = pos;
        op->srcSize = size;
        pos += size;
    }
    ops[nbOps].type = sd_end; ops[nbOps].srcPos = pos; ops[nbOps].srcSize = 0; nbOps++;
    *totalSrcSize = pos;
    return nbOps;
}

/* sd_opExactOK() :
 * runs @op into exactly @refSize bytes (or @capacity if larger),
 * and checks it produces the same output as reference.
 * @return 1 if OK, 0 otherwise */
static int sd_opExactOK(const sd_stream* s, const sd_op* op, BYTE* dst, size_t capacity,
                        const BYTE* ref, size_t refSize)
{
    size_t r;
    ASSERT(capacity >= refSize);
    sd_setCanary(dst + capacity);
    r = sd_runOp(s, op, dst, capacity);
    if (!sd_canaryIntact(dst + capacity)) {
        printf("%s (srcSize %u) into capacity %u : write beyond capacity \n",
               sd_opName(op->type), (unsigned)op->srcSize, (unsigned)capacity);
        return 0;
    }
    if (r != refSize) {
        printf("%s (srcSize %u) into capacity %u (expected output %u) : %s \n",
               sd_opName(op->type), (unsigned)op->srcSize, (unsigned)capacity, (unsigned)refSize,
               LZ4F_isError(r) ? LZ4F_getErrorName(r) : "different output size");
        return 0;
    }
    if (memcmp(dst, ref, refSize)) {
        printf("%s (srcSize %u) into capacity %u : different output content \n",
               sd_opName(op->type), (unsigned)op->srcSize, (unsigned)capacity);
        return 0;
    }
    return 1;
}

/* sd_partialOpOK() :
 * LZ4F_flush() and LZ4F_compressEnd() fill dstBuffer, or complete :
 * runs @op into @capacity (< @refSize), then completes it with various capacities,
 * sometimes exactly the remaining size (then, a last invocation returns 0).
 * Concatenated output must be identical to reference. Capacity 0 is rejected, without modifying the frame.
 * @return 1 if OK, 0 otherwise */
static int sd_partialOpOK(const sd_stream* s, const sd_op* op, BYTE* dst, size_t capacity,
                          const BYTE* ref, size_t refSize)
{
    size_t pos = 0;
    unsigned nbCalls;
    for (nbCalls = 0; nbCalls < 1000; nbCalls++) {
        size_t r;
        sd_setCanary(dst + pos + capacity);
        r = sd_runOp(s, op, dst + pos, capacity);
        if (!sd_canaryIntact(dst + pos + capacity)) {
            printf("%s into capacity %u : write beyond capacity \n", sd_opName(op->type), (unsigned)capacity);
            return 0;
        }
        if (capacity == 0) {
            if (!sd_isTooSmall(r)) {
                printf("%s into capacity 0 : %s \n", sd_opName(op->type),
                       LZ4F_isError(r) ? LZ4F_getErrorName(r) : "should have been rejected");
                return 0;
            }
        } else {
            if (LZ4F_isError(r) || (r > capacity)) {
                printf("%s into capacity %u : %s \n", sd_opName(op->type), (unsigned)capacity,
                       LZ4F_isError(r) ? LZ4F_getErrorName(r) : "exceeds capacity");
                return 0;
            }
            pos += r;
            if (pos > refSize) {
                printf("%s : more output than reference (%u > %u) \n", sd_opName(op->type), (unsigned)pos, (unsigned)refSize);
                return 0;
            }
            if (r < capacity) break;   /* completed */
        }
        {   size_t const remaining = refSize - pos;
            switch (UT_rand() % 4) {
            case 0: capacity = remaining; break;
            case 1: capacity = 1 + UT_rand() % (remaining + 1); break;
            default: capacity = remaining + 1 + UT_rand() % 16; break;
            }
            if (capacity == 0) capacity = 1;
    }   }
    if ((pos != refSize) || memcmp(dst, ref, refSize)) {
        printf("%s : partial output differs from reference \n", sd_opName(op->type));
        return 0;
    }
    return 1;
}

/* sd_runScript() :
 * runs all @ops, each one using its bound as capacity, writing the frame into @frame.
 * @opStarts and @opSizes are optional.
 * @return : frame size, or an error code */
static size_t sd_runScript(const sd_stream* s, const sd_op* ops, size_t nbOps,
                           BYTE* frame, size_t frameCapacity,
                           size_t* opStarts, size_t* opSizes)
{
    size_t n, pos = 0;
    for (n = 0; n < nbOps; n++) {
        size_t const bound = sd_opBound(s, ops + n);
        size_t r;
        if (pos + bound + SD_CANARY_SIZE > frameCapacity) {
            printf("test buffer too small \n");
            return (size_t)-1;
        }
        sd_setCanary(frame + pos + bound);
        r = sd_runOp(s, ops + n, frame + pos, bound);
        if (LZ4F_isError(r)) {
            printf("op %u (%s, srcSize %u) : %s \n",
                   (unsigned)n, sd_opName(ops[n].type), (unsigned)ops[n].srcSize, LZ4F_getErrorName(r));
            return r;
        }
        if (r > bound || !sd_canaryIntact(frame + pos + bound)) {
            printf("op %u (%s, srcSize %u) : exceeds its bound \n",
                   (unsigned)n, sd_opName(ops[n].type), (unsigned)ops[n].srcSize);
            return (size_t)-1;
        }
        if (((ops[n].type == sd_flush) || (ops[n].type == sd_end)) && (r == bound)) {
            /* filling dstBuffer would mean "invoke again" : output must remain strictly below LZ4F_compressBound(0) */
            printf("op %u (%s) : output reaches its bound \n", (unsigned)n, sd_opName(ops[n].type));
            return (size_t)-1;
        }
        if (opStarts) opStarts[n] = pos;
        if (opSizes) opSizes[n] = r;
        pos += r;
    }
    return pos;
}

/* sd_newContext() :
 * each pass starts from a new context, so that replayed operations produce the same output :
 * the output of independent blocks < LZ4_CLEAR_TABLE_THRESHOLD depends on the context's history,
 * since their hash table is reused rather than cleared */
static void sd_newContext(sd_stream* s)
{
    if (s->cctx != NULL) UT_release(s->cctx);
    s->cctx = newCctx();
}

#define SD_OPS_MAX 16
#define SD_NB_CONFIGS 24

TEST(streaming_smallDst)
{
    size_t const srcBufSize = 192 KB;
    size_t const dictSize = 32 KB;
    static const int levels[] = { -2, 1, 2, 3, 10 };   /* fast, accelerated, mid, hash chain, optimal */
    BYTE* const src = (BYTE*)UT_malloc(srcBufSize);
    const BYTE* const dictBuf = (const BYTE*)UT_sample(dictSize);
    size_t frameCapacity;
    BYTE* frame;
    BYTE* dst;
    LZ4F_CDict* const cdict = newCDict(dictBuf, dictSize);
    sd_op ops[SD_OPS_MAX];
    size_t opSizes[SD_OPS_MAX] = { 0 };    /* written by sd_runScript() : initialized for static analyzers */
    size_t opStarts[SD_OPS_MAX] = { 0 };
    size_t ends[SD_ELTS_MAX] = { 0 };
    unsigned configNb;

    {   LZ4F_preferences_t worstPrefs = LZ4F_INIT_PREFERENCES;
        worstPrefs.frameInfo.blockSizeID = LZ4F_max64KB;
        worstPrefs.frameInfo.blockChecksumFlag = LZ4F_blockChecksumEnabled;
        worstPrefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        /* generous : each op can add a partial block */
        frameCapacity = LZ4F_compressFrameBound(srcBufSize, &worstPrefs)
                      + SD_OPS_MAX * LZ4F_compressBound(0, &worstPrefs) + SD_CANARY_SIZE;
    }
    frame = (BYTE*)UT_malloc(frameCapacity);
    dst = (BYTE*)UT_malloc(frameCapacity);
    /* alternate compressible and incompressible segments */
    {   size_t pos;
        for (pos = 0; pos < srcBufSize; pos += 64 KB) {
            size_t const segSize = MIN(64 KB, srcBufSize - pos);
            size_t const noiseSize = MIN(48 KB, segSize);
            UT_fillSample(src + pos, noiseSize);
            UT_fillNoise(src + pos + noiseSize, segSize - noiseSize);
    }   }

    for (configNb = 0; configNb < SD_NB_CONFIGS; configNb++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        sd_stream s;
        size_t blockSize, totalSrcSize, nbOps, refSize, nbEnds, n;
        unsigned probe;

        prefs.frameInfo.blockMode = (LZ4F_blockMode_t)(UT_rand() & 1);
        prefs.frameInfo.blockSizeID = (UT_rand() % 8 == 0) ? LZ4F_max256KB : LZ4F_max64KB;
        prefs.frameInfo.blockChecksumFlag = (LZ4F_blockChecksum_t)(UT_rand() & 1);
        prefs.frameInfo.contentChecksumFlag = (LZ4F_contentChecksum_t)(UT_rand() & 1);
        prefs.frameInfo.dictID = (UT_rand() & 1) ? 0x1817 : 0;
        prefs.autoFlush = (UT_rand() % 4) == 0;
        prefs.favorDecSpeed = UT_rand() & 1;
        prefs.compressionLevel = levels[UT_rand() % ARRAY_SIZE(levels)];
        blockSize = LZ4F_getBlockSize(prefs.frameInfo.blockSizeID);

        memset(&s, 0, sizeof(s));
        s.prefs = &prefs;
        s.cOpts.stableSrc = UT_rand() & 1;
        s.src = src;
        s.beginMode = UT_rand() % 3;
        s.dict = dictBuf;
        s.dictSize = dictSize;
        s.cdict = cdict;

        nbOps = sd_genScript(ops, SD_OPS_MAX, blockSize, srcBufSize, &totalSrcSize);
        if (UT_rand() & 1) prefs.frameInfo.contentSize = totalSrcSize;

        /* reference pass, each operation using its bound */
        sd_newContext(&s);
        refSize = sd_runScript(&s, ops, nbOps, frame, frameCapacity, opStarts, opSizes);
        ASSERT_MSG(!LZ4F_isError(refSize), "config %u : reference pass", configNb);
        assertDecodesTo(frame, refSize, s.beginMode ? dictBuf : NULL, s.beginMode ? dictSize : 0, src, totalSrcSize);
        nbEnds = frameElementEnds(ends, SD_ELTS_MAX, frame, refSize);
        ASSERT_MSG(nbEnds > 0, "config %u : reference frame is malformed", configNb);

        /* failure probes : replay up to op k, then give op k less than its output size */
        for (probe = 0; probe < 4; probe++) {
            size_t k = UT_rand() % nbOps;
            size_t capacity, r;
            while (opSizes[k] == 0) k = (k + 1) % nbOps;   /* begin and end always produce output */
            capacity = sd_pickSmallCapacity(ends, nbEnds, opStarts[k], opSizes[k]);
            sd_newContext(&s);
            for (n = 0; n < k; n++)
                ASSERT_MSG(sd_opExactOK(&s, ops + n, dst, opSizes[n], frame + opStarts[n], opSizes[n]),
                           "config %u, probe %u : replay op %u", configNb, probe, (unsigned)n);
            if ((ops[k].type == sd_flush) || (ops[k].type == sd_end)) {
                /* output in several invocations, the frame continues */
                ASSERT_MSG(sd_partialOpOK(&s, ops + k, dst, capacity, frame + opStarts[k], opSizes[k]),
                           "config %u, probe %u : partial op %u", configNb, probe, (unsigned)k);
                for (n = k + 1; n < nbOps; n++)
                    ASSERT_MSG(sd_opExactOK(&s, ops + n, dst, sd_opBound(&s, ops + n), frame + opStarts[n], opSizes[n]),
                               "config %u, probe %u : op %u after partial op %u", configNb, probe, (unsigned)n, (unsigned)k);
                continue;
            }
            sd_setCanary(dst + capacity);
            r = sd_runOp(&s, ops + k, dst, capacity);
            ASSERT_MSG(sd_canaryIntact(dst + capacity), "config %u : op %u (%s) writes beyond capacity",
                       configNb, (unsigned)k, sd_opName(ops[k].type));
            ASSERT_MSG(sd_isTooSmall(r), "config %u, op %u (%s, srcSize %u) into capacity %u < %u : %s",
                       configNb, (unsigned)k, sd_opName(ops[k].type), (unsigned)ops[k].srcSize,
                       (unsigned)capacity, (unsigned)opSizes[k],
                       LZ4F_isError(r) ? LZ4F_getErrorName(r) : "should have failed");
            /* the frame can't be continued anymore */
            {   sd_op next;
                next.type = (sd_opType_e)(sd_update + (probe % 4));
                next.srcPos = 0;
                next.srcSize = 1;
                r = sd_runOp(&s, &next, dst, frameCapacity);
                ASSERT_MSG(sd_isUninitialized(r), "config %u : %s after failed op %u (%s) : %s",
                           configNb, sd_opName(next.type), (unsigned)k, sd_opName(ops[k].type),
                           LZ4F_isError(r) ? LZ4F_getErrorName(r) : "should have failed");
            }
            /* the failed context remains usable for a new frame */
            if (probe == 0) {
                r = sd_runScript(&s, ops, nbOps, dst, frameCapacity, NULL, NULL);
                ASSERT_MSG(!LZ4F_isError(r), "config %u : new frame after failed op %u", configNb, (unsigned)k);
                assertDecodesTo(dst, r, s.beginMode ? dictBuf : NULL, s.beginMode ? dictSize : 0, src, totalSrcSize);
        }   }

        /* exact pass : each operation into exactly its output size, sometimes more */
        sd_newContext(&s);
        for (n = 0; n < nbOps; n++) {
            size_t const bound = sd_opBound(&s, ops + n);
            size_t capacity = opSizes[n];
            if ((UT_rand() % 4 == 0) && (bound > opSizes[n]))
                capacity += UT_rand() % (bound - opSizes[n]);
            ASSERT_MSG(sd_opExactOK(&s, ops + n, dst, capacity, frame + opStarts[n], opSizes[n]),
                       "config %u : exact pass, op %u", configNb, (unsigned)n);
        }
        UT_release(s.cctx);
    }
}


/*-************************************
*  Reproducibility : frames don't depend on the context's history
**************************************/
/* compresses @src with LZ4F_compressFrame_usingCDict() if @chunkSize == 0,
 * otherwise streams it by chunks of @chunkSize bytes.
 * @return : frame size, or an error code */
static size_t rf_compressFrame(LZ4F_cctx* cctx, BYTE* dst, size_t dstCapacity,
                               const BYTE* src, size_t srcSize, size_t chunkSize,
                               const LZ4F_CDict* cdict, const LZ4F_preferences_t* prefs)
{
    size_t pos, n, r;
    if (chunkSize == 0)
        return LZ4F_compressFrame_usingCDict(cctx, dst, dstCapacity, src, srcSize, cdict, prefs);
    pos = LZ4F_compressBegin_usingCDict(cctx, dst, dstCapacity, cdict, prefs);
    if (LZ4F_isError(pos)) return pos;
    for (n = 0; n < srcSize; n += chunkSize) {
        r = LZ4F_compressUpdate(cctx, dst + pos, dstCapacity - pos, src + n, MIN(chunkSize, srcSize - n), NULL);
        if (LZ4F_isError(r)) return r;
        pos += r;
    }
    r = LZ4F_compressEnd(cctx, dst + pos, dstCapacity - pos, NULL);
    if (LZ4F_isError(r)) return r;
    return pos + r;
}

TEST(compressFrame_reproducible)
{
    size_t const srcSize = 100 KB;     /* > 64 KB blocks : LZ4F_compressFrame*() keeps blocks linked */
    size_t const otherSize = 64 KB;    /* history frames, and dictionary */
    size_t const dictSize = 16 KB;
    static const int levels[] = { -3, 1, 2, 9, 10 };   /* accelerated, fast, mid, hash chain, optimal */
    static const size_t chunkSizes[] = { 0, 3000, 70 KB };
    BYTE* const src = (BYTE*)UT_sample(srcSize);
    const BYTE* const other = (const BYTE*)UT_sample(otherSize);
    size_t dstCapacity;
    BYTE* ref;
    BYTE* dst;
    LZ4F_CDict* const cdict = newCDict(other, dictSize);
    LZ4F_cctx* const cctx = newCctx();
    size_t lv, ch, variant;

    {   LZ4F_preferences_t worstPrefs = LZ4F_INIT_PREFERENCES;
        worstPrefs.frameInfo.blockSizeID = LZ4F_max64KB;
        worstPrefs.frameInfo.blockChecksumFlag = LZ4F_blockChecksumEnabled;
        worstPrefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        dstCapacity = LZ4F_compressFrameBound(srcSize, &worstPrefs);
    }
    ref = (BYTE*)UT_malloc(dstCapacity);
    dst = (BYTE*)UT_malloc(dstCapacity);
    /* the first 4 bytes reappear, with a different 5th byte :
     * a match only found when starting from a cleared table */
    memcpy(src, "ABCDx", 5);
    memcpy(src + 16, "ABCDy", 5);

    for (lv = 0; lv < ARRAY_SIZE(levels); lv++)
    for (ch = 0; ch < ARRAY_SIZE(chunkSizes); ch++)
    for (variant = 0; variant < 8; variant++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        const LZ4F_CDict* const cd = (variant & 1) ? cdict : NULL;
        size_t refSize, history;
        prefs.compressionLevel = levels[lv];
        prefs.frameInfo.blockMode = (LZ4F_blockMode_t)((variant >> 1) & 1);
        prefs.frameInfo.blockSizeID = LZ4F_max64KB;
        prefs.autoFlush = (variant >> 2) & 1;

        /* reference : a new context */
        {   LZ4F_cctx* const fresh = newCctx();
            refSize = LZ4F_OK(rf_compressFrame(fresh, ref, dstCapacity, src, srcSize, chunkSizes[ch], cd, &prefs));
            UT_release(fresh);
        }
        assertDecodesTo(ref, refSize, cd ? other : NULL, cd ? dictSize : 0, src, srcSize);

        /* the same frame, on a context with various histories */
        for (history = 0; history < 4; history++) {
            LZ4F_preferences_t hPrefs = LZ4F_INIT_PREFERENCES;
            size_t r;
            switch (history) {
            case 0: break;   /* whatever the previous frame was */
            case 1: hPrefs.compressionLevel = 1; break;                         /* fast, linked blocks */
            case 2: hPrefs.compressionLevel = 9; break;                         /* switch to HC */
            default: hPrefs.frameInfo.blockMode = LZ4F_blockIndependent; break; /* small table type */
            }
            if (history) LZ4F_OK(rf_compressFrame(cctx, dst, dstCapacity, other, otherSize, 1000, NULL, &hPrefs));
            r = LZ4F_OK(rf_compressFrame(cctx, dst, dstCapacity, src, srcSize, chunkSizes[ch], cd, &prefs));
            ASSERT_MSG(r == refSize && !memcmp(dst, ref, refSize),
                       "level %i, chunks %u, blockMode %u, autoFlush %u, cdict %u : "
                       "after history %u, frame differs from a new context's (%u vs %u bytes)",
                       prefs.compressionLevel, (unsigned)chunkSizes[ch], prefs.frameInfo.blockMode, prefs.autoFlush,
                       cd != NULL, (unsigned)history, (unsigned)r, (unsigned)refSize);
        }
    }
}


/*-************************************
*  Random access within a frame (#1304) :
*  a first pass, directed by hints, records the start of each block, within the frame and within decoded data.
*  Decoding can later resume from any block start.
*  decompress_randomAccess checks the recipe used by Wireshark, which works since v1.10.0, using a reset state :
*  the frame header is decoded again with LZ4F_getFrameInfo(),
*  the last 64 KB decoded before the block are provided with LZ4F_decompress_usingDict() (linked blocks only),
*  and checksums are skipped, since they can't be verified from the middle of a frame.
*  Note : frames declaring their content size can't be resumed this way.
*  resumeFrame checks LZ4F_resumeFrame(), which supports all frames.
**************************************/
/* ra_expectedHint() :
 * @ends : frame elements, as listed by frameElementEnds(). ends[@nbBlocks] is the start of endMark.
 * @return : hint expected after consuming @pos bytes of the frame,
 *           up to the end of next block header, or up to the end of frame once endMark is consumed */
static size_t ra_expectedHint(const size_t* ends, size_t nbBlocks, size_t frameSize, size_t pos)
{
    size_t k;
    for (k = 0; k <= nbBlocks; k++)
        if (ends[k] + 4 > pos) return ends[k] + 4 - pos;
    return frameSize - pos;
}

/* ra_indexFrame() :
 * decodes @frame into @decoded, providing exactly the hinted amount of input,
 * which stops decoding right after each block header, and checks hints along the way.
 * Records into @dStarts the position of each block start within decoded data,
 * and into @frameInfo the frame parameters.
 * @dict : the dictionary used to compress the frame, or NULL. */
static void ra_indexFrame(LZ4F_dctx* dctx, LZ4F_frameInfo_t* frameInfo,
                          const BYTE* frame, size_t frameSize, const size_t* ends, size_t nbBlocks,
                          const void* dict, size_t dictSize,
                          BYTE* decoded, size_t decodedSize, size_t* dStarts)
{
    size_t const hSize = ends[0];
    size_t pos = hSize, dPos = 0, k, r;
    LZ4F_resetDecompressionContext(dctx);
    r = LZ4F_getFrameInfo(dctx, frameInfo, frame, &pos);
    ASSERT_MSG(r == 4 && pos == hSize, "%s", LZ4F_isError(r) ? LZ4F_getErrorName(r) : "wrong header");
    for (k = 0; r != 0; k++) {
        LZ4F_frameInfo_t fi;
        size_t srcLen = r, dstLen = decodedSize - dPos, noInput = 0;
        r = LZ4F_OK(LZ4F_decompress_usingDict(dctx, decoded + dPos, &dstLen, frame + pos, &srcLen, dict, dictSize, NULL));
        pos += srcLen;
        dPos += dstLen;
        ASSERT_MSG(r == ra_expectedHint(ends, nbBlocks, frameSize, pos), "pos %u : wrong hint %u", (unsigned)pos, (unsigned)r);
        if (r == 0) break;
        ASSERT_MSG(k <= nbBlocks && pos == ends[k] + 4, "pos %u : not after a block header", (unsigned)pos);
        dStarts[k] = dPos;
        /* LZ4F_getFrameInfo() mid-frame provides the same hint */
        ASSERT_MSG(LZ4F_getFrameInfo(dctx, &fi, NULL, &noInput) == r,
                   "pos %u : LZ4F_getFrameInfo() provides a different hint", (unsigned)pos);
    }
    ASSERT_MSG(pos == frameSize && dPos == decodedSize, "frame not fully decoded");
}

TEST(decompress_randomAccess)
{
    size_t const blockSize = 64 KB;
    size_t const srcSize = 5 * blockSize + 1000;   /* last block is partial */
    BYTE* const src = (BYTE*)UT_sample(srcSize);
    BYTE* const decoded = (BYTE*)UT_malloc(srcSize);
    size_t frameCapacity;
    BYTE* frame;
    LZ4F_dctx* const dctx = newDctx();
    unsigned variant, pass;

    {   LZ4F_preferences_t worstPrefs = LZ4F_INIT_PREFERENCES;
        worstPrefs.frameInfo.blockSizeID = LZ4F_max64KB;
        worstPrefs.frameInfo.blockChecksumFlag = LZ4F_blockChecksumEnabled;
        worstPrefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        frameCapacity = LZ4F_compressFrameBound(srcSize, &worstPrefs);
    }
    frame = (BYTE*)UT_malloc(frameCapacity);
    UT_fillNoise(src + 2 * blockSize, blockSize);   /* 3rd block is stored uncompressed */

    for (variant = 0; variant < 8; variant++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        LZ4F_frameInfo_t fi;
        size_t ends[SD_ELTS_MAX] = { 0 }, dStarts[SD_ELTS_MAX] = { 0 };
        size_t frameSize, nbElts, nbBlocks, hSize, pos, dPos, k, r;
        prefs.frameInfo.blockMode = (LZ4F_blockMode_t)(variant & 1);
        prefs.frameInfo.blockSizeID = LZ4F_max64KB;
        prefs.frameInfo.blockChecksumFlag = (LZ4F_blockChecksum_t)((variant >> 1) & 1);
        prefs.frameInfo.contentChecksumFlag = (LZ4F_contentChecksum_t)((variant >> 2) & 1);
        frameSize = LZ4F_OK(LZ4F_compressFrame(frame, frameCapacity, src, srcSize, &prefs));
        nbElts = frameElementEnds(ends, SD_ELTS_MAX, frame, frameSize);
        ASSERT_MSG(nbElts > 0, "variant %u : malformed frame", variant);
        nbBlocks = nbElts - 2 - prefs.frameInfo.contentChecksumFlag;   /* minus header, endMark, and content checksum */
        hSize = ends[0];

        /* first pass : records block starts */
        ra_indexFrame(dctx, &fi, frame, frameSize, ends, nbBlocks, NULL, 0, decoded, srcSize, dStarts);
        ASSERT_MSG(!memcmp(decoded, src, srcSize), "variant %u : first pass corrupted", variant);

        /* hints remain exact whatever the amount of input and output */
        for (pass = 0; pass < 16; pass++) {
            LZ4F_resetDecompressionContext(dctx);
            pos = hSize;
            r = LZ4F_OK(LZ4F_getFrameInfo(dctx, &fi, frame, &pos));
            dPos = 0;
            while (r != 0) {
                size_t const cut = UT_rand() % 12;   /* stops within block checksums and headers */
                size_t srcLen = (UT_rand() & 1) ? 1 + UT_rand() % r : (cut < r ? r - cut : 1);
                size_t dstLen = (UT_rand() & 1) ? srcSize - dPos : UT_rand() % (srcSize - dPos + 1);
                r = LZ4F_OK(LZ4F_decompress(dctx, decoded + dPos, &dstLen, frame + pos, &srcLen, NULL));
                pos += srcLen;
                dPos += dstLen;
                ASSERT_MSG(r == ra_expectedHint(ends, nbBlocks, frameSize, pos),
                           "variant %u, pos %u : wrong hint %u", variant, (unsigned)pos, (unsigned)r);
            }
            ASSERT_MSG(pos == frameSize && dPos == srcSize && !memcmp(decoded, src, srcSize), "variant %u : decoding corrupted", variant);
        }

        /* resume from each block start */
        for (k = 0; k < nbBlocks; k++) {
            size_t const dictSize = (prefs.frameInfo.blockMode == LZ4F_blockLinked) ? MIN(dStarts[k], 64 KB) : 0;
            size_t hs = hSize, noSrc = 0, noDst = 0;
            size_t srcLen = frameSize - ends[k], dstLen = srcSize - dStarts[k];
            LZ4F_decompressOptions_t dOpt;
            memset(&dOpt, 0, sizeof(dOpt));
            dOpt.skipChecksums = 1;
            LZ4F_resetDecompressionContext(dctx);
            r = LZ4F_getFrameInfo(dctx, &fi, frame, &hs);
            ASSERT_MSG(!LZ4F_isError(r) && hs == hSize, "variant %u, block %u : %s", variant, (unsigned)k,
                       LZ4F_isError(r) ? LZ4F_getErrorName(r) : "wrong header");
            LZ4F_OK(LZ4F_decompress_usingDict(dctx, NULL, &noDst, NULL, &noSrc, src + dStarts[k] - dictSize, dictSize, &dOpt));
            /* skipChecksums remains active up to the end of frame */
            r = LZ4F_decompress(dctx, decoded, &dstLen, frame + ends[k], &srcLen, NULL);
            ASSERT_MSG(r == 0, "variant %u, block %u : %s", variant, (unsigned)k,
                       LZ4F_isError(r) ? LZ4F_getErrorName(r) : "frame not completed");
            ASSERT_MSG(srcLen == frameSize - ends[k] && dstLen == srcSize - dStarts[k]
                       && !memcmp(decoded, src + dStarts[k], dstLen),
                       "variant %u, block %u : resumed decoding is corrupted", variant, (unsigned)k);
        }
    }
}

/* ra_compressFrame() :
 * compresses @src into a frame, flushing after @firstSize bytes, so that the 2nd block starts there.
 * @return : frame size, or an error code */
static size_t ra_compressFrame(LZ4F_cctx* cctx, BYTE* dst, size_t dstCapacity,
                               const BYTE* src, size_t srcSize, size_t firstSize,
                               const LZ4F_CDict* cdict, const LZ4F_preferences_t* prefs)
{
    size_t pos, r;
    pos = LZ4F_compressBegin_usingCDict(cctx, dst, dstCapacity, cdict, prefs);
    if (LZ4F_isError(pos)) return pos;
    r = LZ4F_compressUpdate(cctx, dst + pos, dstCapacity - pos, src, firstSize, NULL);
    if (LZ4F_isError(r)) return r;
    pos += r;
    r = LZ4F_flush(cctx, dst + pos, dstCapacity - pos, NULL);
    if (LZ4F_isError(r)) return r;
    pos += r;
    r = LZ4F_compressUpdate(cctx, dst + pos, dstCapacity - pos, src + firstSize, srcSize - firstSize, NULL);
    if (LZ4F_isError(r)) return r;
    pos += r;
    r = LZ4F_compressEnd(cctx, dst + pos, dstCapacity - pos, NULL);
    if (LZ4F_isError(r)) return r;
    return pos + r;
}

/* ra_blockDict() :
 * determines the data a block starting at position @pos within @src may refer to, as expected by LZ4F_resumeFrame() :
 * for linked blocks, the last (up to) 64 KB of @dict followed by @src[0, @pos[, copied into @hist ;
 * for independent blocks, @dict.
 * @dict : the dictionary used to compress the frame, or NULL.
 * @return : its size, and sets *dictPtr to its start */
static size_t ra_blockDict(const void** dictPtr, BYTE* hist, LZ4F_blockMode_t blockMode,
                           const BYTE* dict, size_t dictSize, const BYTE* src, size_t pos)
{
    if (blockMode == LZ4F_blockIndependent) {
        *dictPtr = dict;
        return dictSize;
    }
    {   size_t const fromSrc = MIN(pos, 64 KB);
        size_t const fromDict = MIN(dictSize, 64 KB - fromSrc);
        if (fromDict) memcpy(hist, dict + dictSize - fromDict, fromDict);
        if (fromSrc) memcpy(hist + fromDict, src + pos - fromSrc, fromSrc);
        *dictPtr = hist;
        return fromDict + fromSrc;
    }
}

TEST(resumeFrame)
{
    size_t const blockSize = 64 KB;
    size_t const firstSize = 10 KB;   /* 2nd block starts there : its history is partly within the dictionary */
    size_t const srcSize = firstSize + 4 * blockSize + 1000;   /* last block is partial */
    size_t const rawBlock = 3;        /* stored uncompressed */
    size_t const dictSize = 32 KB;
    const BYTE* const dict = (const BYTE*)UT_sample(dictSize);
    BYTE* const src = (BYTE*)UT_sample(srcSize);
    BYTE* const decoded = (BYTE*)UT_malloc(srcSize);
    BYTE* const hist = (BYTE*)UT_malloc(64 KB);
    size_t frameCapacity, bStart;
    BYTE* frame;
    LZ4F_CDict* const cdict = newCDict(dict, dictSize);
    LZ4F_cctx* const cctx = newCctx();
    LZ4F_dctx* const dctx = newDctx();
    unsigned variant;

    {   LZ4F_preferences_t worstPrefs = LZ4F_INIT_PREFERENCES;
        worstPrefs.frameInfo.blockSizeID = LZ4F_max64KB;
        worstPrefs.frameInfo.blockChecksumFlag = LZ4F_blockChecksumEnabled;
        worstPrefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        worstPrefs.frameInfo.contentSize = srcSize;
        frameCapacity = 2 * LZ4F_compressFrameBound(srcSize, &worstPrefs);   /* margin for the flushed block */
    }
    frame = (BYTE*)UT_malloc(frameCapacity);
    UT_fillNoise(src + firstSize + (rawBlock-1) * blockSize, blockSize);
    /* blocks start with content from the dictionary, so that they depend on it */
    for (bStart = 0; bStart < srcSize; bStart = bStart ? bStart + blockSize : firstSize) {
        if (bStart == firstSize + (rawBlock-1) * blockSize) continue;
        memcpy(src + bStart, dict + dictSize - 2 KB, MIN(1 KB, srcSize - bStart));
    }

    for (variant = 0; variant < 32; variant++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        unsigned const withDict = (variant >> 4) & 1;
        const BYTE* const fDict = withDict ? dict : NULL;
        size_t const fDictSize = withDict ? dictSize : 0;
        LZ4F_frameInfo_t fi;
        size_t ends[SD_ELTS_MAX] = { 0 }, dStarts[SD_ELTS_MAX] = { 0 };
        size_t frameSize, nbElts, nbBlocks, k, r;
        prefs.frameInfo.blockMode = (LZ4F_blockMode_t)(variant & 1);
        prefs.frameInfo.blockSizeID = LZ4F_max64KB;
        prefs.frameInfo.blockChecksumFlag = (LZ4F_blockChecksum_t)((variant >> 1) & 1);
        prefs.frameInfo.contentChecksumFlag = (LZ4F_contentChecksum_t)((variant >> 2) & 1);
        prefs.frameInfo.contentSize = ((variant >> 3) & 1) ? srcSize : 0;
        frameSize = LZ4F_OK(ra_compressFrame(cctx, frame, frameCapacity, src, srcSize, firstSize, withDict ? cdict : NULL, &prefs));
        nbElts = frameElementEnds(ends, SD_ELTS_MAX, frame, frameSize);
        ASSERT_MSG(nbElts > 0, "variant %u : malformed frame", variant);
        nbBlocks = nbElts - 2 - prefs.frameInfo.contentChecksumFlag;   /* minus header, endMark, and content checksum */
        ASSERT_MSG(nbBlocks > rawBlock && (readLE32(frame + ends[rawBlock]) & 0x80000000U),
                   "variant %u : block %u should be stored uncompressed", variant, (unsigned)rawBlock);

        /* first pass : records block starts */
        ra_indexFrame(dctx, &fi, frame, frameSize, ends, nbBlocks, fDict, fDictSize, decoded, srcSize, dStarts);
        ASSERT_MSG(!memcmp(decoded, src, srcSize), "variant %u : first pass corrupted", variant);
        ASSERT_MSG(dStarts[1] == firstSize, "variant %u : 2nd block should start at %u", variant, (unsigned)firstSize);

        /* resume from each block start */
        for (k = 0; k < nbBlocks; k++) {
            const void* bDict;
            size_t const bDictSize = ra_blockDict(&bDict, hist, prefs.frameInfo.blockMode, fDict, fDictSize, src, dStarts[k]);
            size_t srcLen = frameSize - ends[k], dstLen = srcSize - dStarts[k];
            LZ4F_OK(LZ4F_resumeFrame(dctx, &fi, dStarts[k], bDict, bDictSize));
            r = LZ4F_decompress(dctx, decoded, &dstLen, frame + ends[k], &srcLen, NULL);
            ASSERT_MSG(r == 0, "variant %u, block %u : %s", variant, (unsigned)k,
                       LZ4F_isError(r) ? LZ4F_getErrorName(r) : "frame not completed");
            ASSERT_MSG(srcLen == frameSize - ends[k] && dstLen == srcSize - dStarts[k]
                       && !memcmp(decoded, src + dStarts[k], dstLen),
                       "variant %u, block %u : resumed decoding is corrupted", variant, (unsigned)k);
        }

        /* skipping the content checksum doesn't extend to next frame */
        if (prefs.frameInfo.contentChecksumFlag) {
            size_t srcLen = frameSize, dstLen = srcSize;
            frame[frameSize - 1] ^= 1;
            r = LZ4F_decompress_usingDict(dctx, decoded, &dstLen, frame, &srcLen, fDict, fDictSize, NULL);
            frame[frameSize - 1] ^= 1;
            ASSERT_MSG(LZ4F_getErrorCode(r) == LZ4F_ERROR_contentChecksum_invalid, "variant %u : %s", variant,
                       LZ4F_isError(r) ? LZ4F_getErrorName(r) : "corrupted content checksum not detected");
        }

        /* block checksums remain verified (including uncompressed blocks), and content size too */
        {   const void* bDict;
            size_t const bDictSize = ra_blockDict(&bDict, hist, prefs.frameInfo.blockMode, fDict, fDictSize, src, dStarts[rawBlock]);
            size_t srcLen = frameSize - ends[rawBlock], dstLen = srcSize;
            if (prefs.frameInfo.blockChecksumFlag) {
                LZ4F_OK(LZ4F_resumeFrame(dctx, &fi, dStarts[rawBlock], bDict, bDictSize));
                frame[ends[rawBlock+1] - 1] ^= 1;
                r = LZ4F_decompress(dctx, decoded, &dstLen, frame + ends[rawBlock], &srcLen, NULL);
                frame[ends[rawBlock+1] - 1] ^= 1;
                ASSERT_MSG(LZ4F_getErrorCode(r) == LZ4F_ERROR_blockChecksum_invalid, "variant %u : %s", variant,
                           LZ4F_isError(r) ? LZ4F_getErrorName(r) : "corrupted block checksum not detected");
            }
            if (prefs.frameInfo.contentSize) {
                srcLen = frameSize - ends[rawBlock];
                dstLen = srcSize;
                LZ4F_OK(LZ4F_resumeFrame(dctx, &fi, dStarts[rawBlock] + 1, bDict, bDictSize));
                r = LZ4F_decompress(dctx, decoded, &dstLen, frame + ends[rawBlock], &srcLen, NULL);
                ASSERT_MSG(LZ4F_getErrorCode(r) == LZ4F_ERROR_frameSize_wrong, "variant %u : %s", variant,
                           LZ4F_isError(r) ? LZ4F_getErrorName(r) : "wrong decodedPos not detected");
                r = LZ4F_resumeFrame(dctx, &fi, srcSize + 1, NULL, 0);
                ASSERT_MSG(LZ4F_getErrorCode(r) == LZ4F_ERROR_frameSize_wrong, "variant %u : %s", variant,
                           LZ4F_isError(r) ? LZ4F_getErrorName(r) : "decodedPos beyond content size accepted");
        }   }
    }

    /* invalid parameters */
    {   LZ4F_frameInfo_t fi;
        memset(&fi, 0, sizeof(fi));
        fi.blockSizeID = LZ4F_max64KB;
        LZ4F_OK(LZ4F_resumeFrame(dctx, &fi, 0, NULL, 0));   /* valid frameInfo */
        ASSERT_LZ4F_ERROR(LZ4F_resumeFrame(dctx, NULL, 0, NULL, 0), LZ4F_ERROR_parameter_null);   /* NULL frameInfo */
        ASSERT_LZ4F_ERROR(LZ4F_resumeFrame(dctx, &fi, 0, NULL, 1), LZ4F_ERROR_parameter_null);    /* NULL dict */
        fi.frameType = LZ4F_skippableFrame;
        ASSERT_LZ4F_ERROR(LZ4F_resumeFrame(dctx, &fi, 0, NULL, 0), LZ4F_ERROR_frameType_unknown);
        fi.frameType = LZ4F_frame;
        fi.blockSizeID = LZ4F_default;
        ASSERT_LZ4F_ERROR(LZ4F_resumeFrame(dctx, &fi, 0, NULL, 0), LZ4F_ERROR_maxBlockSize_invalid);   /* undefined block size */
        fi.blockSizeID = LZ4F_max64KB;
        fi.blockMode = (LZ4F_blockMode_t)2;
        ASSERT_LZ4F_ERROR(LZ4F_resumeFrame(dctx, &fi, 0, NULL, 0), LZ4F_ERROR_blockMode_invalid);
    }
}


/*-************************************
*  LZ4F_seek() : random reads within a file, compared to its decoded content.
*  The file starts after unrelated data, and contains frames using linked or independent blocks,
*  checksums, content size, a skippable frame, and an empty frame.
**************************************/
static void closeReadFile(void* rf) { LZ4F_readClose((LZ4_readFile_t*)rf); }

/* @return : a read handle for @file, closed at the end of the test */
static LZ4_readFile_t* newReadFile(FILE* file)
{
    LZ4_readFile_t* rf = NULL;
    LZ4F_OK(LZ4F_readOpen(&rf, file));
    return (LZ4_readFile_t*)UT_defer(closeReadFile, rf);
}

/* fs_writeFrame() : compresses @src into a frame, appended to @f */
static void fs_writeFrame(FILE* f, const BYTE* src, size_t srcSize, const LZ4F_preferences_t* prefs)
{
    size_t const capacity = LZ4F_compressFrameBound(srcSize, prefs);
    BYTE* const frame = (BYTE*)UT_malloc(capacity);
    size_t const frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, srcSize, prefs));
    ASSERT_EQ(fwrite(frame, 1, frameSize, f), frameSize);
    UT_release(frame);
}

TEST(lz4file_seek)
{
    /* a skippable frame follows the 1st one.
     * the last frame uses linked blocks and is larger than LZ4F_RESUME_SPAN (1 MB) :
     * a resume point is recorded at its end, which is the end of file */
    static const size_t sizes[] = { 1200 KB, 1000, 0, 2500 KB, 1200 KB };
    size_t const prefixSize = 100;   /* unrelated data before the 1st frame */
    size_t const bufSize = 200 KB;
    size_t totalSize = 0, n, srcPos, pos;
    BYTE* ref;
    BYTE* const buf = (BYTE*)UT_malloc(bufSize);
    FILE* const f = newTmpFile();
    LZ4_readFile_t* rf;
    unsigned op;

    for (n = 0; n < ARRAY_SIZE(sizes); n++) totalSize += sizes[n];
    ref = (BYTE*)UT_sample(totalSize);
    srcPos = sizes[0] + sizes[1] + sizes[2];
    UT_fillNoise(ref + srcPos + 64 KB, 64 KB);   /* uncompressed block in the 4th frame */

    UT_fillNoise(buf, prefixSize);
    ASSERT_EQ(fwrite(buf, 1, prefixSize, f), prefixSize);
    for (srcPos = 0, n = 0; n < ARRAY_SIZE(sizes); srcPos += sizes[n], n++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        prefs.frameInfo.blockSizeID = LZ4F_max64KB;
        prefs.frameInfo.blockMode = (n == 0 || n == 4) ? LZ4F_blockLinked : LZ4F_blockIndependent;
        if (n == 0 || n == 3) prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        if (n == 3) {
            prefs.frameInfo.blockChecksumFlag = LZ4F_blockChecksumEnabled;
            prefs.frameInfo.contentSize = sizes[n];
        }
        fs_writeFrame(f, ref + srcPos, sizes[n], &prefs);
        if (n == 0) {
            BYTE skippable[8 + 100];
            writeLE32(skippable, LZ4F_MAGIC_SKIPPABLE_START + 5);
            writeLE32(skippable + 4, 100);
            memset(skippable + 8, 0x5A, 100);
            ASSERT_EQ(fwrite(skippable, 1, sizeof(skippable), f), sizeof(skippable));
    }   }
    ASSERT_EQ(fseek(f, (long)prefixSize, SEEK_SET), 0);
    rf = newReadFile(f);

    /* random seeks and reads */
    for (pos = 0, op = 0; op < 400; op++) {
        unsigned const kind = UT_rand() % 4;
        if (kind == 0) {   /* absolute */
            size_t const target = UT_rand() % (totalSize + 1);
            size_t const r = LZ4F_seek(rf, (long long)target, SEEK_SET);
            ASSERT_MSG(!LZ4F_isError(r), "op %u : seek to %u : %s", op, (unsigned)target, LZ4F_getErrorName(r));
            pos = target;
        } else if (kind == 1) {   /* relative, nearby */
            long long const delta = (long long)(UT_rand() % (400 KB)) - 200 KB;
            long long const target = MIN(MAX((long long)pos + delta, 0), (long long)totalSize);
            size_t const r = LZ4F_seek(rf, target - (long long)pos, SEEK_CUR);
            ASSERT_MSG(!LZ4F_isError(r), "op %u : seek to %u : %s", op, (unsigned)target, LZ4F_getErrorName(r));
            pos = (size_t)target;
        } else {   /* read, large or small */
            size_t const size = 1 + UT_rand() % ((kind == 2) ? bufSize : 100);
            size_t const r = LZ4F_read(rf, buf, size);
            ASSERT_MSG(r == MIN(size, totalSize - pos), "op %u : read %u bytes at %u : %s", op, (unsigned)size, (unsigned)pos,
                       LZ4F_isError(r) ? LZ4F_getErrorName(r) : "wrong size");
            ASSERT_MSG(!memcmp(buf, ref + pos, r), "op %u : wrong data at %u", op, (unsigned)pos);
            pos += r;
        }
        ASSERT_MSG(LZ4F_tell(rf) == pos, "op %u : LZ4F_tell() = %u instead of %u", op, (unsigned)LZ4F_tell(rf), (unsigned)pos);
    }

    /* invalid seeks don't move */
    ASSERT_LZ4F_ERROR(LZ4F_seek(NULL, 0, SEEK_SET), LZ4F_ERROR_parameter_null);
    ASSERT_LZ4F_ERROR(LZ4F_seek(rf, 0, SEEK_END), LZ4F_ERROR_parameter_invalid);
    ASSERT_LZ4F_ERROR(LZ4F_seek(rf, -1, SEEK_SET), LZ4F_ERROR_parameter_invalid);   /* negative position */
    ASSERT_LZ4F_ERROR(LZ4F_seek(rf, -(long long)pos - 1, SEEK_CUR), LZ4F_ERROR_parameter_invalid);   /* before start */
    ASSERT_EQ(LZ4F_tell(rf), pos);

    /* end of data */
    LZ4F_OK(LZ4F_seek(rf, (long long)totalSize, SEEK_SET));
    ASSERT_EQ(LZ4F_read(rf, buf, 1), 0);
    ASSERT_LZ4F_ERROR(LZ4F_seek(rf, (long long)totalSize + 1, SEEK_SET), LZ4F_ERROR_parameter_invalid);
    ASSERT_EQ(LZ4F_tell(rf), totalSize);   /* a seek beyond end stops at end */

    /* full sequential read, from start */
    LZ4F_OK(LZ4F_seek(rf, 0, SEEK_SET));
    for (pos = 0; pos < totalSize; ) {
        size_t const r = LZ4F_read(rf, buf, bufSize);
        ASSERT_MSG(!LZ4F_isError(r) && r == MIN(bufSize, totalSize - pos) && !memcmp(buf, ref + pos, r),
                   "sequential read : wrong data at %u", (unsigned)pos);
        pos += r;
    }
    /* end of file is reached cleanly, also when restarting from its resume point */
    ASSERT_EQ(LZ4F_read(rf, buf, 1), 0);
    LZ4F_OK(LZ4F_seek(rf, 0, SEEK_SET));
    ASSERT_MSG(LZ4F_read(rf, buf, 1) == 1 && buf[0] == ref[0], "read at start failed");
    LZ4F_OK(LZ4F_seek(rf, (long long)totalSize, SEEK_SET));   /* indexed end */
    ASSERT_EQ(LZ4F_read(rf, buf, 1), 0);   /* not reported as truncated after restart */
}

/* LZ4F_read() : complete files decode fully, then report end of file.
 * Files ending within a frame report ERROR_io_read. */
TEST(lz4file_truncated)
{
    static const struct {
        size_t srcSize;
        size_t readSize;
        size_t cut;        /* bytes removed from the end of the last frame */
        int headerOnly;    /* last frame reduced to its header */
    } cases[] = {
        {   0,   1, 0, 0 },   /* empty frame */
        { 256,   1, 0, 0 },   /* complete, read by small, exact and large chunks */
        { 256, 256, 0, 0 },
        { 256, 257, 0, 0 },
        { 256,   1, 1, 0 },   /* truncated end mark, or checksum */
        { 256, 257, 1, 0 },
        { 256, 257, 4, 0 },
        { 256, 257, 8, 0 },   /* truncated block, without checksums */
        { 256, 257, 0, 1 }    /* header only */
    };
    BYTE src[256];
    BYTE frame[1 KB];
    BYTE decoded[1 KB];
    unsigned checksums, nbFrames, k;
    size_t n;

    UT_fillSample(src, sizeof(src));
    for (checksums = 0; checksums < 4; checksums++)
    for (nbFrames = 1; nbFrames <= 2; nbFrames++)
    for (n = 0; n < ARRAY_SIZE(cases); n++) {
        size_t const expected = cases[n].srcSize * nbFrames;
        int const truncated = cases[n].cut || cases[n].headerOnly;
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        size_t frameSize, lastSize, total = 0, r;
        FILE* f;
        LZ4_readFile_t* rf;
        prefs.frameInfo.contentChecksumFlag = (LZ4F_contentChecksum_t)(checksums & 1);
        prefs.frameInfo.blockChecksumFlag = (LZ4F_blockChecksum_t)(checksums >> 1);
        if (cases[n].headerOnly) prefs.frameInfo.contentSize = cases[n].srcSize;   /* 15-byte header : LZ4F_readOpen() needs 11 bytes */
        frameSize = LZ4F_OK(LZ4F_compressFrame(frame, sizeof(frame), src, cases[n].srcSize, &prefs));
        lastSize = cases[n].headerOnly ? LZ4F_headerSize(frame, frameSize) : frameSize - cases[n].cut;

        f = newTmpFile();
        for (k = 1; k < nbFrames; k++) ASSERT_EQ(fwrite(frame, 1, frameSize, f), frameSize);
        ASSERT_EQ(fwrite(frame, 1, lastSize, f), lastSize);
        rewind(f);
        rf = newReadFile(f);
        do {
            r = LZ4F_read(rf, decoded + total, cases[n].readSize);
            if (LZ4F_isError(r)) break;
            total += r;
        } while (r != 0 && total <= expected);
        if (truncated) {
            ASSERT_MSG(LZ4F_getErrorCode(r) == LZ4F_ERROR_io_read,
                       "case %u, %u frame(s), checksums %u : truncation not detected", (unsigned)n, nbFrames, checksums);
        } else {
            ASSERT_MSG(!LZ4F_isError(r) && total == expected,
                       "case %u, %u frame(s), checksums %u : decoded %u bytes", (unsigned)n, nbFrames, checksums, (unsigned)total);
            for (k = 0; k < nbFrames; k++)
                ASSERT_MSG(!memcmp(decoded + k * cases[n].srcSize, src, cases[n].srcSize), "case %u : wrong data", (unsigned)n);
            ASSERT_MSG(LZ4F_read(rf, decoded, 1) == 0, "case %u : data beyond end", (unsigned)n);
        }
        UT_release(rf);
        UT_release(f);
    }
}
