/*
    unittest_lz4frameMT - unit tests of lz4frame multi-threaded compression
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

/* Multi-threaded compression (LZ4F_preferences_t.nbWorkers).
 * These tests run in builds with and without multi-threading support (LZ4F_MULTITHREAD) :
 * `make test-unittest` runs both, `unittest` and `unittest-mt`.
 * Without multi-threading support, multi-threaded requests are served serially, with the same results.
 * tests/fuzz/frameMT.c checks that both builds produce the same frames.
 * Assertions are only used in the calling thread : all allocations happen there too (see failingAlloc() in lz4frame_helpers.c). */

#include "unittest.h"
#include "lz4frame_helpers.h"   /* lz4frame.h, LZ4F_OK, newCctx, createCctx, newTmpFile, assertDecodesTo */
#include <stdio.h>      /* FILE, ftell, fseek, fread */
#include <string.h>     /* memcpy, memset, memcmp */
#include <limits.h>     /* INT_MAX */
#include "lz4file.h"
#include "lz4hc.h"      /* LZ4HC_CLEVEL_MIN */
#include "timefn.h"     /* TIME_getTime */


/* LZ4F_getMaxNbWorkers() reports multi-threading support */
TEST(getMaxNbWorkers)
{
    unsigned const maxWorkers = LZ4F_getMaxNbWorkers();
#if defined(LZ4F_MULTITHREAD) && LZ4F_MULTITHREAD
#  ifdef LZ4F_NBWORKERS_MAX
    ASSERT_EQ(maxWorkers, LZ4F_NBWORKERS_MAX);
#  else
    ASSERT(maxWorkers >= 1);
#  endif
#else
    ASSERT_EQ(maxWorkers, 0);
#endif
}

/* LZ4F_compressBound() : with frames cut into jobs, the job replaces the block as unit :
 * ceil(srcSize / jobSize) jobs (1 for srcSize == 0), each stored uncompressed in worst case, plus the frame footer,
 * whatever nbWorkers and multi-threading support : updates defer completed jobs not fitting into dstBuffer,
 * and flush / end emit jobs in flight in several invocations.
 * Sufficiency : see streamCompress() in tests/fuzz/frameMT.c, each invocation receiving exactly LZ4F_compressBound() */
TEST(compressBound_jobs)
{
    static const unsigned nbWorkersList[] = { 1, 8 };
    size_t const jobSize = 1 MB;   /* 64 KB blocks */
    size_t wi;
    for (wi = 0; wi < ARRAY_SIZE(nbWorkersList); wi++) {
        LZ4F_preferences_t p = LZ4F_INIT_PREFERENCES;
        size_t b0, b1, bJ, bJ1, b2J;
        p.frameInfo.blockMode = LZ4F_blockLinked;   /* frames cut into jobs, even without multi-threading support */
        p.nbWorkers = nbWorkersList[wi];
        b0 = LZ4F_compressBound(0, &p);
        b1 = LZ4F_compressBound(1, &p);
        bJ = LZ4F_compressBound(jobSize, &p);
        bJ1 = LZ4F_compressBound(jobSize + 1, &p);
        b2J = LZ4F_compressBound(2 * jobSize, &p);
        ASSERT_MSG((b0 == b1) && (b1 == bJ),
                   "%u workers : one job : %u, %u, %u", p.nbWorkers, (unsigned)b0, (unsigned)b1, (unsigned)bJ);
        ASSERT_MSG((bJ1 == b2J) && (b2J - bJ == bJ - LZ4F_ENDMARK_SIZE),
                   "%u workers : two jobs : %u, %u", p.nbWorkers, (unsigned)bJ1, (unsigned)b2J);
        ASSERT_MSG((bJ > jobSize) && (bJ < jobSize + 64 KB),
                   "%u workers : job bound %u", p.nbWorkers, (unsigned)bJ);
    }
}

/* autoFlush : each invocation compresses and emits all its input, whatever nbWorkers :
 * the output so far decodes into all input so far */
TEST(autoFlush_emitsAllInput)
{
    static const unsigned nbWorkersList[] = { 0, 2 };
    size_t const srcSize = 3 MB + 77;
    size_t const sliceSize = 300 KB + 5;   /* jobs (1 MB) are not completed by a single invocation */
    const char* const src = (const char*)UT_sample(srcSize);
    char* const decoded = (char*)UT_malloc(srcSize);
    size_t const dstCapacity = 2 * LZ4F_compressFrameBound(srcSize, NULL);
    char* const dst = (char*)UT_malloc(dstCapacity);
    size_t wi;
    int linked;
    for (wi = 0; wi < ARRAY_SIZE(nbWorkersList); wi++)
    for (linked = 0; linked <= 1; linked++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        LZ4F_cctx* const cctx = newCctx();
        size_t pos, consumed;
        prefs.autoFlush = 1;
        prefs.nbWorkers = nbWorkersList[wi];
        prefs.frameInfo.blockMode = linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
        pos = LZ4F_OK(LZ4F_compressBegin(cctx, dst, dstCapacity, &prefs));
        for (consumed = 0; consumed < srcSize; ) {
            size_t const slice = MIN(sliceSize, srcSize - consumed);
            LZ4F_dctx* const dctx = newDctx();
            size_t dSize = srcSize, cSize;
            pos += LZ4F_OK(LZ4F_compressUpdate(cctx, dst + pos, LZ4F_compressBound(slice, &prefs), src + consumed, slice, NULL));
            consumed += slice;
            cSize = pos;
            LZ4F_OK(LZ4F_decompress(dctx, decoded, &dSize, dst, &cSize, NULL));
            ASSERT_MSG((cSize == pos) && (dSize == consumed) && !memcmp(decoded, src, consumed),
                       "%u workers, linked=%i : %u bytes decoded after %u bytes of input",
                       prefs.nbWorkers, linked, (unsigned)dSize, (unsigned)consumed);
            UT_release(dctx);
        }
        pos += LZ4F_OK(LZ4F_compressEnd(cctx, dst + pos, LZ4F_compressBound(0, &prefs), NULL));
        assertDecodesTo(dst, pos, NULL, 0, src, srcSize);
        UT_release(cctx);
    }
}

/* with linked blocks, each job starts from the last 64 KB of preceding content (see LZ4F_compressJob()) :
 * a pattern repeated over several jobs isn't stored again by each job, compared to single-threaded compression
 * (not to a fixed size : depending on content and hash, level 1 can miss the pattern at some block starts) */
TEST(linkedJobs_history)
{
    static const int levels[] = { 1, 9 };
    size_t const patternSize = 48 KB;
    size_t const srcSize = 4 MB;   /* 4 jobs of 64 KB blocks */
    const char* const pattern = (const char*)UT_fixedNoise(patternSize);   /* ratio check */
    char* const src = (char*)UT_malloc(srcSize);
    size_t const dstCapacity = LZ4F_compressFrameBound(srcSize, NULL);
    char* const dst = (char*)UT_malloc(dstCapacity);
    size_t pos, li;
    for (pos = 0; pos < srcSize; pos += MIN(patternSize, srcSize - pos))
        memcpy(src + pos, pattern, MIN(patternSize, srcSize - pos));
    for (li = 0; li < ARRAY_SIZE(levels); li++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        size_t cSize, stSize;
        prefs.compressionLevel = levels[li];
        prefs.frameInfo.blockMode = LZ4F_blockLinked;
        prefs.frameInfo.blockSizeID = LZ4F_max64KB;
        stSize = LZ4F_OK(LZ4F_compressFrame(dst, dstCapacity, src, srcSize, &prefs));
        prefs.nbWorkers = 2;
        cSize = LZ4F_OK(LZ4F_compressFrame(dst, dstCapacity, src, srcSize, &prefs));
        ASSERT_MSG(cSize < stSize + patternSize, "level %i : %u bytes, %u bytes single-threaded : jobs ignore history",
                   levels[li], (unsigned)cSize, (unsigned)stSize);
        assertDecodesTo(dst, cSize, NULL, 0, src, srcSize);
    }
}

/* LZ4F_write() with workers, invoked with random sizes, produces the same frame as one-shot compression,
 * and LZ4F_read() decodes it */
static void checkWriteMatchesOneShot(LZ4F_blockMode_t blockMode)
{
    size_t const srcSize = 9 MB + 5;
    size_t const dstCapacity = LZ4F_compressFrameBound(srcSize, NULL);
    const char* const src = (const char*)UT_sample(srcSize);
    char* const ref = (char*)UT_malloc(dstCapacity);
    char* const out = (char*)UT_malloc(dstCapacity);
    char* const decoded = (char*)UT_malloc(srcSize + 1);
    FILE* const f = newTmpFile();
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    LZ4_writeFile_t* wf;
    LZ4_readFile_t* rf;
    size_t refSize, pos;
    long fileEnd;

    prefs.compressionLevel = 1;
    prefs.nbWorkers = 3;
    prefs.frameInfo.blockMode = blockMode;
    prefs.frameInfo.blockSizeID = LZ4F_max256KB;
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    refSize = LZ4F_OK(LZ4F_compressFrame_usingCDict(newCctx(), ref, dstCapacity, src, srcSize, NULL, &prefs));

    LZ4F_OK(LZ4F_writeOpen(&wf, f, &prefs));
    for (pos = 0; pos < srcSize; ) {
        size_t const sizeRand = (size_t)(UT_rand() % (2 MB)) + 1;   /* not within MIN() : evaluated once */
        size_t const size = MIN(sizeRand, srcSize - pos);
        ASSERT_EQ(LZ4F_OK(LZ4F_write(wf, src + pos, size)), size);
        pos += size;
    }
    LZ4F_OK(LZ4F_writeClose(wf));

    fileEnd = ftell(f);
    ASSERT(fileEnd >= 0);
    ASSERT_EQ((size_t)fileEnd, refSize);
    ASSERT_EQ(fseek(f, 0, SEEK_SET), 0);
    ASSERT_EQ(fread(out, 1, refSize, f), refSize);
    ASSERT_MEMEQ(out, ref, refSize);

    ASSERT_EQ(fseek(f, 0, SEEK_SET), 0);
    LZ4F_OK(LZ4F_readOpen(&rf, f));
    for (pos = 0; ; ) {
        size_t const r = LZ4F_OK(LZ4F_read(rf, decoded + pos, MIN(1 MB + 3, srcSize - pos + 1)));
        if (r == 0) break;
        pos += r;
        ASSERT(pos <= srcSize);
    }
    ASSERT_EQ(pos, srcSize);
    ASSERT_MEMEQ(decoded, src, srcSize);
    LZ4F_OK(LZ4F_readClose(rf));
}

TEST(write_independentBlocks) { checkWriteMatchesOneShot(LZ4F_blockIndependent); }
TEST(write_linkedBlocks) { checkWriteMatchesOneShot(LZ4F_blockLinked); }

/* LZ4F_srcStableUntilFlush : once flushed, input can be modified, except its last 64 KB (LZ4F_srcStable guarantee).
 * It's overwritten with upcoming content : reading it would produce matches that the decoder can't reproduce. */
static void scrambleFlushed(char* work, size_t* scrambledEnd, size_t consumed, const char* src, size_t srcSize)
{
    size_t const keep = 64 KB;
    size_t const start = *scrambledEnd;
    size_t const avail = srcSize - consumed;
    size_t end = (consumed > keep) ? consumed - keep : 0;
    if (end <= start) return;
    *scrambledEnd = end;
    /* filled backwards : upcoming content directly precedes the kept input */
    while (end > start) {
        size_t const n = avail ? MIN(end - start, avail) : end - start;
        if (avail) memcpy(work + end - n, src + consumed, n); else memset(work + end - n, 0xA5, n);
        end -= n;
    }
}

/* LZ4F_srcStableUntilFlush : once LZ4F_flush() returns, input can be modified, except its last 64 KB.
 * One buffer, sent in 2 invocations separated by a flush : the second one starts right where the first one ended,
 * so that, with linked blocks, its first job reads its history in place (multi-threaded),
 * and its first block references the previous invocation's input (single-threaded). */
TEST(flush_srcStableUntilFlush)
{
    static const unsigned nbWorkersList[] = { 0, 2 };
    size_t const part1 = 5 MB, srcSize = 13 MB;
    const char* const src = (const char*)UT_sample(srcSize);
    char* const work = (char*)UT_malloc(srcSize);
    LZ4F_cctx* const cctx = newCctx();
    LZ4F_preferences_t worst = LZ4F_INIT_PREFERENCES;
    size_t dstCapacity;
    char* dst;
    size_t wi;
    int autoFlush;

    worst.nbWorkers = 2;
    worst.frameInfo.blockSizeID = LZ4F_max64KB;
    worst.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    dstCapacity = 2 * LZ4F_compressBound(srcSize, &worst);
    dst = (char*)UT_malloc(dstCapacity);
    for (wi = 0; wi < ARRAY_SIZE(nbWorkersList); wi++)
    for (autoFlush = 0; autoFlush <= 1; autoFlush++) {
        LZ4F_preferences_t p = worst;
        LZ4F_compressOptions_t cOpts;
        size_t pos, scrambledEnd = 0;
        p.compressionLevel = 1;
        p.autoFlush = (unsigned)autoFlush;
        p.nbWorkers = nbWorkersList[wi];
        memset(&cOpts, 0, sizeof(cOpts));
        cOpts.stableSrc = LZ4F_srcStableUntilFlush;
        memcpy(work, src, srcSize);
        pos = LZ4F_OK(LZ4F_compressBegin(cctx, dst, dstCapacity, &p));
        pos += LZ4F_OK(LZ4F_compressUpdate(cctx, dst + pos, dstCapacity - pos, work, part1, &cOpts));
        pos += LZ4F_OK(LZ4F_flush(cctx, dst + pos, dstCapacity - pos, NULL));
        scrambleFlushed(work, &scrambledEnd, part1, src, srcSize);
        pos += LZ4F_OK(LZ4F_compressUpdate(cctx, dst + pos, dstCapacity - pos, work + part1, srcSize - part1, &cOpts));
        pos += LZ4F_OK(LZ4F_compressEnd(cctx, dst + pos, dstCapacity - pos, NULL));
        assertDecodesTo(dst, pos, NULL, 0, src, srcSize);
    }
}

/* LZ4F_srcStableUntilFlush with inputs which are not contiguous : a job reads its history in place
 * only when the previous input ends right before it ; otherwise, the history is copied */
TEST(flush_srcStableUntilFlush_nonContiguous)
{
    size_t const partSize = 2 MB;   /* whole jobs (64 KB blocks) */
    size_t const srcSize = 2 * partSize;
    const char* const src = (const char*)UT_sample(srcSize);
    char* const second = (char*)UT_malloc(64 KB + partSize);
    LZ4F_cctx* const cctx = newCctx();
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    LZ4F_compressOptions_t cOpts;
    size_t capacity, pos;
    char* dst;
    /* the 64 KB preceding the second part look like a history, but differ from the actual one */
    memcpy(second, src + partSize, 64 KB);
    memcpy(second + 64 KB, src + partSize, partSize);
    prefs.nbWorkers = 2;
    prefs.frameInfo.blockMode = LZ4F_blockLinked;
    prefs.frameInfo.blockSizeID = LZ4F_max64KB;
    capacity = LZ4F_compressFrameBound(srcSize, &prefs);
    dst = (char*)UT_malloc(capacity);
    memset(&cOpts, 0, sizeof(cOpts));
    cOpts.stableSrc = LZ4F_srcStableUntilFlush;
    pos = LZ4F_OK(LZ4F_compressBegin(cctx, dst, capacity, &prefs));
    pos += LZ4F_OK(LZ4F_compressUpdate(cctx, dst + pos, capacity - pos, src, partSize, &cOpts));
    pos += LZ4F_OK(LZ4F_compressUpdate(cctx, dst + pos, capacity - pos, second + 64 KB, partSize, &cOpts));
    pos += LZ4F_OK(LZ4F_compressEnd(cctx, dst + pos, capacity - pos, NULL));
    assertDecodesTo(dst, pos, NULL, 0, src, srcSize);
}

/* abandoned frames : jobs in flight when a new frame starts, or when the context is freed */
TEST(abandonedFrames)
{
    size_t const srcSize = 9 MB + 777;
    size_t const dstCapacity = 2 * LZ4F_compressFrameBound(srcSize, NULL);
    const char* const src = (const char*)UT_sample(srcSize);
    char* const out = (char*)UT_malloc(dstCapacity);
    allocState_t* const mem = newAllocState();
    LZ4F_cctx* const cctx = createCctx(mem);
    LZ4F_cctx* cctx2;
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    LZ4F_preferences_t prefs3;
    ASSERT(cctx != NULL);
    prefs.nbWorkers = 4;
    prefs.compressionLevel = 9;
    LZ4F_OK(LZ4F_compressBegin(cctx, out, dstCapacity, &prefs));
    LZ4F_OK(LZ4F_compressUpdate(cctx, out, dstCapacity, src, srcSize, NULL));
    prefs3 = prefs;
    prefs3.nbWorkers = 3;
    assertDecodesTo(out, LZ4F_OK(LZ4F_compressFrame_usingCDict(cctx, out, dstCapacity, src, srcSize, NULL, &prefs3)),
                    NULL, 0, src, srcSize);
    cctx2 = createCctx(mem);
    ASSERT(cctx2 != NULL);
    LZ4F_OK(LZ4F_compressBegin(cctx2, out, dstCapacity, &prefs));
    LZ4F_OK(LZ4F_compressUpdate(cctx2, out, dstCapacity, src, srcSize, NULL));
    UT_release(cctx2);
    UT_release(cctx);
    ASSERT_EQ(mem->live, 0);
}

/* compression scenarios */
enum { ONE_SHOT, STREAM_COPY, STREAM_IN_PLACE };

/* compresses @src : one-shot, or streaming, by slices copied into @scratch (STREAM_COPY), or read in place,
 * with a flush in the middle.
 * @return : frame size, or the first error */
static size_t compressScenario(LZ4F_cctx* cctx, int scenario, const LZ4F_preferences_t* prefs,
                               char* dst, size_t dstCapacity,
                               const char* src, size_t srcSize, char* scratch)
{
    size_t const slice = 1 MB + 777;
    LZ4F_compressOptions_t cOpts;
    size_t pos, consumed = 0, r;
    int flushed = 0;
    if (scenario == ONE_SHOT) return LZ4F_compressFrame_usingCDict(cctx, dst, dstCapacity, src, srcSize, NULL, prefs);
    memset(&cOpts, 0, sizeof(cOpts));
    cOpts.stableSrc = (scenario == STREAM_IN_PLACE) ? LZ4F_srcStableUntilFlush : LZ4F_srcUnstable;
    r = LZ4F_compressBegin(cctx, dst, dstCapacity, prefs);
    if (LZ4F_isError(r)) return r;
    pos = r;
    while (consumed < srcSize) {
        size_t const size = MIN(slice, srcSize - consumed);
        const char* in = src + consumed;
        if (scenario == STREAM_COPY) { memcpy(scratch, in, size); in = scratch; }
        r = LZ4F_compressUpdate(cctx, dst + pos, dstCapacity - pos, in, size, &cOpts);
        if (LZ4F_isError(r)) return r;
        pos += r;
        consumed += size;
        if (!flushed && (consumed > srcSize / 2)) {
            r = LZ4F_flush(cctx, dst + pos, dstCapacity - pos, NULL);
            if (LZ4F_isError(r)) return r;
            pos += r;
            flushed = 1;
        }
    }
    r = LZ4F_compressEnd(cctx, dst + pos, dstCapacity - pos, NULL);
    if (LZ4F_isError(r)) return r;
    return pos + r;
}

/* Each allocation fails in turn : the operation must fail with allocation_failed,
 * except failures preventing additional worker threads, which only reduce their nb (same output).
 * The context must remain usable, and nothing must leak. */
static void checkAllocFailures(int scenario)
{
    size_t const srcSize = 5 MB + 333;
    size_t const dstCapacity = 2 * LZ4F_compressFrameBound(srcSize, NULL);
    const char* const src = (const char*)UT_sample(srcSize);
    char* const scratch = (char*)UT_malloc(1 MB + 777);
    char* const ref = (char*)UT_malloc(dstCapacity);
    char* const out = (char*)UT_malloc(dstCapacity);
    allocState_t* const mem = newAllocState();
    unsigned nbFailures = 0;
    int linked;

    for (linked = 0; linked <= 1; linked++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        LZ4F_cctx* cctx;
        size_t refSize, maxN, n;
        prefs.compressionLevel = 1;
        prefs.nbWorkers = 4;
        prefs.frameInfo.blockMode = linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
        prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;

        /* reference, and nb of allocations */
        mem->failAt = 0;
        mem->nbAllocs = 0;
        cctx = createCctx(mem);
        ASSERT(cctx != NULL);
        refSize = LZ4F_OK(compressScenario(cctx, scenario, &prefs, ref, dstCapacity, src, srcSize, scratch));
        UT_release(cctx);
        maxN = mem->nbAllocs;   /* can vary slightly with scheduling : try a few more */

        for (n = 1; n <= maxN + 2; n++) {
            size_t r;
            mem->nbAllocs = 0;
            mem->failAt = n;
            cctx = createCctx(mem);
            if (cctx == NULL) {
                ASSERT_MSG(n == 1, "context creation failed at allocation %u", (unsigned)n);
                continue;
            }
            r = compressScenario(cctx, scenario, &prefs, out, dstCapacity, src, srcSize, scratch);
            if (LZ4F_isError(r)) {
                ASSERT_MSG(LZ4F_getErrorCode(r) == LZ4F_ERROR_allocation_failed,
                           "linked=%i, allocation %u : unexpected error %s", linked, (unsigned)n, LZ4F_getErrorName(r));
                nbFailures++;
            } else {   /* additional worker thread not started, or this run needed fewer allocations (scheduling) */
                ASSERT_MSG(r == refSize && !memcmp(out, ref, r), "linked=%i, allocation %u : output differs", linked, (unsigned)n);
            }
            /* context remains usable */
            mem->failAt = 0;
            r = compressScenario(cctx, scenario, &prefs, out, dstCapacity, src, srcSize, scratch);
            ASSERT_MSG(!LZ4F_isError(r) && (r == refSize) && !memcmp(out, ref, r),
                       "linked=%i, allocation %u : context not reusable", linked, (unsigned)n);
            UT_release(cctx);
            ASSERT_MSG(mem->live == 0, "linked=%i, allocation %u : memory leak (%u allocations)", linked, (unsigned)n, (unsigned)mem->live);
        }
    }
    ASSERT(nbFailures > 0);
}

TEST(allocFailures_oneShot) { checkAllocFailures(ONE_SHOT); }
TEST(allocFailures_streamCopy) { checkAllocFailures(STREAM_COPY); }
TEST(allocFailures_streamInPlace) { checkAllocFailures(STREAM_IN_PLACE); }

/* begin, update (whole input), flush, end : from operation @smallFrom onwards (0:update, 1:flush, 2:end ; 3:never),
 * each invocation only receives @capacity, instead of all remaining capacity. @smallFrom == 2 : no flush.
 * LZ4F_flush() and LZ4F_compressEnd() fill dstBuffer, or complete : they are invoked again while they fill it.
 * @return : frame size, or the first error, *failedOp being then the failed operation */
static size_t streamOps(LZ4F_cctx* cctx, const LZ4F_preferences_t* prefs,
                        char* dst, size_t dstCapacity, const char* src, size_t srcSize,
                        int smallFrom, size_t capacity, int* failedOp)
{
    size_t pos = LZ4F_compressBegin(cctx, dst, dstCapacity, prefs);
    int op;
    *failedOp = -1;
    if (LZ4F_isError(pos)) return pos;
    for (op = 0; op < 3; op++) {
        int again;
        if ((op == 1) && (smallFrom == 2)) continue;
        do {
            size_t const c = (op >= smallFrom) ? capacity : dstCapacity - pos;
            size_t r;
            if (op == 0) r = LZ4F_compressUpdate(cctx, dst + pos, c, src, srcSize, NULL);
            else if (op == 1) r = LZ4F_flush(cctx, dst + pos, c, NULL);
            else r = LZ4F_compressEnd(cctx, dst + pos, c, NULL);
            if (LZ4F_isError(r)) { *failedOp = op; return r; }
            pos += r;
            again = (op > 0) && (r == c);
        } while (again);
    }
    return pos;
}

/* Destination too small :
 * - LZ4F_compressUpdate() must ingest all input : it fails with dstCapacity_tooSmall when output must be written
 *   (with autoFlush, a full job slot ring, or nbWorkers == 0), and invalidates the frame :
 *   next operations fail with compressionState_uninitialized (in particular, no end mark : no truncated frame),
 *   until LZ4F_compressBegin() starts a new frame, identical to the reference. Same contract as nbWorkers == 0.
 * - LZ4F_flush() and LZ4F_compressEnd() fill dstBuffer, or complete : the frame is produced in pieces, identical to the reference. */
static void checkDstTooSmall(int smallFrom)
{
    static const unsigned nbWorkersList[] = { 0, 1, 3 };   /* 1 : job slots fill up within LZ4F_compressUpdate() */
    size_t const srcSize = 13 MB + 50 KB;   /* incomplete last job, and last block */
    size_t const smallCapacity = 1 KB;
    size_t const dstCapacity = 2 * LZ4F_compressFrameBound(srcSize, NULL);
    const char* const src = (const char*)UT_sample(srcSize);
    char* const ref = (char*)UT_malloc(dstCapacity);
    char* const out = (char*)UT_malloc(dstCapacity);
    allocState_t* const mem = newAllocState();
    size_t wi;
    int linked, autoFlush;

    for (wi = 0; wi < ARRAY_SIZE(nbWorkersList); wi++)
    for (linked = 0; linked <= 1; linked++)
    for (autoFlush = 0; autoFlush <= 1; autoFlush++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        /* compressUpdate() must write output into too little capacity ; otherwise, it can defer it (jobs) */
        int const mustFail = (smallFrom == 0) && (autoFlush || (nbWorkersList[wi] == 0));
        LZ4F_cctx* const cctx = createCctx(mem);
        size_t refSize, r;
        int failedOp;
        ASSERT(cctx != NULL);
        prefs.compressionLevel = 1;
        prefs.nbWorkers = nbWorkersList[wi];
        prefs.autoFlush = (unsigned)autoFlush;
        prefs.frameInfo.blockMode = linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
        prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        refSize = LZ4F_OK(streamOps(cctx, &prefs, ref, dstCapacity, src, srcSize, 3, 0, &failedOp));

        r = streamOps(cctx, &prefs, out, dstCapacity, src, srcSize, smallFrom, smallCapacity, &failedOp);
        if (!LZ4F_isError(r)) {
            ASSERT_MSG(!mustFail, "%u workers, linked=%i, autoFlush=%i : should have failed", prefs.nbWorkers, linked, autoFlush);
            ASSERT_MSG(r == refSize && !memcmp(out, ref, r), "%u workers, linked=%i, autoFlush=%i : output differs",
                       prefs.nbWorkers, linked, autoFlush);
        } else {
            ASSERT_MSG(LZ4F_getErrorCode(r) == LZ4F_ERROR_dstCapacity_tooSmall,
                       "%u workers : unexpected error %s", prefs.nbWorkers, LZ4F_getErrorName(r));
            ASSERT_MSG(failedOp == 0, "%u workers : %s failed", prefs.nbWorkers, (failedOp == 1) ? "flush" : "end");
            ASSERT_MSG(smallFrom == 0, "%u workers : failed with enough capacity", prefs.nbWorkers);
            /* frame invalidated */
            ASSERT_LZ4F_ERROR(LZ4F_compressUpdate(cctx, out, dstCapacity, src, 1 KB, NULL), LZ4F_ERROR_compressionState_uninitialized);
            ASSERT_LZ4F_ERROR(LZ4F_flush(cctx, out, dstCapacity, NULL), LZ4F_ERROR_compressionState_uninitialized);
            ASSERT_LZ4F_ERROR(LZ4F_compressEnd(cctx, out, dstCapacity, NULL), LZ4F_ERROR_compressionState_uninitialized);
        }
        /* context remains usable */
        r = streamOps(cctx, &prefs, out, dstCapacity, src, srcSize, 3, 0, &failedOp);
        ASSERT_MSG(r == refSize && !memcmp(out, ref, r), "%u workers, linked=%i, autoFlush=%i : next frame differs",
                   prefs.nbWorkers, linked, autoFlush);
        UT_release(cctx);
        ASSERT_EQ(mem->live, 0);
    }
}

TEST(dstTooSmall_fromUpdate) { checkDstTooSmall(0); }
TEST(dstTooSmall_fromEnd) { checkDstTooSmall(2); }

/* invokes LZ4F_flush() (@end == 0) or LZ4F_compressEnd() with @capacity, until it doesn't fill it,
 * or up to @maxCalls invocations (0 : no limit).
 * @return : nb of bytes written into @dst, or an error code */
static size_t flushPieces(LZ4F_cctx* cctx, int end, char* dst, size_t capacity, unsigned maxCalls)
{
    size_t pos = 0;
    unsigned n = 0;
    for (;;) {
        size_t const r = end ? LZ4F_compressEnd(cctx, dst + pos, capacity, NULL)
                             : LZ4F_flush(cctx, dst + pos, capacity, NULL);
        if (LZ4F_isError(r)) return r;
        pos += r;
        n++;
        if (r < capacity) break;
        if (maxCalls && (n >= maxCalls)) break;
    }
    return pos;
}

/* reference : begin, update(@part1), flush, then (@uncSize > 0) uncompressed update(@uncSize), update(rest), end,
 * each invocation receiving its bound. Position and size of flush output are reported. */
static size_t partialRef(LZ4F_cctx* cctx, const LZ4F_preferences_t* p, const LZ4F_compressOptions_t* cOpts,
                         char* dst, const char* src, size_t srcSize,
                         size_t part1, size_t uncSize, size_t* flushPos, size_t* flushSize)
{
    LZ4F_compressOptions_t uncOpts = *cOpts;
    size_t pos;
    uncOpts.uncompressed = 1;
    pos = LZ4F_OK(LZ4F_compressBegin(cctx, dst, LZ4F_HEADER_SIZE_MAX, p));
    pos += LZ4F_OK(LZ4F_compressUpdate(cctx, dst + pos, LZ4F_compressBound(part1, p), src, part1, cOpts));
    *flushPos = pos;
    *flushSize = LZ4F_OK(flushPieces(cctx, 0, dst + pos, LZ4F_compressBound(0, p), 0));
    pos += *flushSize;
    if (uncSize)
        pos += LZ4F_OK(LZ4F_compressUpdate(cctx, dst + pos, LZ4F_compressBound(uncSize, p), src + part1, uncSize, &uncOpts));
    pos += LZ4F_OK(LZ4F_compressUpdate(cctx, dst + pos, LZ4F_compressBound(srcSize - part1 - uncSize, p),
                                       src + part1 + uncSize, srcSize - part1 - uncSize, cOpts));
    return pos + LZ4F_OK(flushPieces(cctx, 1, dst + pos, LZ4F_compressBound(0, p), 0));
}

/* partial flush variants */
enum { PF_PIECES,            /* flush and end in pieces of a given capacity */
       PF_EXACT,             /* capacity exactly equal to remaining output */
       PF_UPDATE_BEFORE_END, /* update before the flush completes */
       PF_ABANDONED_END,     /* compressBegin() abandons an incomplete end */
       PF_ZERO_CAPACITY,     /* flush and end into 0 byte : rejected */
       PF_NB_VARIANTS };

/* LZ4F_flush() and LZ4F_compressEnd() fill dstBuffer, or complete :
 * output produced in pieces is identical to the reference, for any capacity, even 1 byte.
 * - the first invocation of a flush sets the flush point : input is no longer read (LZ4F_srcStableUntilFlush) ;
 * - capacity exactly equal to the remaining output : the next invocation returns 0 ;
 * - compressUpdate(), with compressed or uncompressed input, after an incomplete flush first emits the remaining output ;
 * - compressBegin() abandons an incomplete end ;
 * - capacity 0 is rejected when output remains, without modifying the frame. */
TEST(flush_partial)
{
    static const unsigned nbWorkersList[] = { 0, 1, 3 };
    static const size_t capacities[] = { 1, 4 KB + 1, 1 MB - 3 };
    size_t const srcSize = 9 MB + 123;
    size_t const part1 = 5 MB + 7;   /* jobs in flight at flush point */
    size_t const uncSize = 64 KB + 5;
    size_t const dstCapacity = 2 * LZ4F_compressFrameBound(srcSize, NULL) + 64 MB;
    const char* const src = (const char*)UT_sample(srcSize);
    char* const work = (char*)UT_malloc(srcSize);
    char* const ref = (char*)UT_malloc(dstCapacity);
    char* const refFull = (char*)UT_malloc(dstCapacity);
    char* const out = (char*)UT_malloc(dstCapacity);
    allocState_t* const mem = newAllocState();
    size_t wi, ci;
    int linked, variant;

    for (wi = 0; wi < ARRAY_SIZE(nbWorkersList); wi++)
    for (linked = 0; linked <= 1; linked++) {
        LZ4F_preferences_t p = LZ4F_INIT_PREFERENCES;
        LZ4F_cctx* const cctx = createCctx(mem);
        size_t refSize, refFullSize, flushPos, flushSize;
        ASSERT(cctx != NULL);
        p.nbWorkers = nbWorkersList[wi];
        p.frameInfo.blockMode = linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
        p.frameInfo.blockChecksumFlag = (LZ4F_blockChecksum_t)linked;
        p.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        {   LZ4F_compressOptions_t refOpts;
            memset(&refOpts, 0, sizeof(refOpts));
            refOpts.stableSrc = LZ4F_srcStableUntilFlush;   /* single-threaded linked output depends on stableSrc */
            refSize = partialRef(cctx, &p, &refOpts, ref, src, srcSize, part1, uncSize, &flushPos, &flushSize);
            refFullSize = LZ4F_OK(LZ4F_compressBegin(cctx, refFull, LZ4F_HEADER_SIZE_MAX, &p));
            refFullSize += LZ4F_OK(LZ4F_compressUpdate(cctx, refFull + refFullSize, LZ4F_compressBound(srcSize, &p), src, srcSize, NULL));
            refFullSize += LZ4F_OK(flushPieces(cctx, 1, refFull + refFullSize, LZ4F_compressBound(0, &p), 0));
        }

        for (ci = 0; ci < ARRAY_SIZE(capacities); ci++)
        for (variant = 0; variant < PF_NB_VARIANTS; variant++) {
            size_t const cap = capacities[ci];
            LZ4F_compressOptions_t cOpts, uncOpts;
            size_t pos, r, flushRemaining;
            if ((variant >= PF_UPDATE_BEFORE_END) && (cap != 4 KB + 1)) continue;   /* one capacity is enough */
            memset(&cOpts, 0, sizeof(cOpts));
            cOpts.stableSrc = LZ4F_srcStableUntilFlush;
            uncOpts = cOpts;
            uncOpts.uncompressed = 1;
            memcpy(work, src, srcSize);
            pos = LZ4F_OK(LZ4F_compressBegin(cctx, out, LZ4F_HEADER_SIZE_MAX, &p));
            pos += LZ4F_OK(LZ4F_compressUpdate(cctx, out + pos, LZ4F_compressBound(part1, &p), work, part1, &cOpts));
            /* output split between update and flush depends on scheduling (jobs) */
            ASSERT_MSG((pos <= flushPos + flushSize) && !memcmp(out, ref, pos), "update output differs");
            flushRemaining = flushPos + flushSize - pos;
            if (variant == PF_ZERO_CAPACITY)
                ASSERT_LZ4F_ERROR(LZ4F_flush(cctx, out + pos, 0, NULL), LZ4F_ERROR_dstCapacity_tooSmall);
            if (variant == PF_EXACT) {
                ASSERT_EQ(LZ4F_OK(LZ4F_flush(cctx, out + pos, flushRemaining, NULL)), flushRemaining);
                ASSERT_EQ(LZ4F_OK(LZ4F_flush(cctx, out + pos + flushRemaining, cap, NULL)), 0);   /* after exact fill */
                r = flushRemaining;
            } else {
                r = LZ4F_OK(flushPieces(cctx, 0, out + pos, cap, (variant == PF_UPDATE_BEFORE_END) ? 1 : 0));
            }
            memset(work, 0xA5, part1 - 64 KB);   /* flush point passed : input no longer read, except the last 64 KB (history) */
            if (variant == PF_UPDATE_BEFORE_END) {
                /* remaining flush output comes first */
                size_t const flushed = r;
                ASSERT_MSG(flushed <= flushRemaining, "flush output beyond reference");
                r = flushed + LZ4F_OK(LZ4F_compressUpdate(cctx, out + pos + flushed, LZ4F_compressBound(uncSize, &p),
                                                          work + part1, uncSize, &uncOpts));
                /* uncompressed blocks follow all compressed ones : no more output pending */
            } else {
                ASSERT_MSG(r == flushRemaining && !memcmp(out + pos, ref + pos, r), "%u workers, capacity %u : flush output differs",
                           p.nbWorkers, (unsigned)cap);
                r += LZ4F_OK(LZ4F_compressUpdate(cctx, out + pos + r, LZ4F_compressBound(uncSize, &p), work + part1, uncSize, &uncOpts));
            }
            pos += r;
            pos += LZ4F_OK(LZ4F_compressUpdate(cctx, out + pos, LZ4F_compressBound(srcSize - part1 - uncSize, &p),
                                               work + part1 + uncSize, srcSize - part1 - uncSize, &cOpts));
            if (variant == PF_ABANDONED_END) {
                LZ4F_OK(flushPieces(cctx, 1, out + pos, cap, 1));
                pos = LZ4F_OK(LZ4F_compressBegin(cctx, out, LZ4F_HEADER_SIZE_MAX, &p));
                pos += LZ4F_OK(LZ4F_compressUpdate(cctx, out + pos, LZ4F_compressBound(srcSize, &p), src, srcSize, NULL));
                pos += LZ4F_OK(flushPieces(cctx, 1, out + pos, LZ4F_compressBound(0, &p), 0));
                ASSERT_MSG(pos == refFullSize && !memcmp(out, refFull, pos), "%u workers : frame after abandoned end differs", p.nbWorkers);
                continue;
            }
            if (variant == PF_ZERO_CAPACITY)
                ASSERT_LZ4F_ERROR(LZ4F_compressEnd(cctx, out + pos, 0, NULL), LZ4F_ERROR_dstCapacity_tooSmall);
            if (variant == PF_EXACT) {
                size_t const endSize = refSize - pos;   /* output split between update and end depends on scheduling */
                ASSERT_EQ(LZ4F_OK(LZ4F_compressEnd(cctx, out + pos, endSize, NULL)), endSize);
                ASSERT_EQ(LZ4F_OK(LZ4F_compressEnd(cctx, out + pos + endSize, cap, NULL)), 0);   /* after exact fill */
                r = endSize;
            } else {
                r = LZ4F_OK(flushPieces(cctx, 1, out + pos, cap, 0));
            }
            pos += r;
            ASSERT_MSG(pos == refSize && !memcmp(out, ref, pos), "%u workers, linked=%i, capacity %u, variant %i : frame differs",
                       p.nbWorkers, linked, (unsigned)cap, variant);
            /* completed : the frame is over */
            ASSERT_LZ4F_ERROR(LZ4F_compressEnd(cctx, out, cap, NULL), LZ4F_ERROR_compressionState_uninitialized);
        }
        UT_release(cctx);
        ASSERT_EQ(mem->live, 0);
    }
}

/* Without autoFlush, completed jobs are only emitted when they fit :
 * a dstCapacity too small for them doesn't fail, as long as no job must be emitted (free job slot).
 * Deterministic without worker threads (jobs complete within LZ4F_compressUpdate()), otherwise depends on scheduling. */
TEST(compressUpdate_deferredEmission)
{
    static const unsigned nbWorkersList[] = { 1, 3 };
    size_t const jobSize = 1 MB;   /* a single job (64 KB blocks), submitted once full */
    size_t const smallCapacity = 1 KB;
    size_t const dstCapacity = 2 * LZ4F_compressFrameBound(jobSize, NULL);
    const char* const src = (const char*)UT_sample(jobSize);
    char* const ref = (char*)UT_malloc(dstCapacity);
    char* const out = (char*)UT_malloc(dstCapacity);
    size_t wi;
    int linked;

    for (wi = 0; wi < ARRAY_SIZE(nbWorkersList); wi++)
    for (linked = 0; linked <= 1; linked++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        LZ4F_cctx* cctx;
        size_t refSize, pos;
        if (!linked && (LZ4F_getMaxNbWorkers() == 0)) continue;   /* independent blocks : single-threaded mode, writing blocks immediately */
        cctx = newCctx();
        prefs.nbWorkers = nbWorkersList[wi];
        prefs.frameInfo.blockMode = linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
        refSize = LZ4F_OK(LZ4F_compressBegin(cctx, ref, dstCapacity, &prefs));
        refSize += LZ4F_OK(LZ4F_compressUpdate(cctx, ref + refSize, dstCapacity - refSize, src, jobSize, NULL));
        refSize += LZ4F_OK(LZ4F_compressEnd(cctx, ref + refSize, dstCapacity - refSize, NULL));

        pos = LZ4F_OK(LZ4F_compressBegin(cctx, out, dstCapacity, &prefs));
        ASSERT_MSG(LZ4F_OK(LZ4F_compressUpdate(cctx, out + pos, smallCapacity, src, jobSize, NULL)) == 0,
                   "%u workers, linked=%i : job emitted", prefs.nbWorkers, linked);
        {   TIME_t const start = TIME_getTime();   /* meanwhile, the job completes */
            while (TIME_clockSpan_ns(start) < 100000000ULL)
                ASSERT_MSG(LZ4F_OK(LZ4F_compressUpdate(cctx, out + pos, smallCapacity, NULL, 0, NULL)) == 0,
                           "%u workers, linked=%i : job emitted", prefs.nbWorkers, linked);
        }
        pos += LZ4F_OK(LZ4F_compressUpdate(cctx, out + pos, dstCapacity - pos, NULL, 0, NULL));
        pos += LZ4F_OK(LZ4F_compressEnd(cctx, out + pos, dstCapacity - pos, NULL));
        ASSERT_MSG(pos == refSize && !memcmp(out, ref, pos), "%u workers, linked=%i : output differs", prefs.nbWorkers, linked);
        UT_release(cctx);
    }
}

/* LZ4F_compressBegin*() validates its arguments first : rejecting them leaves a frame in progress untouched,
 * including its jobs in flight, still reading input in place (LZ4F_srcStableUntilFlush). Same contract as nbWorkers == 0. */
TEST(compressBegin_rejectedArguments)
{
    static const unsigned nbWorkersList[] = { 0, 1, 3 };
    size_t const srcSize = 6 MB + 123;
    size_t const firstPart = 2 MB + 5;   /* jobs in flight */
    size_t const dstCapacity = 2 * LZ4F_compressFrameBound(srcSize, NULL);
    const char* const src = (const char*)UT_sample(srcSize);
    char* const ref = (char*)UT_malloc(dstCapacity);
    char* const out = (char*)UT_malloc(dstCapacity);
    char header[LZ4F_HEADER_SIZE_MAX];
    LZ4F_compressOptions_t cOpts;
    size_t wi;
    int linked;

    memset(&cOpts, 0, sizeof(cOpts));
    cOpts.stableSrc = LZ4F_srcStableUntilFlush;
    for (wi = 0; wi < ARRAY_SIZE(nbWorkersList); wi++)
    for (linked = 0; linked <= 1; linked++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        LZ4F_preferences_t badPrefs;
        LZ4F_cctx* const cctx = newCctx();
        size_t refSize, pos;
        prefs.nbWorkers = nbWorkersList[wi];
        prefs.frameInfo.blockMode = linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
        prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        badPrefs = prefs;
        badPrefs.frameInfo.blockSizeID = (LZ4F_blockSizeID_t)3;

        /* reference */
        refSize = LZ4F_OK(LZ4F_compressBegin(cctx, ref, dstCapacity, &prefs));
        refSize += LZ4F_OK(LZ4F_compressUpdate(cctx, ref + refSize, dstCapacity - refSize, src, firstPart, &cOpts));
        refSize += LZ4F_OK(LZ4F_compressUpdate(cctx, ref + refSize, dstCapacity - refSize, src + firstPart, srcSize - firstPart, &cOpts));
        refSize += LZ4F_OK(LZ4F_compressEnd(cctx, ref + refSize, dstCapacity - refSize, NULL));

        /* rejected arguments : the frame in progress continues */
        pos = LZ4F_OK(LZ4F_compressBegin(cctx, out, dstCapacity, &prefs));
        pos += LZ4F_OK(LZ4F_compressUpdate(cctx, out + pos, dstCapacity - pos, src, firstPart, &cOpts));
        ASSERT_LZ4F_ERROR(LZ4F_compressBegin(cctx, header, 3, &prefs), LZ4F_ERROR_dstCapacity_tooSmall);
        ASSERT_LZ4F_ERROR(LZ4F_compressBegin(cctx, header, sizeof(header), &badPrefs), LZ4F_ERROR_maxBlockSize_invalid);
        ASSERT_LZ4F_ERROR(LZ4F_compressBegin_usingDict(cctx, header, sizeof(header), src, (size_t)INT_MAX + 1, &prefs),
                          LZ4F_ERROR_parameter_invalid);
        pos += LZ4F_OK(LZ4F_compressUpdate(cctx, out + pos, dstCapacity - pos, src + firstPart, srcSize - firstPart, &cOpts));
        pos += LZ4F_OK(LZ4F_compressEnd(cctx, out + pos, dstCapacity - pos, NULL));
        ASSERT_MSG(pos == refSize && !memcmp(out, ref, pos), "%u workers, linked=%i : frame disturbed by a rejected begin",
                   prefs.nbWorkers, linked);
        UT_release(cctx);
    }
}

/* LZ4F_compressBegin*() allocation failures invalidate the frame in progress :
 * next operations fail with compressionState_uninitialized, and the context remains usable. */
TEST(compressBegin_allocFailures)
{
    static const unsigned nbWorkersList[] = { 0, 1, 3 };
    size_t const srcSize = 6 MB + 123;
    size_t const firstPart = 2 MB + 5;   /* jobs in flight */
    size_t const nextSrcSize = 2 MB + 77;
    size_t const dstCapacity = 2 * LZ4F_compressFrameBound(srcSize, NULL);
    const char* const src = (const char*)UT_sample(srcSize);
    char* const nextRef = (char*)UT_malloc(dstCapacity);
    char* const out = (char*)UT_malloc(dstCapacity);
    allocState_t* const mem = newAllocState();
    LZ4F_compressOptions_t cOpts, uncOpts;
    unsigned nbFailures = 0;
    size_t wi;
    int linked;

    memset(&cOpts, 0, sizeof(cOpts));
    cOpts.stableSrc = LZ4F_srcStableUntilFlush;
    uncOpts = cOpts;
    uncOpts.uncompressed = 1;
    for (wi = 0; wi < ARRAY_SIZE(nbWorkersList); wi++)
    for (linked = 0; linked <= 1; linked++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        LZ4F_preferences_t nextPrefs = LZ4F_INIT_PREFERENCES;   /* HC state, larger blocks : resources are reallocated */
        LZ4F_cctx* cctx;
        size_t nextRefSize, pos, n;
        prefs.nbWorkers = nextPrefs.nbWorkers = nbWorkersList[wi];
        prefs.frameInfo.blockMode = nextPrefs.frameInfo.blockMode = linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
        prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        nextPrefs.compressionLevel = LZ4HC_CLEVEL_MIN;
        nextPrefs.frameInfo.blockSizeID = LZ4F_max256KB;

        /* reference */
        cctx = newCctx();
        nextRefSize = LZ4F_OK(LZ4F_compressBegin(cctx, nextRef, dstCapacity, &nextPrefs));
        nextRefSize += LZ4F_OK(LZ4F_compressUpdate(cctx, nextRef + nextRefSize, dstCapacity - nextRefSize, src, nextSrcSize, NULL));
        nextRefSize += LZ4F_OK(LZ4F_compressEnd(cctx, nextRef + nextRefSize, dstCapacity - nextRefSize, NULL));
        UT_release(cctx);

        /* each allocation of next frame's begin fails in turn, while a frame is in progress */
        for (n = 1; ; n++) {
            mem->failAt = 0;
            cctx = createCctx(mem);
            ASSERT(cctx != NULL);
            pos = LZ4F_OK(LZ4F_compressBegin(cctx, out, dstCapacity, &prefs));
            LZ4F_OK(LZ4F_compressUpdate(cctx, out + pos, dstCapacity - pos, src, firstPart, &cOpts));
            mem->nbAllocs = 0;
            mem->failAt = n;
            pos = LZ4F_compressBegin(cctx, out, dstCapacity, &nextPrefs);
            mem->failAt = 0;
            if (mem->nbAllocs < n) {   /* begin needed fewer allocations : all failure points tested */
                UT_release(cctx);
                break;
            }
            if (LZ4F_isError(pos)) {
                ASSERT_MSG(LZ4F_getErrorCode(pos) == LZ4F_ERROR_allocation_failed,
                           "allocation %u : unexpected error %s", (unsigned)n, LZ4F_getErrorName(pos));
                nbFailures++;
                ASSERT_LZ4F_ERROR(LZ4F_compressUpdate(cctx, out, dstCapacity, src, 1 KB, &cOpts), LZ4F_ERROR_compressionState_uninitialized);
                ASSERT_LZ4F_ERROR(LZ4F_compressUpdate(cctx, out, dstCapacity, src, 1 KB, &uncOpts), LZ4F_ERROR_compressionState_uninitialized);
                ASSERT_LZ4F_ERROR(LZ4F_flush(cctx, out, dstCapacity, NULL), LZ4F_ERROR_compressionState_uninitialized);
                ASSERT_LZ4F_ERROR(LZ4F_compressEnd(cctx, out, dstCapacity, NULL), LZ4F_ERROR_compressionState_uninitialized);
                pos = LZ4F_OK(LZ4F_compressBegin(cctx, out, dstCapacity, &nextPrefs));   /* context remains usable */
            }
            pos += LZ4F_OK(LZ4F_compressUpdate(cctx, out + pos, dstCapacity - pos, src, nextSrcSize, NULL));
            pos += LZ4F_OK(LZ4F_compressEnd(cctx, out + pos, dstCapacity - pos, NULL));
            ASSERT_MSG(pos == nextRefSize && !memcmp(out, nextRef, pos), "%u workers, linked=%i, allocation %u : next frame differs",
                       prefs.nbWorkers, linked, (unsigned)n);
            UT_release(cctx);
            ASSERT_MSG(mem->live == 0, "allocation %u : memory leak (%u allocations)", (unsigned)n, (unsigned)mem->live);
        }
    }
    ASSERT(nbFailures > 0);
}

/* An allocation failure preventing the first worker thread fails the frame (allocation_failed) :
 * jobs are not processed serially instead. Failures preventing additional worker threads only reduce their nb.
 * The context remains usable : next frames create resources again. */
TEST(workers_allocFailures)
{
    size_t const srcSize = 9 MB;
    const char* const src = (const char*)UT_sample(srcSize);
    allocState_t* const mem = newAllocState();
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t dstCapacity, refSize, n;
    char* ref;
    char* out;
    unsigned nbFailures = 0;

    prefs.nbWorkers = 2;
    prefs.frameInfo.blockMode = LZ4F_blockIndependent;
    dstCapacity = LZ4F_compressFrameBound(srcSize, &prefs);
    ref = (char*)UT_malloc(dstCapacity);
    out = (char*)UT_malloc(dstCapacity);
    refSize = LZ4F_OK(LZ4F_compressFrame(ref, dstCapacity, src, srcSize, &prefs));

    for (n = 1; ; n++) {
        LZ4F_cctx* cctx;
        size_t r;
        mem->failAt = 0;
        cctx = createCctx(mem);
        ASSERT(cctx != NULL);
        mem->nbAllocs = 0;
        mem->failAt = n;
        r = LZ4F_compressFrame_usingCDict(cctx, out, dstCapacity, src, srcSize, NULL, &prefs);
        mem->failAt = 0;
        if (mem->nbAllocs < n) {   /* the frame needed fewer allocations : all failure points tested */
            UT_release(cctx);
            break;
        }
        if (LZ4F_isError(r)) {
            ASSERT_MSG(LZ4F_getErrorCode(r) == LZ4F_ERROR_allocation_failed, "allocation %u : %s", (unsigned)n, LZ4F_getErrorName(r));
            nbFailures++;
        } else {   /* additional worker thread not started */
            ASSERT_MSG(r == refSize && !memcmp(out, ref, r), "allocation %u absorbed : output differs", (unsigned)n);
        }
        r = LZ4F_compressFrame_usingCDict(cctx, out, dstCapacity, src, srcSize, NULL, &prefs);
        ASSERT_MSG(r == refSize && !memcmp(out, ref, r), "allocation %u : next frame differs", (unsigned)n);
        UT_release(cctx);
    }
    ASSERT(nbFailures > 0);
    ASSERT_EQ(mem->live, 0);
}

/* after an allocation failure, the context can be freed right away, whatever resources remain (ex: no pool) */
TEST(workers_allocFailures_free)
{
    size_t const srcSize = 3 MB;
    const char* const src = (const char*)UT_sample(srcSize);
    allocState_t* const mem = newAllocState();
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    size_t dstCapacity, n;
    char* dst;

    prefs.nbWorkers = 2;
    dstCapacity = LZ4F_compressFrameBound(srcSize, &prefs);
    dst = (char*)UT_malloc(dstCapacity);
    for (n = 1; ; n++) {
        LZ4F_cctx* cctx;
        size_t r;
        mem->failAt = 0;
        cctx = createCctx(mem);
        ASSERT(cctx != NULL);
        mem->nbAllocs = 0;
        mem->failAt = n;
        r = LZ4F_compressFrame_usingCDict(cctx, dst, dstCapacity, src, srcSize, NULL, &prefs);
        mem->failAt = 0;
        UT_release(cctx);   /* right after the failure */
        ASSERT_MSG(mem->live == 0, "allocation %u : memory leak (%u allocations)", (unsigned)n, (unsigned)mem->live);
        if (mem->nbAllocs < n) break;   /* the frame needed fewer allocations : all failure points tested */
        ASSERT_MSG(!LZ4F_isError(r) || (LZ4F_getErrorCode(r) == LZ4F_ERROR_allocation_failed),
                   "allocation %u : %s", (unsigned)n, LZ4F_getErrorName(r));
    }
}
