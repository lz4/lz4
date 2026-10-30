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
#ifndef LZ4F_DISABLE_DEPRECATE_WARNINGS
#  define LZ4F_DISABLE_DEPRECATE_WARNINGS   /* LZ4F_uncompressedUpdate */
#endif
#include "lz4frame_helpers.h"   /* lz4frame.h, LZ4F_OK, newCctx, assertDecodesTo */
#include <stdlib.h>     /* malloc, calloc, free */
#include <stdio.h>      /* FILE, fopen */
#include <string.h>     /* memcpy, memset */
#include <limits.h>     /* INT_MIN */
#include <time.h>       /* clock */
#if defined(_POSIX_C_SOURCE) && (_POSIX_C_SOURCE >= 200112L) && !defined(_WIN32)   /* glibc defines it, except in strict ISO C mode */
#  include <unistd.h>   /* pipe, write, close */
#  define PIPE_AVAILABLE 1
#endif
#include "lz4file.h"
#define LZ4_STATIC_LINKING_ONLY  /* LZ4_ACCELERATION_MAX */
#include "lz4.h"

/*-************************************
*  Helpers
**************************************/
typedef unsigned char BYTE;

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

/* LZ4F_compressBound() without preferences covers the worst case, including block and content checksums
 * (unlike LZ4F_compressFrameBound(), which then assumes default preferences) */
TEST(compressBound_nullPreferences)
{
    static const size_t srcSizes[] = { 0, 1, 64 KB + 1, 1 MB };
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t i;
    prefs.frameInfo.blockChecksumFlag = LZ4F_blockChecksumEnabled;
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    for (i = 0; i < ARRAY_SIZE(srcSizes); i++)
        ASSERT_MSG(LZ4F_compressBound(srcSizes[i], NULL) >= LZ4F_compressBound(srcSizes[i], &prefs), "srcSize %u", (unsigned)srcSizes[i]);
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
    static const unsigned dictIDs[] = { 1, 0x99, 0xFFFFFFFFU };
    size_t i;
    for (i = 0; i < ARRAY_SIZE(dictIDs); i++) {
        char header[LZ4F_HEADER_SIZE_MAX];
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        LZ4F_frameInfo_t frameInfo;
        size_t headerSize;
        prefs.frameInfo.dictID = dictIDs[i];
        headerSize = LZ4F_OK(LZ4F_compressBegin(newCctx(), header, sizeof(header), &prefs));
        LZ4F_OK(LZ4F_getFrameInfo(newDctx(), &frameInfo, header, &headerSize));
        ASSERT_EQ(frameInfo.dictID, dictIDs[i]);
    }
}

/* reference frame : the header checksum is the second byte of XXH32(descriptor), as specified,
 * here computed independently of the library */
TEST(compressFrame_referenceEmptyFrame)
{
    static const BYTE expected[] = { 0x04, 0x22, 0x4D, 0x18,   /* magic number */
                                     0x60, 0x40, 0x82,         /* FLG (version 1, independent blocks), BD (64 KB), header checksum */
                                     0x00, 0x00, 0x00, 0x00 }; /* endMark */
    size_t const capacity = LZ4F_compressFrameBound(0, NULL);
    BYTE* const frame = (BYTE*)UT_malloc(capacity);
    ASSERT_EQ(LZ4F_OK(LZ4F_compressFrame(frame, capacity, NULL, 0, NULL)), sizeof(expected));
    ASSERT_MEMEQ(frame, expected, sizeof(expected));
}

/* the content size is a 64-bit little-endian field, written and read back entirely */
TEST(frameHeader_contentSize64)
{
    static const BYTE expected[] = { 0x04, 0x22, 0x4D, 0x18,
                                     0x48, 0x40,   /* FLG (version 1, linked blocks, content size), BD (64 KB) */
                                     0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01,
                                     0xA4 };       /* header checksum */
    unsigned long long const contentSize = ((unsigned long long)0x01234567 << 32) | 0x89ABCDEF;
    BYTE header[LZ4F_HEADER_SIZE_MAX];
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    LZ4F_frameInfo_t frameInfo;
    size_t headerSize;
    prefs.frameInfo.blockSizeID = LZ4F_max64KB;
    prefs.frameInfo.contentSize = contentSize;
    headerSize = LZ4F_OK(LZ4F_compressBegin(newCctx(), header, sizeof(header), &prefs));
    ASSERT_EQ(headerSize, sizeof(expected));
    ASSERT_MEMEQ(header, expected, sizeof(expected));
    LZ4F_OK(LZ4F_getFrameInfo(newDctx(), &frameInfo, header, &headerSize));
    ASSERT_EQ(frameInfo.contentSize, contentSize);
}

/* LZ4F_headerSize() : skippable frames have an 8-byte header ; errors */
TEST(headerSize_errors)
{
    BYTE header[LZ4F_HEADER_SIZE_MAX];
    size_t const headerSize = LZ4F_OK(LZ4F_compressBegin(newCctx(), header, sizeof(header), NULL));
    ASSERT_EQ(LZ4F_OK(LZ4F_headerSize(header, headerSize)), headerSize);
    ASSERT_LZ4F_ERROR(LZ4F_headerSize(NULL, headerSize), LZ4F_ERROR_srcPtr_wrong);
    ASSERT_LZ4F_ERROR(LZ4F_headerSize(header, LZ4F_MIN_SIZE_TO_KNOW_HEADER_LENGTH - 1), LZ4F_ERROR_frameHeader_incomplete);
    header[0] ^= 1;   /* unknown magic number */
    ASSERT_LZ4F_ERROR(LZ4F_headerSize(header, headerSize), LZ4F_ERROR_frameType_unknown);
    writeLE32(header, LZ4F_MAGIC_SKIPPABLE_START + 5);
    ASSERT_EQ(LZ4F_OK(LZ4F_headerSize(header, LZ4F_MIN_SIZE_TO_KNOW_HEADER_LENGTH)), 8);
}

/* the decoder rejects an unknown magic number, a reserved block size ID, and a wrong header checksum */
static void assertRejectsHeader(const BYTE* frame, size_t frameSize, size_t pos, BYTE value, LZ4F_errorCodes error)
{
    BYTE* const corrupted = (BYTE*)UT_malloc(frameSize);
    char decoded[64];
    size_t dSize = sizeof(decoded), cSize = frameSize;
    memcpy(corrupted, frame, frameSize);
    corrupted[pos] = value;
    ASSERT_LZ4F_ERROR(LZ4F_decompress(newDctx(), decoded, &dSize, corrupted, &cSize, NULL), error);
    UT_release(corrupted);
}

TEST(decompress_invalidHeader)
{
    size_t const capacity = LZ4F_compressFrameBound(64, NULL);
    BYTE* const frame = (BYTE*)UT_malloc(capacity);
    size_t const frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, UT_sample(64), 64, NULL));
    ASSERT_EQ(frame[5], 0x40);   /* BD : 64 KB blocks, header checksum in frame[6] */
    assertRejectsHeader(frame, frameSize, 0, (BYTE)(frame[0] ^ 1), LZ4F_ERROR_frameType_unknown);
    assertRejectsHeader(frame, frameSize, 5, 3 << 4, LZ4F_ERROR_maxBlockSize_invalid);   /* checked before the header checksum */
    assertRejectsHeader(frame, frameSize, 5, 0, LZ4F_ERROR_maxBlockSize_invalid);
    assertRejectsHeader(frame, frameSize, 6, (BYTE)(frame[6] ^ 1), LZ4F_ERROR_headerChecksum_invalid);
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

/* fast levels : acceleration is 1 for levels 0 and 1, and (-level + 1) for negative levels :
 * a frame made of a single block contains the same block as LZ4_compress_fast() */
TEST(compressFrame_levelsAsAcceleration)
{
    static const int levels[] = { 1, 0, -1, -4 };
    size_t const srcSize = 32 KB;
    const char* const src = (const char*)UT_sample(srcSize);
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    BYTE* const frame = (BYTE*)UT_malloc(capacity);
    char* const block = (char*)UT_malloc((size_t)LZ4_compressBound((int)srcSize));
    size_t i;
    for (i = 0; i < ARRAY_SIZE(levels); i++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        int const acceleration = (levels[i] >= 0) ? 1 : 1 - levels[i];
        int const blockSize = LZ4_compress_fast(src, block, (int)srcSize, LZ4_compressBound((int)srcSize), acceleration);
        prefs.compressionLevel = levels[i];
        LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, srcSize, &prefs));
        ASSERT_MSG(readLE32(frame + 7) == (unsigned)blockSize, "level %i : block of %u bytes, instead of %i",
                   levels[i], readLE32(frame + 7) & 0x7FFFFFFFU, blockSize);   /* header : 7 bytes */
        ASSERT_MEMEQ(frame + 11, block, (size_t)blockSize);
    }
}

/* LZ4F_compressFrame() selects the smallest block size holding the input, up to the requested one,
 * and independent blocks for a single block */
TEST(compressFrame_blockSize)
{
    static const struct { size_t srcSize; LZ4F_blockSizeID_t requested, expected; } cases[] = {
        { 100,       LZ4F_default,  LZ4F_max64KB },
        { 64 KB,     LZ4F_max4MB,   LZ4F_max64KB },
        { 64 KB + 1, LZ4F_max4MB,   LZ4F_max256KB },
        { 256 KB,    LZ4F_max4MB,   LZ4F_max256KB },
        { 1 MB,      LZ4F_max4MB,   LZ4F_max1MB },
        { 1 MB + 1,  LZ4F_max4MB,   LZ4F_max4MB },
        { 5 MB,      LZ4F_max4MB,   LZ4F_max4MB },
        { 5 MB,      LZ4F_max256KB, LZ4F_max256KB } };
    size_t const srcMax = 5 MB;
    const void* const src = UT_sample(srcMax);
    size_t const capacity = LZ4F_compressFrameBound(srcMax, NULL);
    void* const frame = UT_malloc(capacity);
    size_t i;
    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;   /* linked blocks */
        LZ4F_frameInfo_t frameInfo;
        size_t size;
        LZ4F_blockMode_t expectedMode;
        prefs.frameInfo.blockSizeID = cases[i].requested;
        size = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, cases[i].srcSize, &prefs));
        LZ4F_OK(LZ4F_getFrameInfo(newDctx(), &frameInfo, frame, &size));
        expectedMode = (cases[i].srcSize <= LZ4F_getBlockSize(cases[i].expected)) ? LZ4F_blockIndependent : LZ4F_blockLinked;
        ASSERT_MSG(frameInfo.blockSizeID == cases[i].expected && frameInfo.blockMode == expectedMode,
                   "%u bytes : blockSizeID %i (expected %i), blockMode %i", (unsigned)cases[i].srcSize,
                   (int)frameInfo.blockSizeID, (int)cases[i].expected, (int)frameInfo.blockMode);
    }
}

/* a frame filling dstCapacity exactly is completed : the context waits for a new frame */
TEST(compressFrame_exactCapacity)
{
    size_t const srcSize = 10 KB;
    const void* const src = UT_sample(srcSize);
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    char* const frame = (char*)UT_malloc(capacity);
    LZ4F_cctx* const cctx = newCctx();
    size_t const frameSize = LZ4F_OK(LZ4F_compressFrame_usingCDict(cctx, frame, capacity, src, srcSize, NULL, NULL));
    ASSERT_EQ(LZ4F_OK(LZ4F_compressFrame_usingCDict(cctx, frame, frameSize, src, srcSize, NULL, NULL)), frameSize);
    ASSERT_LZ4F_ERROR(LZ4F_compressUpdate(cctx, frame, capacity, src, srcSize, NULL), LZ4F_ERROR_compressionState_uninitialized);
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

/* linked blocks reference the dictionary, at fast and HC levels.
 * The input starts with the end of the dictionary (noise) : the first block can't be decoded without the dictionary.
 * (A size comparison with and without dictionary depends on the data at fast levels : the dictionary enlarges indexes,
 *  hence halves the hash table of 64 KB blocks) */
TEST(compressBegin_usingDict)
{
    static const int levels[] = { 0, 9 };
    size_t const dictSize = 7 KB;   /* small enough for LZ4_MEMORY_USAGE == 10 */
    size_t const srcSize = 66 KB;   /* must be > 64 KB to avoid short-size optimizations */
    size_t const tailSize = 512;    /* most recent positions : kept by small hash tables (LZ4_MEMORY_USAGE == 10) */
    const char* const dict = (const char*)UT_noise(dictSize);
    char* const src = (char*)UT_sample(srcSize);
    char* const decoded = (char*)UT_malloc(64 KB);
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    char* const frame = (char*)UT_malloc(capacity);
    size_t li;
    memcpy(src, dict + dictSize - tailSize, tailSize);
    for (li = 0; li < ARRAY_SIZE(levels); li++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;   /* 64 KB linked blocks */
        size_t frameSize;
        unsigned blockHeader;
        prefs.compressionLevel = levels[li];
        frameSize = LZ4F_OK(streamFrame(newCctx(), frame, capacity, src, srcSize, dict, dictSize, NULL, &prefs));
        assertDecodesTo(frame, frameSize, dict, dictSize, src, srcSize);
        blockHeader = readLE32(frame + 7);   /* frame header : no content size, no dictID */
        ASSERT_MSG(LZ4_decompress_safe(frame + 7 + 4, decoded, (int)(blockHeader & 0x7FFFFFFFU), 64 KB) < 0,
                   "level %i : the first block decodes without dictionary", levels[li]);
    }
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
    const void* const src = UT_fixedSample(srcSize);   /* the dictionary must be used */
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

/* LZ4F_compressBegin_usingDict() : with HC levels >= 3, same frame as with a CDict of this dictionary,
 * with independent blocks, or nbWorkers >= 1. Fast levels digest the dictionary faster than a CDict,
 * level 2 in its own format (a CDict is digested at default level) */
TEST(compressBegin_usingDict_sameAsCDict_HC)
{
    static const int levels[] = { 3, 9 };
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
    char dst[16];
    size_t dSize = 0, cSize = 0;
    ASSERT(LZ4F_OK(LZ4F_decompress(newDctx(), NULL, &dSize, src, &cSize, NULL)) > 0);
    dSize = sizeof(dst);   /* nothing produced, nothing consumed */
    ASSERT(LZ4F_OK(LZ4F_decompress(newDctx(), dst, &dSize, src, &cSize, NULL)) > 0);
    ASSERT_EQ(dSize, 0);
    ASSERT_EQ(cSize, 0);
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
    ASSERT_EQ(size, 0);   /* nothing consumed */
    size = LZ4F_OK(LZ4F_headerSize(frame, LZ4F_MIN_SIZE_TO_KNOW_HEADER_LENGTH));
    LZ4F_OK(LZ4F_getFrameInfo(dctx, &frameInfo, frame, &size));
}

/* LZ4F_getFrameInfo() : arguments, header partially decoded, header already decoded */
TEST(getFrameInfo_states)
{
    size_t const srcSize = 1 KB;
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL) + 8;
    BYTE* const frame = (BYTE*)UT_malloc(capacity);
    char decoded[16];
    LZ4F_frameInfo_t frameInfo;
    LZ4F_dctx* dctx;
    size_t frameSize, size, dSize;
    prefs.frameInfo.contentSize = srcSize;
    frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, UT_sample(srcSize), srcSize, &prefs));

    size = frameSize;
    ASSERT_LZ4F_ERROR(LZ4F_getFrameInfo(newDctx(), NULL, frame, &size), LZ4F_ERROR_parameter_null);
    ASSERT_LZ4F_ERROR(LZ4F_getFrameInfo(newDctx(), &frameInfo, frame, NULL), LZ4F_ERROR_parameter_null);
    frame[0] ^= 1;
    ASSERT_LZ4F_ERROR(LZ4F_getFrameInfo(newDctx(), &frameInfo, frame, &size), LZ4F_ERROR_frameType_unknown);
    ASSERT_EQ(size, 0);
    frame[0] ^= 1;
    size = frameSize;
    frame[14] ^= 1;   /* header checksum (content size field present) */
    ASSERT_LZ4F_ERROR(LZ4F_getFrameInfo(newDctx(), &frameInfo, frame, &size), LZ4F_ERROR_headerChecksum_invalid);
    ASSERT_EQ(size, 0);
    frame[14] ^= 1;

    /* header partially decoded by LZ4F_decompress() */
    dctx = newDctx();
    size = 3;
    dSize = 0;
    LZ4F_OK(LZ4F_decompress(dctx, NULL, &dSize, frame, &size, NULL));
    ASSERT_EQ(size, 3);
    size = frameSize;
    ASSERT_LZ4F_ERROR(LZ4F_getFrameInfo(dctx, &frameInfo, frame + 3, &size), LZ4F_ERROR_frameDecoding_alreadyStarted);
    ASSERT_EQ(size, 0);

    /* header already decoded : no input read, frame information still available */
    dctx = newDctx();
    size = frameSize;
    dSize = sizeof(decoded);
    LZ4F_OK(LZ4F_decompress(dctx, decoded, &dSize, frame, &size, NULL));
    ASSERT(size < frameSize);   /* decoding in progress */
    memset(&frameInfo, 0, sizeof(frameInfo));
    size = frameSize;
    ASSERT(LZ4F_OK(LZ4F_getFrameInfo(dctx, &frameInfo, frame, &size)) > 0);
    ASSERT_EQ(size, 0);
    ASSERT_EQ(frameInfo.contentSize, srcSize);
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

/* skippable frame : LZ4F_getFrameInfo() reports its type, then its content size once known */
TEST(getFrameInfo_skippableFrame)
{
    BYTE frame[8 + 100];
    char dst[16];
    LZ4F_dctx* const dctx = newDctx();
    LZ4F_frameInfo_t frameInfo;
    size_t size = sizeof(frame), dSize = sizeof(dst);
    memset(frame, 0, sizeof(frame));
    writeLE32(frame, LZ4F_MAGIC_SKIPPABLE_START + 3);
    writeLE32(frame + 4, 100);
    LZ4F_OK(LZ4F_getFrameInfo(dctx, &frameInfo, frame, &size));
    ASSERT_EQ(frameInfo.frameType, LZ4F_skippableFrame);
    ASSERT_EQ(size, 4);   /* the size field is read by LZ4F_decompress() */
    size = 4;
    LZ4F_OK(LZ4F_decompress(dctx, dst, &dSize, frame + 4, &size, NULL));
    ASSERT_EQ(size, 4);
    size = sizeof(frame);
    LZ4F_OK(LZ4F_getFrameInfo(dctx, &frameInfo, frame, &size));
    ASSERT_EQ(size, 0);
    ASSERT_EQ(frameInfo.frameType, LZ4F_skippableFrame);
    ASSERT_EQ(frameInfo.contentSize, 100);
}

/* a skippable frame decoded in a single invocation, after a reset in the middle of a frame header */
TEST(decompress_skippableAfterReset)
{
    BYTE frame[8 + 20];
    char dst[16];
    LZ4F_dctx* const dctx = newDctx();
    size_t size = 3, dSize = sizeof(dst);
    writeLE32(frame, LZ4F_MAGICNUMBER);
    LZ4F_OK(LZ4F_decompress(dctx, dst, &dSize, frame, &size, NULL));   /* header partially gathered */
    LZ4F_resetDecompressionContext(dctx);
    memset(frame, 0, sizeof(frame));
    writeLE32(frame, LZ4F_MAGIC_SKIPPABLE_START);
    writeLE32(frame + 4, 20);
    size = sizeof(frame);   /* >= LZ4F_HEADER_SIZE_MAX : header decoded directly from input */
    dSize = sizeof(dst);
    ASSERT_EQ(LZ4F_OK(LZ4F_decompress(dctx, dst, &dSize, frame, &size, NULL)), 0);
    ASSERT_EQ(size, sizeof(frame));
    ASSERT_EQ(dSize, 0);
}

/* a frame header delivered byte after byte, with its optional fields (content size, dictID) */
TEST(decompress_headerByteAfterByte)
{
    size_t const srcSize = 1 KB;
    const void* const src = UT_sample(srcSize);
    char* const decoded = (char*)UT_malloc(srcSize);
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t capacity, frameSize;
    char* frame;
    prefs.frameInfo.contentSize = srcSize;
    prefs.frameInfo.dictID = 7;
    capacity = LZ4F_compressFrameBound(srcSize, &prefs);
    frame = (char*)UT_malloc(capacity);
    frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, srcSize, &prefs));
    ASSERT_EQ(decompressBySegments(newDctx(), decoded, srcSize, frame, frameSize, 1), srcSize);
    ASSERT_MEMEQ(decoded, src, srcSize);
}

/* decodes @frame with all remaining input available, into outputs of @sizes[0], @sizes[1], ... bytes (cycling) :
 * contiguous outputs (@reuse == 0), or a single output buffer, copied into @dst then overwritten after each invocation
 * @return : decoded size */
static size_t decompressIntoOutputs(LZ4F_dctx* dctx, BYTE* dst, size_t dstCapacity,
                                    const BYTE* frame, size_t frameSize, const void* dict, size_t dictSize,
                                    const size_t* sizes, size_t nbSizes, int reuse)
{
    size_t maxSize = 0, pos = 0, dPos = 0, r = 1, k;
    BYTE* out;
    for (k = 0; k < nbSizes; k++) maxSize = MAX(maxSize, sizes[k]);
    out = reuse ? (BYTE*)UT_malloc(maxSize) : NULL;
    for (k = 0; r != 0; k++) {
        size_t cSize = frameSize - pos;
        size_t dSize = MIN(sizes[k % nbSizes], dstCapacity - dPos);
        r = LZ4F_OK(LZ4F_decompress_usingDict(dctx, reuse ? out : dst + dPos, &dSize, frame + pos, &cSize, dict, dictSize, NULL));
        ASSERT_MSG(cSize > 0 || dSize > 0 || r == 0, "no progress at position %u", (unsigned)pos);
        if (reuse) {
            memcpy(dst + dPos, out, dSize);
            memset(out, 0xA5, maxSize);   /* previous output is no longer available */
        }
        pos += cSize;
        dPos += dSize;
    }
    ASSERT_EQ(pos, frameSize);
    if (reuse) UT_release(out);
    return dPos;
}

static size_t decompressIntoChunks(LZ4F_dctx* dctx, BYTE* dst, size_t dstCapacity,
                                   const BYTE* frame, size_t frameSize,
                                   const void* dict, size_t dictSize, size_t chunk, int reuse)
{
    return decompressIntoOutputs(dctx, dst, dstCapacity, frame, frameSize, dict, dictSize, &chunk, 1, reuse);
}

/* decoding into outputs of various sizes, contiguous or not (dstBuffer can change between invocations) :
 * the decoder keeps the 64 KB of history needed by linked blocks, including matches at the maximum distance (65535),
 * across several small blocks, and into the dictionary */
TEST(decompress_historyIntoSmallOutputs)
{
    static const size_t s1[] = { 1 }, s2[] = { 7 }, s3[] = { 1000 }, s4[] = { 10 KB - 1 }, s5[] = { 64 KB + 1 },
                        s6[] = { 1000, 200 KB }, s7[] = { 200 KB, 1, 33 KB };
    static const struct { const size_t* sizes; size_t nb; } outputs[] = {
        { s1, 1 }, { s2, 1 }, { s3, 1 }, { s4, 1 }, { s5, 1 }, { s6, 2 }, { s7, 3 } };
    size_t const period = 65535;   /* maximum match distance */
    size_t const dictSize = 64 KB;
    size_t const srcSize = 300 KB + 7;
    static const size_t blockSizes[] = { 10 KB, 500, 64 KB - 100 };   /* small blocks : history spans several blocks */
    BYTE* const dict = (BYTE*)UT_noise(dictSize);
    BYTE* const src = (BYTE*)UT_malloc(srcSize);
    BYTE* const decoded = (BYTE*)UT_malloc(srcSize);
    size_t capacity, i, withDict, bi;
    BYTE* frame;
    /* each period repeats the previous one, with a byte changed every 1000 :
     * matches at the maximum distance, which can't be found anywhere else */
    memcpy(src, dict + dictSize - period, period);   /* first period : the end of the dictionary */
    for (i = period; i < srcSize; i++) src[i] = (i % 1000) ? src[i - period] : (BYTE)UT_rand();
    capacity = 2 * LZ4F_compressFrameBound(srcSize, NULL);
    frame = (BYTE*)UT_malloc(capacity);
    for (bi = 0; bi < ARRAY_SIZE(blockSizes); bi++)
    for (withDict = 0; withDict <= 1; withDict++) {
        size_t const blockSize = blockSizes[bi];
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;   /* linked blocks */
        LZ4F_compressOptions_t stable;
        LZ4F_cctx* const cctx = newCctx();
        const void* const d = withDict ? dict : NULL;
        size_t const dSize = withDict ? dictSize : 0;
        size_t frameSize, oi, consumed;
        prefs.compressionLevel = 9;   /* finds matches at the maximum distance */
        prefs.frameInfo.blockSizeID = LZ4F_max64KB;
        prefs.autoFlush = 1;          /* a block per invocation */
        memset(&stable, 0, sizeof(stable));
        stable.stableSrc = LZ4F_srcStable;   /* history remains within @src */
        frameSize = LZ4F_OK(LZ4F_compressBegin_usingDict(cctx, frame, capacity, d, dSize, &prefs));
        for (consumed = 0; consumed < srcSize; consumed += blockSize) {
            size_t const size = MIN(blockSize, srcSize - consumed);
            frameSize += LZ4F_OK(LZ4F_compressUpdate(cctx, frame + frameSize, capacity - frameSize, src + consumed, size, &stable));
        }
        frameSize += LZ4F_OK(LZ4F_compressEnd(cctx, frame + frameSize, capacity - frameSize, NULL));
        ASSERT_MSG(frameSize < 2 * period, "%u bytes : long distance matches not found", (unsigned)frameSize);
        for (oi = 0; oi < ARRAY_SIZE(outputs); oi++) {
            int reuse;
            for (reuse = 0; reuse <= 1; reuse++) {
                LZ4F_dctx* const dctx = newDctx();
                memset(decoded, 0, srcSize);
                ASSERT_EQ(decompressIntoOutputs(dctx, decoded, srcSize, frame, frameSize, d, dSize,
                                                outputs[oi].sizes, outputs[oi].nb, reuse), srcSize);
                ASSERT_MSG(!memcmp(decoded, src, srcSize), "blocks of %u bytes, dictionary %u, outputs %u, reused output %i : corrupted",
                           (unsigned)blockSize, (unsigned)withDict, (unsigned)oi, reuse);
                UT_release(dctx);
            }
        }
    }
}

/* linked blocks with a dictionary : the history isn't saved after the last block, when the endMark follows it within input,
 * but it must be whenever a block follows : compressed, or stored uncompressed (containing zeros, like an endMark),
 * with or without block checksums, decoded directly into the output, into outputs smaller than a block,
 * or with input ending right before the endMark */
TEST(decompress_endMarkAfterLastBlock)
{
    static const size_t dictSizes[] = { 64 KB, 1000 };   /* small dictionary : history shorter than 64 KB */
    size_t const partSize = 3000;
    size_t const decodedCapacity = 64 KB + 3 * 3000;   /* >= block size : blocks are decoded directly into it */
    static const size_t outSizes[] = { 1, 7, 64 KB + 3 * 3000 };
    BYTE* const dictBuffer = (BYTE*)UT_noise(64 KB);
    BYTE* const smallDict = (BYTE*)UT_malloc(1000);   /* end of dictBuffer, allocated apart : sanitizers detect reads before it */
    BYTE* const src = (BYTE*)UT_malloc(3 * partSize);
    BYTE* const decoded = (BYTE*)UT_malloc(decodedCapacity);
    size_t const capacity = LZ4F_compressFrameBound(3 * partSize, NULL) + 3 * (LZ4F_BLOCK_HEADER_SIZE + LZ4F_BLOCK_CHECKSUM_SIZE);
    BYTE* const frame = (BYTE*)UT_malloc(capacity);
    LZ4F_cctx* const cctx = newCctx();
    size_t i, di, nbParts, crc;
    /* part 1 : last 1000 bytes of the dictionary, repeated, modified ;
     * part 2 : noise containing 4 zero bytes every 600 bytes : stored uncompressed, it remains history ;
     * part 3 : end of part 1 and beginning of part 2, modified : references both */
    for (i = 0; i < partSize; i++) src[i] = (i % 100) ? dictBuffer[64 KB - 1000 + i % 1000] : (BYTE)UT_rand();
    for (i = 0; i < partSize; i++) src[partSize + i] = (i % 600 < 4) ? 0 : (BYTE)UT_rand();
    for (i = 0; i < partSize; i++) src[2 * partSize + i] = (i % 100) ? src[partSize / 2 + i] : (BYTE)UT_rand();
    memcpy(smallDict, dictBuffer + 64 KB - 1000, 1000);
    for (di = 0; di < ARRAY_SIZE(dictSizes); di++)
    for (nbParts = 1; nbParts <= 3; nbParts++)
    for (crc = 0; crc <= 1; crc++) {
        size_t const dictSize = dictSizes[di];
        const BYTE* const dict = (dictSize == 1000) ? smallDict : dictBuffer;
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;   /* linked blocks */
        size_t const srcSize = nbParts * partSize;
        size_t ends[8];
        size_t frameSize, p, oi;
        prefs.autoFlush = 1;   /* a block per part */
        prefs.frameInfo.blockChecksumFlag = (LZ4F_blockChecksum_t)crc;
        frameSize = LZ4F_OK(LZ4F_compressBegin_usingDict(cctx, frame, capacity, dict, dictSize, &prefs));
        for (p = 0; p < nbParts; p++)
            frameSize += LZ4F_OK(LZ4F_compressUpdate(cctx, frame + frameSize, capacity - frameSize, src + p * partSize, partSize, NULL));
        frameSize += LZ4F_OK(LZ4F_compressEnd(cctx, frame + frameSize, capacity - frameSize, NULL));
        ASSERT_EQ(frameElementEnds(ends, ARRAY_SIZE(ends), frame, frameSize), nbParts + 2);   /* header, blocks, endMark */
        for (p = 0; p < nbParts; p++)   /* part 2 is stored uncompressed, the others are compressed */
            ASSERT_EQ(readLE32(frame + ends[p]) >> 31, (unsigned)(p == 1));
        assertRequiresDict(frame, frameSize, srcSize);
        for (oi = 0; oi < ARRAY_SIZE(outSizes); oi++) {
            LZ4F_dctx* const dctx = newDctx();
            memset(decoded, 0, srcSize);
            ASSERT_EQ(decompressIntoChunks(dctx, decoded, decodedCapacity, frame, frameSize, dict, dictSize, outSizes[oi], 0), srcSize);
            ASSERT_MSG(!memcmp(decoded, src, srcSize), "dictionary of %u bytes, %u parts, block checksums %u, outputs of %u bytes : corrupted",
                       (unsigned)dictSize, (unsigned)nbParts, (unsigned)crc, (unsigned)outSizes[oi]);
            UT_release(dctx);
        }
        {   /* input ends right before the endMark : the history of the last block is saved, as before */
            LZ4F_dctx* const dctx = newDctx();
            size_t const endMarkPos = frameSize - LZ4F_BLOCK_HEADER_SIZE;
            size_t cSize = endMarkPos, dSize = decodedCapacity;
            memset(decoded, 0, srcSize);
            ASSERT(LZ4F_OK(LZ4F_decompress_usingDict(dctx, decoded, &dSize, frame, &cSize, dict, dictSize, NULL)) != 0);
            ASSERT_EQ(cSize, endMarkPos);
            ASSERT_EQ(dSize, srcSize);
            cSize = LZ4F_BLOCK_HEADER_SIZE;
            dSize = decodedCapacity - srcSize;
            ASSERT_EQ(LZ4F_OK(LZ4F_decompress_usingDict(dctx, decoded + srcSize, &dSize, frame + endMarkPos, &cSize, dict, dictSize, NULL)), 0);
            ASSERT_EQ(dSize, 0);
            ASSERT(!memcmp(decoded, src, srcSize));
            UT_release(dctx);
        }
    }
}

/* feeding exactly the hinted nb of bytes : each invocation consumes all of them,
 * frames end exactly where the hint becomes 0, and each block is decoded by a single invocation */
TEST(decompress_followingHints)
{
    size_t const payloadSize = 100;
    size_t const srcSize = 200 KB;
    const void* const src = UT_sample(srcSize);
    BYTE* const decoded = (BYTE*)UT_malloc(srcSize);
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    LZ4F_dctx* const dctx = newDctx();
    size_t ends[16] = { 0 };   /* header, 4 blocks, endMark, checksum */
    size_t capacity, frameSize, nbBlocks, total, pos, dPos, f;
    BYTE* buffer;
    prefs.frameInfo.blockSizeID = LZ4F_max64KB;
    prefs.frameInfo.contentSize = srcSize;   /* header of 15 bytes */
    prefs.frameInfo.blockChecksumFlag = LZ4F_blockChecksumEnabled;
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    capacity = 8 + payloadSize + LZ4F_compressFrameBound(srcSize, &prefs);
    buffer = (BYTE*)UT_malloc(capacity);
    memset(buffer, 0, 8 + payloadSize);   /* skippable frame, then a frame */
    writeLE32(buffer, LZ4F_MAGIC_SKIPPABLE_START);
    writeLE32(buffer + 4, (unsigned)payloadSize);
    frameSize = LZ4F_OK(LZ4F_compressFrame(buffer + 8 + payloadSize, capacity - 8 - payloadSize, src, srcSize, &prefs));
    nbBlocks = frameElementEnds(ends, ARRAY_SIZE(ends), buffer + 8 + payloadSize, frameSize) - 3;   /* minus header, endMark, checksum */
    total = 8 + payloadSize + frameSize;

    for (pos = dPos = f = 0; f < 2; f++) {
        size_t const frameEnd = f ? total : 8 + payloadSize;
        size_t const expectedCalls = f ? 2 + nbBlocks + 1 : 3;   /* header in 2 parts (minimum, then rest), blocks, checksum */
        size_t zero = 0, calls = 0;
        size_t hint = LZ4F_OK(LZ4F_decompress(dctx, NULL, &zero, NULL, &zero, NULL));   /* start of a frame */
        while (pos < frameEnd) {
            size_t const given = hint;
            size_t cSize = given, dSize = srcSize - dPos;
            ASSERT_MSG(hint > 0 && pos + hint <= frameEnd, "frame %u, position %u : hint %u", (unsigned)f, (unsigned)pos, (unsigned)hint);
            hint = LZ4F_OK(LZ4F_decompress(dctx, decoded + dPos, &dSize, buffer + pos, &cSize, NULL));
            ASSERT_EQ(cSize, given);
            pos += cSize;
            dPos += dSize;
            calls++;
        }
        ASSERT_EQ(pos, frameEnd);
        ASSERT_EQ(hint, 0);
        ASSERT_MSG(calls == expectedCalls, "frame %u : %u invocations instead of %u", (unsigned)f, (unsigned)calls, (unsigned)expectedCalls);
    }
    ASSERT_EQ(dPos, srcSize);
    ASSERT_MEMEQ(decoded, src, srcSize);
}

/* skippable frame : while its size field is incomplete, the hint is the nb of missing bytes */
TEST(decompress_skippableSizeHints)
{
    BYTE frame[8 + 10];
    char dst[16];
    LZ4F_dctx* const dctx = newDctx();
    LZ4F_frameInfo_t frameInfo;
    size_t size = sizeof(frame), k;
    memset(frame, 0, sizeof(frame));
    writeLE32(frame, LZ4F_MAGIC_SKIPPABLE_START);
    writeLE32(frame + 4, 10);
    LZ4F_OK(LZ4F_getFrameInfo(dctx, &frameInfo, frame, &size));
    ASSERT_EQ(size, 4);   /* magic number */
    for (k = 0; k < 4; k++) {   /* size field, byte after byte */
        size_t cSize = 1, dSize = sizeof(dst);
        size_t const hint = LZ4F_OK(LZ4F_decompress(dctx, dst, &dSize, frame + 4 + k, &cSize, NULL));
        ASSERT_EQ(cSize, 1);
        ASSERT_MSG(hint == ((k < 3) ? 3 - k : 10), "after %u bytes of the size field : hint %u", (unsigned)k + 1, (unsigned)hint);
    }
}

/* frame of 64 KB blocks, without checksums, containing @block : header (7 bytes), block, endMark */
static size_t craftFrame(BYTE* frame, const BYTE* block, size_t blockSize, unsigned uncompressed)
{
    size_t const headerSize = LZ4F_OK(LZ4F_compressBegin(newCctx(), frame, LZ4F_HEADER_SIZE_MAX, NULL));
    ASSERT_EQ(headerSize, 7);
    writeLE32(frame + 7, (unsigned)blockSize | (uncompressed ? 0x80000000U : 0));
    memcpy(frame + 11, block, blockSize);
    writeLE32(frame + 11 + blockSize, 0);
    return 15 + blockSize;
}

/* an invalid block is reported, whether it's decoded directly into dstBuffer or into an internal buffer (small dstBuffer) ;
 * a compressed block decoding into 0 byte is valid */
TEST(decompress_invalidAndEmptyBlocks)
{
    static const BYTE invalid[] = { 0x00, 0xFF, 0xFF };   /* match at distance 65535, before the frame's content */
    static const BYTE empty[] = { 0x00 };                  /* no literal, no match */
    static const size_t capacities[] = { 64 KB, 10 };
    BYTE frame[32];
    BYTE* const decoded = (BYTE*)UT_malloc(64 KB);
    size_t frameSize, i;
    frameSize = craftFrame(frame, invalid, sizeof(invalid), 0);
    for (i = 0; i < ARRAY_SIZE(capacities); i++) {
        size_t dSize = capacities[i], cSize = frameSize;
        ASSERT_LZ4F_ERROR(LZ4F_decompress(newDctx(), decoded, &dSize, frame, &cSize, NULL), LZ4F_ERROR_decompressionFailed);
    }
    frameSize = craftFrame(frame, empty, sizeof(empty), 0);
    for (i = 0; i < ARRAY_SIZE(capacities); i++) {
        size_t dSize = capacities[i], cSize = frameSize;
        ASSERT_EQ(LZ4F_OK(LZ4F_decompress(newDctx(), decoded, &dSize, frame, &cSize, NULL)), 0);
        ASSERT_EQ(dSize, 0);
        ASSERT_EQ(cSize, frameSize);
    }
}

/* a frame abandoned in the middle of a block (output pending within the context), then a reset :
 * the next frame doesn't output anything from the abandoned one */
TEST(decompress_resetPendingOutput)
{
    size_t const srcSize = 100 KB;
    const char* const src = (const char*)UT_sample(srcSize);
    const char* const other = (const char*)UT_sample(srcSize);
    char* const decoded = (char*)UT_malloc(srcSize);
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    char* const frame = (char*)UT_malloc(capacity);
    char* const frame2 = (char*)UT_malloc(capacity);
    LZ4F_dctx* const dctx = newDctx();
    size_t const frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, srcSize, NULL));
    size_t const frameSize2 = LZ4F_OK(LZ4F_compressFrame(frame2, capacity, other, srcSize, NULL));
    size_t cSize = frameSize, dSize = 1000;
    LZ4F_OK(LZ4F_decompress(dctx, decoded, &dSize, frame, &cSize, NULL));   /* output pending */
    ASSERT_EQ(dSize, 1000);
    LZ4F_resetDecompressionContext(dctx);
    ASSERT_EQ(decompressIntoChunks(dctx, (BYTE*)decoded, srcSize, (const BYTE*)frame2, frameSize2, NULL, 0, 777, 1), srcSize);
    ASSERT_MEMEQ(decoded, other, srcSize);
}

/* allocation failures of the decoder's internal buffers are reported, and leave the context usable */
TEST(decompress_allocFailures)
{
    size_t const srcSize = 300 KB;
    const BYTE* const src = (const BYTE*)UT_sample(srcSize);
    BYTE* const decoded = (BYTE*)UT_malloc(srcSize);
    allocState_t* const mem = newAllocState();
    LZ4F_preferences_t small = LZ4F_INIT_PREFERENCES, large = LZ4F_INIT_PREFERENCES;
    size_t capacity, smallSize, largeSize, n;
    BYTE* smallFrame;
    BYTE* largeFrame;
    small.frameInfo.blockSizeID = LZ4F_max64KB;
    large.frameInfo.blockSizeID = LZ4F_max4MB;   /* larger internal buffers */
    capacity = LZ4F_compressFrameBound(srcSize, &large);
    smallFrame = (BYTE*)UT_malloc(capacity);
    largeFrame = (BYTE*)UT_malloc(capacity);
    smallSize = LZ4F_OK(streamFrame(newCctx(), smallFrame, capacity, src, srcSize, NULL, 0, NULL, &small));
    largeSize = LZ4F_OK(streamFrame(newCctx(), largeFrame, capacity, src, srcSize, NULL, 0, NULL, &large));
    for (n = 1; ; n++) {
        LZ4F_dctx* dctx;
        size_t pos = 0, r = 1;
        mem->failAt = 0;
        dctx = createDctx(mem);
        ASSERT(dctx != NULL);
        ASSERT_EQ(decompressIntoChunks(dctx, decoded, srcSize, smallFrame, smallSize, NULL, 0, 1000, 1), srcSize);
        mem->nbAllocs = 0;
        mem->failAt = n;
        while (r != 0) {   /* larger frame : internal buffers are reallocated */
            size_t cSize = largeSize - pos, dSize = 1000;
            r = LZ4F_decompress(dctx, decoded, &dSize, largeFrame + pos, &cSize, NULL);
            if (LZ4F_isError(r)) break;
            pos += cSize;
        }
        mem->failAt = 0;
        if (mem->nbAllocs < n) {   /* decoding needed fewer allocations : all failure points tested */
            ASSERT_EQ(r, 0);
            UT_release(dctx);
            break;
        }
        ASSERT_LZ4F_ERROR(r, LZ4F_ERROR_allocation_failed);
        /* context remains usable : small frame, by segments, into small outputs (internal buffers) */
        LZ4F_resetDecompressionContext(dctx);
        {   size_t sPos = 0, dPos = 0;
            r = 1;
            while (r != 0) {
                size_t cSize = MIN(5000, smallSize - sPos), dSize = MIN(1000, srcSize - dPos);
                r = LZ4F_OK(LZ4F_decompress(dctx, decoded + dPos, &dSize, smallFrame + sPos, &cSize, NULL));
                sPos += cSize;
                dPos += dSize;
            }
            ASSERT_EQ(dPos, srcSize);
            ASSERT_MEMEQ(decoded, src, srcSize);
        }
        UT_release(dctx);
        ASSERT_MSG(mem->live == 0, "allocation %u : memory leak", (unsigned)n);
    }
}

/* LZ4F_createCDict_advanced() : each allocation failure is reported, without leak */
TEST(createCDict_allocFailures)
{
    size_t const dictSize = 64 KB;
    const void* const dict = UT_sample(dictSize);
    allocState_t* const mem = newAllocState();
    LZ4F_CustomMem const cmem = { failingAlloc, NULL, countingFree, mem };
    size_t n;
    for (n = 1; ; n++) {
        LZ4F_CDict* cdict;
        mem->nbAllocs = 0;
        mem->failAt = n;
        cdict = LZ4F_createCDict_advanced(cmem, dict, dictSize);
        mem->failAt = 0;
        if (mem->nbAllocs < n) {   /* all failure points tested */
            ASSERT(cdict != NULL);
            LZ4F_freeCDict(cdict);
            break;
        }
        ASSERT_MSG(cdict == NULL, "allocation %u : failure not reported", (unsigned)n);
        ASSERT_MSG(mem->live == 0, "allocation %u : memory leak", (unsigned)n);
    }
    ASSERT_EQ(mem->live, 0);
}

/* allocation failures of flush and end, when a block doesn't fit into dstBuffer (stage buffer), are reported,
 * and leave the context usable */
TEST(flushAndEnd_allocFailures)
{
    size_t const srcSize = 50 KB;   /* buffered : less than a block */
    const void* const src = UT_sample(srcSize);
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    char* const dst = (char*)UT_malloc(capacity);
    allocState_t* const mem = newAllocState();
    int end;
    for (end = 0; end <= 1; end++) {
        size_t n;
        for (n = 1; ; n++) {
            LZ4F_cctx* cctx;
            size_t pos, r;
            mem->failAt = 0;
            cctx = createCctx(mem);
            ASSERT(cctx != NULL);
            pos = LZ4F_OK(LZ4F_compressBegin(cctx, dst, capacity, NULL));
            pos += LZ4F_OK(LZ4F_compressUpdate(cctx, dst + pos, capacity - pos, src, srcSize, NULL));
            mem->nbAllocs = 0;
            mem->failAt = n;
            r = end ? LZ4F_compressEnd(cctx, dst + pos, 100, NULL) : LZ4F_flush(cctx, dst + pos, 100, NULL);
            mem->failAt = 0;
            if (mem->nbAllocs < n) {   /* all failure points tested */
                ASSERT_EQ(LZ4F_OK(r), 100);
                UT_release(cctx);
                break;
            }
            ASSERT_LZ4F_ERROR(r, LZ4F_ERROR_allocation_failed);
            ASSERT_LZ4F_ERROR(LZ4F_compressUpdate(cctx, dst, capacity, src, 10, NULL), LZ4F_ERROR_compressionState_uninitialized);
            /* context remains usable */
            assertDecodesTo(dst, LZ4F_OK(streamFrame(cctx, dst, capacity, src, srcSize, NULL, 0, NULL, NULL)), NULL, 0, src, srcSize);
            UT_release(cctx);
            ASSERT_MSG(mem->live == 0, "%s, allocation %u : memory leak", end ? "end" : "flush", (unsigned)n);
        }
    }
}

/* LZ4F_freeDecompressionContext() reports an unfinished frame */
TEST(freeDecompressionContext_unfinishedFrame)
{
    size_t const capacity = LZ4F_compressFrameBound(64, NULL);
    char* const frame = (char*)UT_malloc(capacity);
    char decoded[64];
    size_t const frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, UT_sample(64), 64, NULL));
    size_t size, dSize;
    LZ4F_errorCode_t unfinished, finished;
    LZ4F_dctx* dctx;
    LZ4F_OK(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION));
    size = frameSize - 1;
    dSize = sizeof(decoded);
    (void)LZ4F_decompress(dctx, decoded, &dSize, frame, &size, NULL);
    unfinished = LZ4F_freeDecompressionContext(dctx);
    LZ4F_OK(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION));
    size = frameSize;
    dSize = sizeof(decoded);
    (void)LZ4F_decompress(dctx, decoded, &dSize, frame, &size, NULL);
    finished = LZ4F_freeDecompressionContext(dctx);
    ASSERT(unfinished != 0);
    ASSERT_EQ(finished, 0);
}

/* results which are not error codes share the same name, distinct from error names */
TEST(getErrorName_notAnError)
{
    const char* const name = LZ4F_getErrorName(0);
    ASSERT(!strcmp(name, LZ4F_getErrorName(1000)));
    ASSERT(strcmp(name, LZ4F_getErrorName(LZ4F_getBlockSize((LZ4F_blockSizeID_t)3))) != 0);
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

/* LZ4F_writeOpen_advanced(), LZ4F_readOpen_advanced() : allocations employ the custom allocator,
 * including the index of resume points, and are all released on closing */
TEST(lz4file_customMem)
{
    size_t const srcSize = 3 MB;   /* a resume point per MB */
    const char* const src = (const char*)UT_sample(srcSize);
    char* const decoded = (char*)UT_malloc(srcSize);
    FILE* const file = newTmpFile();
    allocState_t* const mem = newAllocState();
    LZ4F_CustomMem const cmem = { failingAlloc, NULL, countingFree, mem };
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    LZ4_writeFile_t* writeCtx;
    LZ4_readFile_t* readCtx;
    prefs.frameInfo.blockMode = LZ4F_blockIndependent;   /* resume points at block starts */
    LZ4F_OK(LZ4F_writeOpen_advanced(&writeCtx, file, &prefs, cmem));
    ASSERT(mem->live > 0);
    LZ4F_OK(LZ4F_write(writeCtx, src, srcSize));
    LZ4F_OK(LZ4F_writeClose(writeCtx));
    ASSERT_EQ(mem->live, 0);

    rewind(file);
    LZ4F_OK(LZ4F_readOpen_advanced(&readCtx, file, cmem));
    ASSERT(mem->live > 0);
    ASSERT_EQ(LZ4F_OK(LZ4F_read(readCtx, decoded, srcSize)), srcSize);
    LZ4F_OK(LZ4F_seek(readCtx, 1500 KB, SEEK_SET));   /* restarts from a resume point, then skips */
    ASSERT_EQ(LZ4F_OK(LZ4F_read(readCtx, decoded, 100 KB)), 100 KB);
    ASSERT_MEMEQ(decoded, src + 1500 KB, 100 KB);
    LZ4F_OK(LZ4F_readClose(readCtx));
    ASSERT_EQ(mem->live, 0);
}

/* each allocation failure, while writing then reading a file, is reported, without leak */
TEST(lz4file_allocFailures)
{
    size_t const srcSize = 200 KB;   /* no resume point : the index is optional, its allocation failures are not reported */
    const char* const src = (const char*)UT_sample(srcSize);
    char* const decoded = (char*)UT_malloc(srcSize);
    FILE* const file = newTmpFile();
    allocState_t* const mem = newAllocState();
    LZ4F_CustomMem const cmem = { failingAlloc, NULL, countingFree, mem };
    size_t n;
    for (n = 1; ; n++) {   /* writing : open, write, close */
        LZ4_writeFile_t* writeCtx = NULL;
        size_t r;
        rewind(file);
        mem->nbAllocs = 0;
        mem->failAt = n;
        r = LZ4F_writeOpen_advanced(&writeCtx, file, NULL, cmem);
        if (LZ4F_isError(r)) {
            ASSERT(writeCtx == NULL);
        } else {
            LZ4F_write(writeCtx, src, srcSize);   /* a failure is also reported by LZ4F_writeClose() */
            r = LZ4F_writeClose(writeCtx);
        }
        mem->failAt = 0;
        ASSERT_MSG(mem->live == 0, "writing, allocation %u : memory leak", (unsigned)n);
        if (mem->nbAllocs < n) {   /* all failure points tested */
            LZ4F_OK(r);
            break;
        }
        ASSERT_LZ4F_ERROR(r, LZ4F_ERROR_allocation_failed);
    }
    ASSERT(n - 1 > 2);   /* more allocations than the handle and its buffer : the compression context employs cmem */
    for (n = 1; ; n++) {   /* reading : open, seek (skip buffer), read, close */
        LZ4_readFile_t* readCtx = NULL;
        size_t r;
        rewind(file);
        mem->nbAllocs = 0;
        mem->failAt = n;
        r = LZ4F_readOpen_advanced(&readCtx, file, cmem);
        if (LZ4F_isError(r)) {
            ASSERT(readCtx == NULL);
        } else {
            r = LZ4F_seek(readCtx, srcSize / 2, SEEK_SET);
            if (!LZ4F_isError(r)) r = LZ4F_read(readCtx, decoded, srcSize);
            LZ4F_OK(LZ4F_readClose(readCtx));
        }
        mem->failAt = 0;
        ASSERT_MSG(mem->live == 0, "reading, allocation %u : memory leak", (unsigned)n);
        if (mem->nbAllocs < n) {   /* all failure points tested */
            ASSERT_EQ(LZ4F_OK(r), srcSize - srcSize / 2);
            ASSERT_MEMEQ(decoded, src + srcSize / 2, srcSize - srcSize / 2);
            break;
        }
        ASSERT_LZ4F_ERROR(r, LZ4F_ERROR_allocation_failed);
    }
    ASSERT(n - 1 > 3);   /* more allocations than the handle and its 2 buffers : the decompression context employs cmem */
}

/* NULL arguments are reported, instead of dereferenced */
TEST(lz4file_nullArguments)
{
    FILE* const file = newTmpFile();
    char buf[16];
    LZ4_writeFile_t* writeCtx;
    LZ4_readFile_t* readCtx;
    ASSERT_LZ4F_ERROR(LZ4F_writeOpen(NULL, file, NULL), LZ4F_ERROR_parameter_null);
    LZ4F_OK(LZ4F_writeOpen(&writeCtx, file, NULL));
    ASSERT_LZ4F_ERROR(LZ4F_write(NULL, buf, sizeof(buf)), LZ4F_ERROR_parameter_null);
    ASSERT_LZ4F_ERROR(LZ4F_write(writeCtx, NULL, sizeof(buf)), LZ4F_ERROR_parameter_null);
    LZ4F_OK(LZ4F_writeClose(writeCtx));   /* not a write failure */
    ASSERT_LZ4F_ERROR(LZ4F_writeClose(NULL), LZ4F_ERROR_parameter_null);

    rewind(file);
    ASSERT_LZ4F_ERROR(LZ4F_readOpen(NULL, file), LZ4F_ERROR_parameter_null);
    LZ4F_OK(LZ4F_readOpen(&readCtx, file));
    ASSERT_LZ4F_ERROR(LZ4F_read(NULL, buf, sizeof(buf)), LZ4F_ERROR_parameter_null);
    ASSERT_LZ4F_ERROR(LZ4F_read(readCtx, NULL, sizeof(buf)), LZ4F_ERROR_parameter_null);
    ASSERT_EQ(LZ4F_read(readCtx, buf, sizeof(buf)), 0);   /* not a decoding error : empty frame */
    LZ4F_OK(LZ4F_readClose(readCtx));
    ASSERT_LZ4F_ERROR(LZ4F_readClose(NULL), LZ4F_ERROR_parameter_null);
    ASSERT_EQ(LZ4F_tell(NULL), 0);
}

/* LZ4F_readOpen() requires a frame header and an end mark (11 bytes) : shorter files are reported as truncated */
TEST(lz4file_readOpen_shortFiles)
{
    void* const stale = UT_malloc(1);   /* initial value of the handle */
    BYTE frame[LZ4F_HEADER_SIZE_MAX + 8];
    char buf[1] = { 0 };
    size_t const frameSize = LZ4F_OK(LZ4F_compressFrame(frame, sizeof(frame), buf, 0, NULL));
    size_t n;
    ASSERT_EQ(frameSize, LZ4F_HEADER_SIZE_MIN + 4);
    for (n = 0; n <= frameSize; n++) {
        FILE* const file = newTmpFile();
        LZ4_readFile_t* readCtx = (LZ4_readFile_t*)stale;
        ASSERT_EQ(fwrite(frame, 1, n, file), n);
        rewind(file);
        if (n < frameSize) {
            ASSERT_LZ4F_ERROR(LZ4F_readOpen(&readCtx, file), LZ4F_ERROR_io_read);
            ASSERT(readCtx == NULL);
        } else {
            LZ4F_OK(LZ4F_readOpen(&readCtx, file));
            ASSERT_EQ(LZ4F_read(readCtx, buf, sizeof(buf)), 0);
            LZ4F_OK(LZ4F_readClose(readCtx));
        }
        UT_release(file);
    }
}

#if defined(__linux__)
/* write failures are reported by the function writing : LZ4F_writeOpen() for the header,
 * LZ4F_writeClose() for the end of frame */
TEST(lz4file_writeFailures)
{
    FILE* fullFile = (FILE*)UT_defer(closeFile, fopen("/dev/full", "wb"));
    LZ4_writeFile_t* writeCtx = NULL;
    if (fullFile == NULL) return;
    ASSERT_EQ(setvbuf(fullFile, NULL, _IONBF, 0), 0);   /* each fwrite() fails */
    ASSERT_LZ4F_ERROR(LZ4F_writeOpen(&writeCtx, fullFile, NULL), LZ4F_ERROR_io_write);
    ASSERT(writeCtx == NULL);

    fullFile = (FILE*)UT_defer(closeFile, fopen("/dev/full", "wb"));
    ASSERT(fullFile != NULL);
    LZ4F_OK(LZ4F_writeOpen(&writeCtx, fullFile, NULL));   /* header remains buffered */
    LZ4F_OK(LZ4F_write(writeCtx, UT_fixedNoise(60 KB), 60 KB));   /* less than a block : remains within the compression context */
    ASSERT_LZ4F_ERROR(LZ4F_writeClose(writeCtx), LZ4F_ERROR_io_write);   /* last block exceeds the FILE buffer */
}
#endif

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
    static const struct { LZ4F_blockSizeID_t bsid; LZ4F_blockMode_t mode; int level; unsigned nbWorkers; } frames[] = {
        { LZ4F_max4MB,   LZ4F_blockIndependent, 1, 0 },
        { LZ4F_max64KB,  LZ4F_blockIndependent, 1, 0 },   /* smaller blocks : buffers are reused */
        { LZ4F_max256KB, LZ4F_blockLinked,      9, 0 },
        { LZ4F_max64KB,  LZ4F_blockLinked,      1, 0 },
        { LZ4F_max1MB,   LZ4F_blockIndependent, 9, 0 },
        { LZ4F_max64KB,  LZ4F_blockIndependent, 1, 0 },
        { LZ4F_max64KB,  LZ4F_blockLinked,      1, 3 },   /* jobs : multi-threading resources */
        { LZ4F_max1MB,   LZ4F_blockIndependent, 9, 2 },   /* HC states, larger jobs */
        { LZ4F_max64KB,  LZ4F_blockIndependent, 1, 0 } };
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
        prefs.nbWorkers = frames[f].nbWorkers;
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

/* flush into 0 byte is rejected while input is buffered, even a single byte */
TEST(flush_zeroCapacity)
{
    char dst[LZ4F_HEADER_SIZE_MAX];
    LZ4F_cctx* const cctx = newCctx();
    LZ4F_OK(LZ4F_compressBegin(cctx, dst, sizeof(dst), NULL));
    ASSERT_EQ(LZ4F_OK(LZ4F_compressUpdate(cctx, dst, sizeof(dst), "a", 1, NULL)), 0);   /* buffered */
    ASSERT_LZ4F_ERROR(LZ4F_flush(cctx, dst, 0, NULL), LZ4F_ERROR_dstCapacity_tooSmall);
}

/* an update first emits output left pending by an incomplete flush, entirely : with too little capacity, it fails,
 * and an end in progress no longer accepts input */
TEST(compressUpdate_afterIncompleteFlush)
{
    size_t const srcSize = 50 KB;   /* buffered : less than a block */
    const void* const src = UT_sample(srcSize);
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    char* const dst = (char*)UT_malloc(capacity);
    LZ4F_cctx* const cctx = newCctx();
    size_t pos;

    pos = LZ4F_OK(LZ4F_compressBegin(cctx, dst, capacity, NULL));
    ASSERT_EQ(LZ4F_OK(LZ4F_compressUpdate(cctx, dst + pos, capacity - pos, src, srcSize, NULL)), 0);
    ASSERT_EQ(LZ4F_OK(LZ4F_flush(cctx, dst + pos, 100, NULL)), 100);   /* incomplete : output pending */
    ASSERT_LZ4F_ERROR(LZ4F_compressUpdate(cctx, dst, 1000, src, 10, NULL), LZ4F_ERROR_dstCapacity_tooSmall);
    ASSERT_LZ4F_ERROR(LZ4F_compressUpdate(cctx, dst, capacity, src, 10, NULL), LZ4F_ERROR_compressionState_uninitialized);

    pos = LZ4F_OK(LZ4F_compressBegin(cctx, dst, capacity, NULL));
    LZ4F_OK(LZ4F_compressUpdate(cctx, dst + pos, capacity - pos, src, srcSize, NULL));
    ASSERT_EQ(LZ4F_OK(LZ4F_compressEnd(cctx, dst + pos, 100, NULL)), 100);   /* end in progress */
    ASSERT_LZ4F_ERROR(LZ4F_compressUpdate(cctx, dst, capacity, src, 10, NULL), LZ4F_ERROR_compressionState_uninitialized);
}

/* LZ4F_compressUpdate(), with compressed and uncompressed input mixed, respects LZ4F_compressBound() */
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
        LZ4F_compressOptions_t cOpts;
        size_t const firstSize = bufferedSizes[b];
        size_t const secondSize = inputSizes[i];
        size_t const srcSize = firstSize + secondSize;
        size_t frameSize, cSize, bound;
        unsigned update;

        memset(&cOpts, 0, sizeof(cOpts));
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
            cOpts.uncompressed = uncompressed;
            cSize = LZ4F_compressUpdate(cctx, dst, bound, input, inputSize, &cOpts);
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
                cOpts.uncompressed = uncompressedFirst;
                ASSERT_EQ(LZ4F_OK(LZ4F_compressUpdate(cctx, frame, bound, src, firstSize, &cOpts)), 0);
                dst[capacity] = 0xA5;
                cOpts.uncompressed = !uncompressedFirst;
                cSize = LZ4F_compressUpdate(cctx, dst, capacity, src + firstSize, secondSize, &cOpts);
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

/* uncompressed input (LZ4F_compressOptions_t.uncompressed) with linked blocks : the decoder's history contains uncompressed data,
 * which blocks compressed next must not mistake for previous content (nor for the dictionary).
 * Compressible input (copies within 32 KB), and a dictionary made of input, so that they would reference it.
 * Frames ending with uncompressed data are followed by frames starting with uncompressed data (same cctx). */
TEST(compressUpdate_uncompressedLinkedBlocks)
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
            cOpts.uncompressed = (*op == 'U' || *op == 'u');
            pos += LZ4F_OK(LZ4F_compressUpdate(cctx, frame + pos, frameCapacity - pos, src + consumed, size, &cOpts));
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

/* LZ4F_uncompressedUpdate() (deprecated) : same frame as LZ4F_compressUpdate() with option uncompressed,
 * with default options (NULL) or not */
TEST(uncompressedUpdate_deprecated)
{
    size_t const segSize = 100 KB;
    size_t const frameCapacity = LZ4F_compressFrameBound(3 * segSize, NULL) + 3 * LZ4F_compressBound(0, NULL);
    char* const src = (char*)UT_malloc(3 * segSize);
    char* const ref = (char*)UT_malloc(frameCapacity);
    char* const frame = (char*)UT_malloc(frameCapacity);
    LZ4F_cctx* const cctx = newCctx();
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    LZ4F_compressOptions_t cOpts, uncOpts;
    unsigned withOptions;
    memset(src, 'a', 3 * segSize);   /* compressed segments : a few hundred bytes */
    prefs.frameInfo.blockMode = LZ4F_blockLinked;
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;

    for (withOptions = 0; withOptions < 2; withOptions++) {
        size_t refSize, frameSize;
        memset(&cOpts, 0, sizeof(cOpts));
        cOpts.stableSrc = withOptions ? LZ4F_srcStable : LZ4F_srcUnstable;
        uncOpts = cOpts;
        uncOpts.uncompressed = 1;

        refSize = LZ4F_OK(LZ4F_compressBegin(cctx, ref, frameCapacity, &prefs));
        refSize += LZ4F_OK(LZ4F_compressUpdate(cctx, ref + refSize, frameCapacity - refSize, src, segSize, &cOpts));
        refSize += LZ4F_OK(LZ4F_compressUpdate(cctx, ref + refSize, frameCapacity - refSize, src + segSize, segSize, &uncOpts));
        refSize += LZ4F_OK(LZ4F_compressUpdate(cctx, ref + refSize, frameCapacity - refSize, src + 2 * segSize, segSize, &cOpts));
        refSize += LZ4F_OK(LZ4F_compressEnd(cctx, ref + refSize, frameCapacity - refSize, NULL));

        frameSize = LZ4F_OK(LZ4F_compressBegin(cctx, frame, frameCapacity, &prefs));
        frameSize += LZ4F_OK(LZ4F_compressUpdate(cctx, frame + frameSize, frameCapacity - frameSize, src, segSize, &cOpts));
        frameSize += LZ4F_OK(LZ4F_uncompressedUpdate(cctx, frame + frameSize, frameCapacity - frameSize, src + segSize, segSize,
                                                     withOptions ? &cOpts : NULL));
        frameSize += LZ4F_OK(LZ4F_compressUpdate(cctx, frame + frameSize, frameCapacity - frameSize, src + 2 * segSize, segSize, &cOpts));
        frameSize += LZ4F_OK(LZ4F_compressEnd(cctx, frame + frameSize, frameCapacity - frameSize, NULL));

        ASSERT_EQ(frameSize, refSize);
        ASSERT_MEMEQ(frame, ref, refSize);
        ASSERT(refSize > segSize);   /* the middle segment is stored uncompressed */
    }
}

static size_t frameWithOptions(LZ4F_cctx* cctx, char* dst, size_t dstCapacity, const char* src, size_t srcSize,
                               const LZ4F_compressOptions_t* cOpts)
{
    size_t pos = LZ4F_OK(LZ4F_compressBegin(cctx, dst, dstCapacity, NULL));
    pos += LZ4F_OK(LZ4F_compressUpdate(cctx, dst + pos, dstCapacity - pos, src, srcSize, cOpts));
    return pos + LZ4F_OK(LZ4F_compressEnd(cctx, dst + pos, dstCapacity - pos, NULL));
}

/* LZ4F_compressOptions_t.uncompressed : only 1 stores input as uncompressed blocks ;
 * other values (reserved, or left uninitialized) compress, as 0 */
TEST(compressUpdate_uncompressedValues)
{
    static const unsigned values[] = { 2, 0xA5A5A5A5u };
    size_t const srcSize = 100 KB;
    size_t const frameCapacity = LZ4F_compressFrameBound(srcSize, NULL);
    char* const src = (char*)UT_malloc(srcSize);
    char* const ref = (char*)UT_malloc(frameCapacity);
    char* const frame = (char*)UT_malloc(frameCapacity);
    LZ4F_cctx* const cctx = newCctx();
    LZ4F_compressOptions_t cOpts;
    size_t refSize, v;
    memset(src, 'a', srcSize);   /* compressed : a few hundred bytes */
    memset(&cOpts, 0, sizeof(cOpts));

    refSize = frameWithOptions(cctx, ref, frameCapacity, src, srcSize, &cOpts);
    for (v = 0; v < ARRAY_SIZE(values); v++) {
        cOpts.uncompressed = values[v];
        ASSERT_EQ(frameWithOptions(cctx, frame, frameCapacity, src, srcSize, &cOpts), refSize);
        ASSERT_MEMEQ(frame, ref, refSize);
    }
    cOpts.uncompressed = 1;
    ASSERT(frameWithOptions(cctx, frame, frameCapacity, src, srcSize, &cOpts) > srcSize);
    ASSERT(refSize < srcSize / 10);
}

/* linked blocks, with an input buffer reused between invocations (LZ4F_srcUnstable) :
 * the history needed by next blocks is saved within cctx, so that they compress better than independent blocks */
TEST(compressUpdate_reusedInputBuffer)
{
    static const int levels[] = { 1, 9 };
    size_t const blockSize = 64 KB;
    size_t const srcSize = 8 * blockSize;
    const char* const src = (const char*)UT_fixedSample(srcSize);   /* ratio check */
    char* const buffer = (char*)UT_malloc(blockSize);
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t capacity;
    char* dst;
    size_t li;
    prefs.frameInfo.blockSizeID = LZ4F_max64KB;
    capacity = LZ4F_compressFrameBound(srcSize, &prefs);
    dst = (char*)UT_malloc(capacity);
    for (li = 0; li < ARRAY_SIZE(levels); li++) {
        LZ4F_cctx* const cctx = newCctx();
        size_t independentSize, pos, consumed;
        prefs.compressionLevel = levels[li];
        prefs.frameInfo.blockMode = LZ4F_blockIndependent;
        independentSize = LZ4F_OK(streamFrame(cctx, dst, capacity, src, srcSize, NULL, 0, NULL, &prefs));

        prefs.frameInfo.blockMode = LZ4F_blockLinked;
        pos = LZ4F_OK(LZ4F_compressBegin(cctx, dst, capacity, &prefs));
        for (consumed = 0; consumed < srcSize; consumed += blockSize) {   /* full blocks, compressed from @buffer */
            memcpy(buffer, src + consumed, blockSize);
            pos += LZ4F_OK(LZ4F_compressUpdate(cctx, dst + pos, capacity - pos, buffer, blockSize, NULL));
        }
        memset(buffer, 0, blockSize);
        pos += LZ4F_OK(LZ4F_compressEnd(cctx, dst + pos, capacity - pos, NULL));
        assertDecodesTo(dst, pos, NULL, 0, src, srcSize);
        ASSERT_MSG(pos < independentSize, "level %i : linked blocks %u bytes, independent blocks %u bytes : history lost",
                   levels[li], (unsigned)pos, (unsigned)independentSize);
        UT_release(cctx);
    }
}

/* compresses @src with autoFlush, by updates of @updateSize bytes,
 * copied into @buffer when it's not NULL (LZ4F_srcUnstable), which is then overwritten,
 * or read in place otherwise (LZ4F_srcStable)
 * @return : frame size */
static size_t autoFlushFrame(LZ4F_cctx* cctx, char* dst, size_t capacity, const char* src, size_t srcSize,
                             size_t updateSize, char* buffer, const LZ4F_preferences_t* prefs)
{
    LZ4F_compressOptions_t cOpts;
    size_t pos, consumed;
    memset(&cOpts, 0, sizeof(cOpts));
    cOpts.stableSrc = (buffer == NULL);
    pos = LZ4F_OK(LZ4F_compressBegin(cctx, dst, capacity, prefs));
    for (consumed = 0; consumed < srcSize; consumed += updateSize) {
        size_t const size = MIN(updateSize, srcSize - consumed);
        const char* const in = buffer ? (const char*)memcpy(buffer, src + consumed, size) : src + consumed;
        pos += LZ4F_OK(LZ4F_compressUpdate(cctx, dst + pos, capacity - pos, in, size, &cOpts));
        if (buffer) memset(buffer, 0, size);
    }
    pos += LZ4F_OK(LZ4F_compressEnd(cctx, dst + pos, capacity - pos, NULL));
    assertDecodesTo(dst, pos, NULL, 0, src, srcSize);
    return pos;
}

/* autoFlush, linked blocks, input buffer reused by each update :
 * history remains whole (64 KB) with updates smaller than the history window.
 * (larger updates are compressed from the buffer, in external dictionary mode,
 * whose ratio differs slightly from input in place : see compressUpdate_reusedInputBuffer).
 * Fixed content : the ratio doesn't depend on the seed */
TEST(compressUpdate_autoFlushReusedBuffer)
{
    static const int levels[] = { 1, 9 };
    static const size_t updateSizes[] = { 1 KB, 10 KB, 64 KB - 1 };
    size_t const period = 50 KB;   /* noise repeated, with a modified byte every 1000 :
                                    * matches are within previous updates, at distance @period */
    size_t const srcSize = 8 * period;
    char* const src = (char*)UT_malloc(srcSize);
    char* const buffer = (char*)UT_malloc(64 KB);
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t capacity, n;
    char* dst;
    LZ4F_cctx* const cctx = newCctx();
    unsigned seed = 1;
    for (n = 0; n < srcSize; n++) {
        seed = seed * 1103515245u + 12345u;
        src[n] = (n < period || n % 1000 == 0) ? (char)(seed >> 16) : src[n - period];
    }
    prefs.autoFlush = 1;
    prefs.frameInfo.blockMode = LZ4F_blockLinked;
    prefs.frameInfo.blockSizeID = LZ4F_max64KB;
    capacity = LZ4F_compressFrameBound(srcSize, &prefs) + srcSize / 64;   /* updates of 1 KB : one block per update */
    dst = (char*)UT_malloc(capacity);
    for (n = 0; n < ARRAY_SIZE(levels) * ARRAY_SIZE(updateSizes); n++) {
        size_t const updateSize = updateSizes[n % ARRAY_SIZE(updateSizes)];
        size_t stableSize, unstableSize;
        prefs.compressionLevel = levels[n / ARRAY_SIZE(updateSizes)];
        stableSize = autoFlushFrame(cctx, dst, capacity, src, srcSize, updateSize, NULL, &prefs);
        unstableSize = autoFlushFrame(cctx, dst, capacity, src, srcSize, updateSize, buffer, &prefs);
        ASSERT_MSG(unstableSize <= stableSize + stableSize / 100,
                   "level %i, updates of %u bytes : %u bytes, %u bytes with input in place : history lost",
                   prefs.compressionLevel, (unsigned)updateSize, (unsigned)unstableSize, (unsigned)stableSize);
    }
}

/* favorDecSpeed applies to all blocks of a frame, including after an incompressible block :
 * with independent blocks, a block doesn't depend on the previous ones */
TEST(favorDecSpeed_afterRawBlock)
{
    size_t const blockSize = 64 KB;
    size_t const headerSize = 7;   /* no content size, no dictID */
    char* const src = (char*)UT_fixedNoise(2 * blockSize);   /* incompressible block, then compressible block */
    size_t const capacity = LZ4F_compressFrameBound(2 * blockSize, NULL);
    char* const frame = (char*)UT_malloc(capacity);
    char* const alone = (char*)UT_malloc(capacity);
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t frameSize, aloneSize, cBlockSize;
    memcpy(src + blockSize, UT_fixedSample(blockSize), blockSize);   /* favorDecSpeed must change its output */
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
    const char* const content = (const char*)UT_fixedSample(dictSize + srcSize);   /* favorDecSpeed must change the output */
    char* const dict = (char*)UT_malloc(dictSize);
    char* const src = (char*)UT_malloc(srcSize);
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL) + LZ4F_HEADER_SIZE_MAX;
    char* const ref = (char*)UT_malloc(capacity);
    char* const fds = (char*)UT_malloc(capacity);
    LZ4F_cctx* const cctx = newCctx();
    LZ4F_CDict* cdict;
    unsigned blockMode, beginMode;
    memcpy(dict, content, dictSize);
    memcpy(src, content + dictSize, srcSize);
    cdict = newCDict(dict, dictSize);

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


/* Streaming : LZ4F_compressBegin*(), LZ4F_compressUpdate() (compressed or uncompressed input),
 *             LZ4F_flush(), LZ4F_compressEnd() */

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
    case sd_uncompressedUpdate:
        {   LZ4F_compressOptions_t cOpts = s->cOpts;
            if (op->type == sd_uncompressedUpdate) cOpts.uncompressed = 1;
            return LZ4F_compressUpdate(s->cctx, dst, dstCapacity, s->src + op->srcPos, op->srcSize, &cOpts);
        }
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

/* LZ4F_resumeFrame() validates the frame information, and prepares a new context */
TEST(resumeFrame_arguments)
{
    size_t const srcSize = 100 KB;
    const BYTE* const src = (const BYTE*)UT_sample(srcSize);
    BYTE* const decoded = (BYTE*)UT_malloc(srcSize);
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    LZ4F_frameInfo_t fi, bad;
    size_t capacity, frameSize, headerSize, srcLen, dstLen;
    LZ4F_dctx* dctx;
    BYTE* frame;
    prefs.frameInfo.blockMode = LZ4F_blockIndependent;
    prefs.frameInfo.blockSizeID = LZ4F_max4MB;
    prefs.frameInfo.contentSize = srcSize;
    capacity = LZ4F_compressFrameBound(srcSize, &prefs);
    frame = (BYTE*)UT_malloc(capacity);
    frameSize = LZ4F_OK(streamFrame(newCctx(), frame, capacity, src, srcSize, NULL, 0, NULL, &prefs));
    headerSize = frameSize;
    LZ4F_OK(LZ4F_getFrameInfo(newDctx(), &fi, frame, &headerSize));
    ASSERT_EQ(fi.blockSizeID, LZ4F_max4MB);

    /* a new context resumes at the first block */
    dctx = newDctx();
    ASSERT_EQ(LZ4F_resumeFrame(dctx, &fi, 0, NULL, 0), 0);
    srcLen = frameSize - headerSize;
    dstLen = srcSize;
    ASSERT_EQ(LZ4F_OK(LZ4F_decompress(dctx, decoded, &dstLen, frame + headerSize, &srcLen, NULL)), 0);
    ASSERT_EQ(dstLen, srcSize);
    ASSERT_MEMEQ(decoded, src, srcSize);

    /* at the end of the content, only the end mark remains */
    dctx = newDctx();
    ASSERT_EQ(LZ4F_resumeFrame(dctx, &fi, srcSize, NULL, 0), 0);
    srcLen = 4;
    dstLen = srcSize;
    ASSERT_EQ(LZ4F_OK(LZ4F_decompress(dctx, decoded, &dstLen, frame + frameSize - 4, &srcLen, NULL)), 0);
    ASSERT_EQ(dstLen, 0);
    ASSERT_LZ4F_ERROR(LZ4F_resumeFrame(newDctx(), &fi, srcSize + 1, NULL, 0), LZ4F_ERROR_frameSize_wrong);

    bad = fi;
    bad.blockSizeID = (LZ4F_blockSizeID_t)3;
    ASSERT_LZ4F_ERROR(LZ4F_resumeFrame(newDctx(), &bad, 0, NULL, 0), LZ4F_ERROR_maxBlockSize_invalid);
    bad.blockSizeID = (LZ4F_blockSizeID_t)8;
    ASSERT_LZ4F_ERROR(LZ4F_resumeFrame(newDctx(), &bad, 0, NULL, 0), LZ4F_ERROR_maxBlockSize_invalid);
    bad = fi;
    bad.blockChecksumFlag = (LZ4F_blockChecksum_t)2;
    ASSERT_LZ4F_ERROR(LZ4F_resumeFrame(newDctx(), &bad, 0, NULL, 0), LZ4F_ERROR_parameter_invalid);
    bad = fi;
    bad.contentChecksumFlag = (LZ4F_contentChecksum_t)2;
    ASSERT_LZ4F_ERROR(LZ4F_resumeFrame(newDctx(), &bad, 0, NULL, 0), LZ4F_ERROR_parameter_invalid);
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

/* after a decoding error, LZ4F_read() keeps returning it, until a backward LZ4F_seek() restarts decoding.
 * The corrupted block is tiny : decoding it twice used to trip an assert within lz4frame.c */
TEST(lz4file_readAfterError)
{
    static const BYTE src[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const BYTE corrupted[] = { 1, 0, 0, 0,               /* compressed block of 1 byte */
                                      0x00,                     /* token : no literal, no match */
                                      0xAA, 0xAA, 0xAA, 0xAA,   /* wrong block checksum */
                                      0, 0, 0, 0 };             /* endMark */
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    LZ4F_cctx* const cctx = newCctx();
    BYTE frame[128];
    BYTE decoded[64];
    size_t size;
    FILE* f;
    LZ4_readFile_t* rf;
    prefs.frameInfo.blockChecksumFlag = LZ4F_blockChecksumEnabled;
    size = LZ4F_OK(LZ4F_compressBegin(cctx, frame, sizeof(frame), &prefs));
    size += LZ4F_OK(LZ4F_compressUpdate(cctx, frame + size, sizeof(frame) - size, src, sizeof(src), NULL));
    size += LZ4F_OK(LZ4F_flush(cctx, frame + size, sizeof(frame) - size, NULL));   /* first block : valid */
    memcpy(frame + size, corrupted, sizeof(corrupted));
    size += sizeof(corrupted);

    f = newTmpFile();
    ASSERT_EQ(fwrite(frame, 1, size, f), size);
    rewind(f);
    rf = newReadFile(f);
    ASSERT(LZ4F_getErrorCode(LZ4F_read(rf, decoded, sizeof(decoded))) == LZ4F_ERROR_blockChecksum_invalid);
    ASSERT(LZ4F_getErrorCode(LZ4F_read(rf, decoded, sizeof(decoded))) == LZ4F_ERROR_blockChecksum_invalid);
    LZ4F_OK(LZ4F_seek(rf, 0, SEEK_SET));
    ASSERT_EQ(LZ4F_OK(LZ4F_read(rf, decoded, sizeof(src))), sizeof(src));
    ASSERT_MEMEQ(decoded, src, sizeof(src));
    ASSERT(LZ4F_getErrorCode(LZ4F_read(rf, decoded, 1)) == LZ4F_ERROR_blockChecksum_invalid);
    UT_release(rf);
    UT_release(f);
}

/* seeks to @pos, then checks the next @size bytes against @ref */
static void fs_checkSeekRead(LZ4_readFile_t* rf, const BYTE* ref, size_t pos, size_t size)
{
    size_t const bufSize = 64 KB;
    BYTE* const buf = (BYTE*)UT_malloc(bufSize);
    size_t r = LZ4F_seek(rf, (long long)pos, SEEK_SET);
    ASSERT_MSG(!LZ4F_isError(r), "seek to %u : %s", (unsigned)pos, LZ4F_getErrorName(r));
    while (size > 0) {
        size_t const chunk = MIN(size, bufSize);
        r = LZ4F_read(rf, buf, chunk);
        ASSERT_MSG(r == chunk, "read at %u : %s", (unsigned)pos, LZ4F_isError(r) ? LZ4F_getErrorName(r) : "wrong size");
        ASSERT_MSG(!memcmp(buf, ref + pos, chunk), "wrong data at %u", (unsigned)pos);
        pos += chunk;
        size -= chunk;
    }
    UT_release(buf);
}

/* flips the byte at @pos of @f, then restores the position of @f, employed by a read handle */
static void fs_flipByte(FILE* f, long pos)
{
    long const current = ftell(f);
    int c;
    ASSERT(current >= 0);
    ASSERT_EQ(fseek(f, pos, SEEK_SET), 0);
    c = fgetc(f);
    ASSERT(c != EOF);
    ASSERT_EQ(fseek(f, pos, SEEK_SET), 0);
    ASSERT_EQ(fputc(c ^ 0xFF, f), c ^ 0xFF);
    ASSERT_EQ(fseek(f, current, SEEK_SET), 0);
}

/* LZ4F_seek() restarts from the closest preceding resume point (LZ4F_RESUME_SPAN : 1 MB) :
 * data before it is not decoded again, which is verified by corrupting it after a first pass.
 * Frames : A (1 MB, linked blocks), B (2 MB, independent blocks), C (1.5 MB, linked blocks).
 * Resume points : start of B (1 MB), block of B (2 MB), start of C (3 MB), end of file. */
TEST(lz4file_seek_closestResumePoint)
{
    size_t const sizeA = 1 MB, sizeB = 2 MB, sizeC = 1536 KB;
    size_t const totalSize = sizeA + sizeB + sizeC;
    const BYTE* const ref = (const BYTE*)UT_fixedSample(totalSize);
    BYTE buf[100];
    FILE* const f = newTmpFile();
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    long startB, endB;
    LZ4_readFile_t* rf;
    prefs.frameInfo.blockSizeID = LZ4F_max64KB;
    prefs.frameInfo.blockChecksumFlag = LZ4F_blockChecksumEnabled;   /* detects corruptions */
    fs_writeFrame(f, ref, sizeA, &prefs);
    startB = ftell(f);
    prefs.frameInfo.blockMode = LZ4F_blockIndependent;
    fs_writeFrame(f, ref + sizeA, sizeB, &prefs);
    endB = ftell(f);
    prefs.frameInfo.blockMode = LZ4F_blockLinked;
    prefs.frameInfo.blockChecksumFlag = LZ4F_noBlockChecksum;   /* C can't be decoded with the parameters of B */
    fs_writeFrame(f, ref + sizeA + sizeB, sizeC, &prefs);
    rewind(f);
    rf = newReadFile(f);
    LZ4F_OK(LZ4F_seek(rf, (long long)totalSize, SEEK_SET));   /* records resume points */

    /* corrupts the 1st block of A, and the first and last blocks of B (7-byte headers) */
    fs_flipByte(f, LZ4F_HEADER_SIZE_MIN + 4 + 10);
    fs_flipByte(f, startB + LZ4F_HEADER_SIZE_MIN + 4 + 10);
    fs_flipByte(f, endB - 4 - 4 - 1);   /* before its checksum and the end mark */
    LZ4F_OK(LZ4F_seek(rf, 0, SEEK_SET));
    ASSERT_LZ4F_ERROR(LZ4F_read(rf, buf, sizeof(buf)), LZ4F_ERROR_blockChecksum_invalid);

    fs_checkSeekRead(rf, ref, 2 MB, 64 KB);            /* block of B : clears the decoding error */
    fs_checkSeekRead(rf, ref, 3 MB + 100 KB, 64 KB);   /* forward : restarts from start of C, instead of decoding the end of B */
    fs_checkSeekRead(rf, ref, 4 MB + 200 KB, 64 KB);   /* forward : continues, without resume point at blocks of C */
    fs_checkSeekRead(rf, ref, 3 MB, 64 KB);            /* backward : start of C */
    LZ4F_OK(LZ4F_seek(rf, (long long)totalSize, SEEK_SET));
    ASSERT_EQ(LZ4F_read(rf, buf, 1), 0);

    /* corruptions are detected : seeks above didn't decode them */
    ASSERT_LZ4F_ERROR(LZ4F_seek(rf, 1 MB + 1, SEEK_SET), LZ4F_ERROR_blockChecksum_invalid);
    ASSERT_LZ4F_ERROR(LZ4F_seek(rf, 3 MB - 1, SEEK_SET), LZ4F_ERROR_blockChecksum_invalid);
}

/* resume points recorded after restarting from another frame employ the parameters of their own frame.
 * Frames : F1 (2 MB, independent blocks with checksums), F2 (3 MB, independent blocks, content size).
 * The index is extended in steps, each one restarting from F2 after decoding F1 :
 * resume points at 1 MB (block of F1) and 2 MB (start of F2), then 3 MB, then 4 MB. */
TEST(lz4file_seek_restartInAnotherFrame)
{
    size_t const size1 = 2 MB, size2 = 3 MB;
    size_t const totalSize = size1 + size2;
    const BYTE* const ref = (const BYTE*)UT_fixedSample(totalSize);
    BYTE buf[1];
    FILE* const f = newTmpFile();
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    LZ4_readFile_t* rf;
    size_t pos;
    prefs.frameInfo.blockSizeID = LZ4F_max64KB;
    prefs.frameInfo.blockMode = LZ4F_blockIndependent;
    prefs.frameInfo.blockChecksumFlag = LZ4F_blockChecksumEnabled;
    fs_writeFrame(f, ref, size1, &prefs);
    prefs.frameInfo.blockChecksumFlag = LZ4F_noBlockChecksum;   /* F2 can't be decoded with the parameters of F1 */
    prefs.frameInfo.contentSize = size2;
    fs_writeFrame(f, ref + size1, size2, &prefs);
    rewind(f);
    rf = newReadFile(f);

    LZ4F_OK(LZ4F_seek(rf, 2560 KB, SEEK_SET));
    fs_checkSeekRead(rf, ref, 1 MB, 100);        /* restarts from block of F1 */
    LZ4F_OK(LZ4F_seek(rf, 3584 KB, SEEK_SET));   /* restarts from start of F2 */
    fs_checkSeekRead(rf, ref, 0, 100);           /* restarts from start of file */
    LZ4F_OK(LZ4F_seek(rf, 4608 KB, SEEK_SET));   /* restarts from block of F2 at 3 MB */
    for (pos = 4 MB; pos >= 1 MB; pos -= 1 MB) {   /* each resume point decodes up to end of file */
        fs_checkSeekRead(rf, ref, pos, totalSize - pos);
        ASSERT_EQ(LZ4F_read(rf, buf, 1), 0);
    }
}

/* the index of resume points grows beyond its initial capacity (64 points), with the custom allocator */
TEST(lz4file_seek_manyResumePoints)
{
    size_t const blockSize = 1 MB;   /* a resume point per block */
    unsigned const nbBlocks = 70;
    BYTE* const block = (BYTE*)UT_malloc(blockSize);
    BYTE buf[4];
    FILE* const f = newTmpFile();
    allocState_t* const mem = newAllocState();
    LZ4F_CustomMem const cmem = { failingAlloc, NULL, countingFree, mem };
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    LZ4_writeFile_t* wf;
    LZ4_readFile_t* rf;
    unsigned n;
    prefs.frameInfo.blockMode = LZ4F_blockIndependent;
    prefs.frameInfo.blockSizeID = LZ4F_max1MB;
    memset(block, 0, blockSize);
    LZ4F_OK(LZ4F_writeOpen(&wf, f, &prefs));
    for (n = 0; n < nbBlocks; n++) {   /* blocks start with their index */
        writeLE32(block, n);
        LZ4F_OK(LZ4F_write(wf, block, blockSize));
    }
    LZ4F_OK(LZ4F_writeClose(wf));

    rewind(f);
    LZ4F_OK(LZ4F_readOpen_advanced(&rf, f, cmem));
    LZ4F_OK(LZ4F_seek(rf, (long long)nbBlocks * (long long)blockSize, SEEK_SET));   /* records resume points */
    for (n = nbBlocks; n-- > 0; ) {
        LZ4F_OK(LZ4F_seek(rf, (long long)n * (long long)blockSize, SEEK_SET));
        ASSERT_EQ(LZ4F_OK(LZ4F_read(rf, buf, sizeof(buf))), sizeof(buf));
        ASSERT_EQ(readLE32(buf), n);
    }
    LZ4F_OK(LZ4F_readClose(rf));
    ASSERT_EQ(mem->live, 0);
}

#ifdef PIPE_AVAILABLE
/* without file positions (pipe), LZ4F_seek() moves forward by decoding, but can't move backward */
TEST(lz4file_seek_pipe)
{
    size_t const srcSize = 8 KB;   /* the frame fits within the pipe buffer */
    const BYTE* const src = (const BYTE*)UT_fixedSample(srcSize);
    size_t const capacity = LZ4F_compressFrameBound(srcSize, NULL);
    BYTE* const frame = (BYTE*)UT_malloc(capacity);
    size_t const frameSize = LZ4F_OK(LZ4F_compressFrame(frame, capacity, src, srcSize, NULL));
    BYTE buf[100];
    int fds[2];
    FILE* f;
    LZ4_readFile_t* rf;
    ASSERT_EQ(pipe(fds), 0);
    ASSERT_EQ(write(fds[1], frame, frameSize), (long)frameSize);
    ASSERT_EQ(close(fds[1]), 0);
    f = (FILE*)UT_defer(closeFile, fdopen(fds[0], "rb"));
    ASSERT(f != NULL);
    rf = newReadFile(f);

    LZ4F_OK(LZ4F_seek(rf, 1000, SEEK_SET));
    LZ4F_OK(LZ4F_seek(rf, 0, SEEK_CUR));
    ASSERT_EQ(LZ4F_OK(LZ4F_read(rf, buf, sizeof(buf))), sizeof(buf));
    ASSERT_MEMEQ(buf, src + 1000, sizeof(buf));
    ASSERT_LZ4F_ERROR(LZ4F_seek(rf, 0, SEEK_SET), LZ4F_ERROR_io_read);
    ASSERT_EQ(LZ4F_tell(rf), 1100);
    LZ4F_OK(LZ4F_seek(rf, 2000, SEEK_CUR));
    ASSERT_EQ(LZ4F_OK(LZ4F_read(rf, buf, sizeof(buf))), sizeof(buf));
    ASSERT_MEMEQ(buf, src + 3100, sizeof(buf));
}
#endif
