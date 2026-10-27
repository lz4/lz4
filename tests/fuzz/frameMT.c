/*
    frameMT - validate multi-threaded frame compression
    Copyright (c) Yann Collet. All rights reserved.

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

/* Validates LZ4F_preferences_t.nbWorkers, with one-shot compression
 * (LZ4F_compressFrame_usingCDict(), LZ4F_compressFrame()) and streaming (LZ4F_compressUpdate()) :
 * - round trip
 * - output is identical for any nbWorkers >= 1
 * - streaming : same output as one-shot, whatever input slicing and stableSrc ;
 *   flushes, autoFlush and uncompressed segments produce the same output whatever nbWorkers ;
 *   with independent blocks, the same output as nbWorkers == 0 ;
 *   each invocation succeeds with exactly LZ4F_compressBound(), which only covers job slots actually employed ;
 *   input isn't read after invocation, unless LZ4F_srcStableUntilFlush ;
 *   abandoned frames (jobs in flight) are handled
 * - with independent blocks, output is identical to nbWorkers == 0
 * - raw dictionary (LZ4F_compressBegin_usingDict()) : same output whatever nbWorkers, the same as its CDict at levels >= 3
 * - serial processing (dstCapacity too small for parallel jobs) produces the same output
 * - destination too small : compressUpdate fails and invalidates the frame (no truncated frame), the context remains usable ;
 *   completed jobs not fitting yet are emitted later, without failure ; flush and end produce output in pieces
 * - flush and end in pieces, for any capacity : same output ; flush point, exact fill, update after an incomplete flush
 * - LZ4F_compressBegin() rejecting its arguments leaves the frame in progress (jobs in flight) untouched ;
 *   its allocation failures invalidate it
 * - allocation failures are reported (allocation_failed), except those preventing additional worker threads : context remains usable
 * - all allocations happen in the calling thread (custom allocator is not thread-safe : see TSAN)
 * - prints a digest of produced frames, to compare builds with and without LZ4F_MULTITHREAD
 * Unit tests of multi-threaded compression : see tests/unit/unittest_lz4frameMT.c
 * Bench mode : frameMT -b FILE [-l#] [-B#] [-D] [-c] [-T#]
 * Compress once with # workers : frameMT -b FILE -o OUTFILE [-l#] [-B#] [-D] [-c] [-T#]
 */

/*-************************************
*  Includes
**************************************/
#include <stdlib.h>     /* malloc, free, exit */
#include <stdio.h>      /* fprintf, fopen */
#include <string.h>     /* memcmp, strcmp */
#include <limits.h>     /* INT_MAX */
#define LZ4F_STATIC_LINKING_ONLY
#include "lz4frame.h"
#include "lz4hc.h"      /* LZ4HC_CLEVEL_MIN */
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"
#include "datagen.h"
#include "timefn.h"


/*-************************************
*  Macros
**************************************/
#define KB *(1<<10)
#define MB *(1<<20)

#define DISPLAY(...) fprintf(stderr, __VA_ARGS__)
#define CHECK(c, ...) do { if (!(c)) {                         \
        DISPLAY("Error at line %i (%s) : ", __LINE__, g_step); \
        DISPLAY(__VA_ARGS__);                                  \
        DISPLAY(" \n");                                        \
        exit(1);                                               \
    } } while (0)
#define CHECK_LZ4F(r) CHECK(!LZ4F_isError(r), "%s", LZ4F_getErrorName(r))

static int g_verbose = 0;
static const char* g_step = "";   /* current step, for diagnostics */
static int g_quick = 0;     /* fewer configurations (-q), for sanitizers and make test */

#undef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))


/*-************************************
*  Non thread-safe allocator
**************************************/
/* Counters are deliberately unprotected :
 * an allocation from a worker thread would be reported by ThreadSanitizer */
static size_t g_nbAllocs = 0;
static size_t g_nbFrees = 0;

static void* countingAlloc(void* opaque, size_t size)
{
    (void)opaque;
    g_nbAllocs++;
    return malloc(size);
}

static void countingFree(void* opaque, void* address)
{
    (void)opaque;
    if (address == NULL) return;
    g_nbFrees++;
    free(address);
}


/*-************************************
*  Helpers
**************************************/
static size_t compressMT(LZ4F_cctx* cctx, void* dst, size_t dstCapacity,
                         const void* src, size_t srcSize,
                         const LZ4F_CDict* cdict, const LZ4F_preferences_t* prefs,
                         unsigned nbWorkers)
{
    LZ4F_preferences_t mtPrefs = *prefs;
    mtPrefs.nbWorkers = nbWorkers;
    return LZ4F_compressFrame_usingCDict(cctx, dst, dstCapacity, src, srcSize, cdict, &mtPrefs);
}

/* begins a frame with raw dictionary @dict, then compresses @src in a single invocation */
static size_t compressRawDict(LZ4F_cctx* cctx, char* dst, size_t dstCapacity,
                              const char* src, size_t srcSize,
                              const void* dict, size_t dictSize, const LZ4F_preferences_t* prefs,
                              unsigned nbWorkers)
{
    LZ4F_preferences_t p = *prefs;
    size_t pos, r;
    p.nbWorkers = nbWorkers;
    pos = LZ4F_compressBegin_usingDict(cctx, dst, dstCapacity, dict, dictSize, &p);
    if (LZ4F_isError(pos)) return pos;
    r = LZ4F_compressUpdate(cctx, dst + pos, dstCapacity - pos, src, srcSize, NULL);
    if (LZ4F_isError(r)) return r;
    pos += r;
    r = LZ4F_compressEnd(cctx, dst + pos, dstCapacity - pos, NULL);
    if (LZ4F_isError(r)) return r;
    return pos + r;
}

/* when environment variable FRAMEMT_DUMP is set, writes @buffer to FRAMEMT_DUMP@suffix */
static void dumpFile(const char* suffix, const void* buffer, size_t size)
{
    const char* const prefix = getenv("FRAMEMT_DUMP");
    char name[1024];
    FILE* f;
    if ((prefix == NULL) || (buffer == NULL)) return;
    snprintf(name, sizeof(name), "%s%s", prefix, suffix);
    f = fopen(name, "wb");
    if (f) { fwrite(buffer, 1, size, f); fclose(f); }
}

static void checkRoundTrip(const void* frame, size_t frameSize,
                           const void* src, size_t srcSize,
                           const void* dict, size_t dictSize,
                           void* tmp)
{
    LZ4F_dctx* dctx;
    size_t dstSize = srcSize;
    size_t cSize = frameSize;
    size_t r;
    CHECK_LZ4F(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION));
    r = LZ4F_decompress_usingDict(dctx, tmp, &dstSize, frame, &cSize, dict, dictSize, NULL);
    CHECK_LZ4F(r);
    CHECK(r == 0, "frame not fully decoded");
    CHECK(cSize == frameSize, "frame size mismatch (%u != %u)", (unsigned)cSize, (unsigned)frameSize);
    CHECK(dstSize == srcSize, "decoded size mismatch (%u != %u)", (unsigned)dstSize, (unsigned)srcSize);
    if (memcmp(tmp, src, srcSize)) {
        size_t n = 0;
        while (((const char*)tmp)[n] == ((const char*)src)[n]) n++;
        dumpFile(".frame", frame, frameSize);
        dumpFile(".src", src, srcSize);
        dumpFile(".dict", dict, dictSize);
        CHECK(0, "decoded content differs, from position %u (frame size %u)", (unsigned)n, (unsigned)frameSize);
    }
    LZ4F_freeDecompressionContext(dctx);
}


/*-************************************
*  Validation
**************************************/
static int validate(XXH64_state_t* digest)
{
    static const size_t srcSizes[] = { 1 MB + 1, 4 MB - 1, 4 MB + 1, 9 MB + 12345, 17 MB + 3 };
    static const int levels[] = { -3, 0, 1, 2, 4, 9, 10, 12 };
    static const unsigned nbWorkersList[] = { 2, 3, 8, 1000 /* capped (tests/Makefile : LZ4F_NBWORKERS_MAX=128) */ };
    size_t const srcSizeMax = 17 MB + 3;
    size_t const dictSize = 64 KB;
    size_t const dstCapacity = LZ4F_compressFrameBound(srcSizeMax, NULL);
    char* const src = (char*)malloc(srcSizeMax);
    char* const dict = (char*)malloc(dictSize);
    char* const ref0 = (char*)malloc(dstCapacity);
    char* const ref1 = (char*)malloc(dstCapacity);
    char* const out = (char*)malloc(dstCapacity);
    char* const decoded = (char*)malloc(srcSizeMax);
    LZ4F_CustomMem const cmem = { countingAlloc, NULL, countingFree, NULL };
    LZ4F_cctx* const cctx = LZ4F_createCompressionContext_advanced(cmem, LZ4F_VERSION);
    LZ4F_CDict* cdict;   /* created once dict is filled */
    unsigned nbConfigs = 0;
    unsigned seed = 1;
    size_t si, li;
    int linked;
    g_step = "one-shot";

    CHECK(src && dict && ref0 && ref1 && out && decoded && cctx, "allocation failed");
    RDG_genBuffer(src, srcSizeMax, 0.5, 0.0, 7);
    RDG_genBuffer(dict, dictSize, 0.5, 0.0, 8);
    cdict = LZ4F_createCDict(dict, dictSize);
    CHECK(cdict != NULL, "allocation failed");

    for (si = 0; si < sizeof(srcSizes)/sizeof(srcSizes[0]); si++) {
      for (li = 0; li < sizeof(levels)/sizeof(levels[0]); li++) {
        for (linked = 0; linked <= 1; linked++) {
            size_t const srcSize = srcSizes[si];
            int const level = levels[li];
            LZ4F_preferences_t prefs;
            const LZ4F_CDict* cd;
            size_t c0, c1;
            size_t wi;

            if (level >= 10 && srcSize > 9 MB + 12345) continue;   /* keep test duration reasonable */
            if (g_quick && ((si == 1) || (si == 4) || (level == 0) || (level == 2) || (level == 4) || (level == 12))) continue;
            seed = seed * 1103515245u + 12345u;
            memset(&prefs, 0, sizeof(prefs));
            prefs.compressionLevel = level;
            prefs.frameInfo.blockMode = linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
            prefs.frameInfo.blockSizeID = (LZ4F_blockSizeID_t)(LZ4F_max64KB + (int)((seed >> 8) & 3));
            prefs.frameInfo.blockChecksumFlag = (LZ4F_blockChecksum_t)((seed >> 10) & 1);
            prefs.frameInfo.contentChecksumFlag = (LZ4F_contentChecksum_t)((seed >> 11) & 1);
            prefs.frameInfo.contentSize = (seed >> 12) & 1;
            prefs.favorDecSpeed = (seed >> 13) & 1;
            cd = ((seed >> 14) & 1) ? cdict : NULL;
            prefs.frameInfo.dictID = cd ? 1234 : 0;
            nbConfigs++;
            if (g_verbose)
                DISPLAY("srcSize=%8u level=%3i %s bsid=%i bcrc=%u ccrc=%u csize=%u fds=%u dict=%u \n",
                        (unsigned)srcSize, level, linked ? "linked" : "indep ",
                        (int)prefs.frameInfo.blockSizeID, prefs.frameInfo.blockChecksumFlag,
                        prefs.frameInfo.contentChecksumFlag, (unsigned)(prefs.frameInfo.contentSize!=0),
                        prefs.favorDecSpeed, cd!=NULL);

            /* reference : single-threaded */
            c0 = compressMT(cctx, ref0, dstCapacity, src, srcSize, cd, &prefs, 0);
            CHECK_LZ4F(c0);
            checkRoundTrip(ref0, c0, src, srcSize, cd ? dict : NULL, cd ? dictSize : 0, decoded);

            /* nbWorkers == 1 : one worker thread (serial, within the calling thread, without LZ4F_MULTITHREAD) */
            c1 = compressMT(cctx, ref1, dstCapacity, src, srcSize, cd, &prefs, 1);
            CHECK_LZ4F(c1);
            checkRoundTrip(ref1, c1, src, srcSize, cd ? dict : NULL, cd ? dictSize : 0, decoded);
            if (!linked)
                CHECK(c1 == c0 && !memcmp(ref1, ref0, c0), "independent blocks : output differs from single-threaded");
            XXH64_update(digest, ref1, c1);

            /* any nb of workers : same output */
            for (wi = 0; wi < sizeof(nbWorkersList)/sizeof(nbWorkersList[0]); wi++) {
                size_t const c = compressMT(cctx, out, dstCapacity, src, srcSize, cd, &prefs, nbWorkersList[wi]);
                CHECK_LZ4F(c);
                CHECK(c == c1 && !memcmp(out, ref1, c1), "output differs with %u workers", nbWorkersList[wi]);
            }

            /* LZ4F_compressFrame() : no context, no dictionary */
            if (cd == NULL) {
                LZ4F_preferences_t mtPrefs = prefs;
                size_t c;
                mtPrefs.nbWorkers = 8;
                c = LZ4F_compressFrame(out, dstCapacity, src, srcSize, &mtPrefs);
                CHECK_LZ4F(c);
                CHECK(c == c1 && !memcmp(out, ref1, c1), "LZ4F_compressFrame() : output differs");
            }

            /* dstCapacity too small for parallel jobs : serial, same output, exact capacity is enough */
            {   size_t const c = compressMT(cctx, out, c1, src, srcSize, cd, &prefs, 8);
                CHECK_LZ4F(c);
                CHECK(c == c1 && !memcmp(out, ref1, c1), "output differs with exact dstCapacity");
            }
            {   size_t const c = compressMT(cctx, out, c1 - 1, src, srcSize, cd, &prefs, 8);
                CHECK(LZ4F_isError(c), "dstCapacity too small : should have failed");
            }
    }   }   }

    LZ4F_freeCompressionContext(cctx);
    LZ4F_freeCDict(cdict);
    CHECK(g_nbAllocs == g_nbFrees, "memory leak : %u allocations, %u frees", (unsigned)g_nbAllocs, (unsigned)g_nbFrees);
    DISPLAY("one-shot : %u configurations validated (%u allocations) \n", nbConfigs, (unsigned)g_nbAllocs);

    free(src); free(dict); free(ref0); free(ref1); free(out); free(decoded);
    return 0;
}


/*-************************************
*  Streaming validation
**************************************/
/* when environment variable FRAMEMT_DUMP is set, mismatching frames are written to FRAMEMT_DUMP.{ref,out} */
static void dumpFrames(const char* ref, size_t refSize, const char* out, size_t outSize)
{
    const char* const prefix = getenv("FRAMEMT_DUMP");
    char name[1024];
    FILE* f;
    if (prefix == NULL) return;
    snprintf(name, sizeof(name), "%s.ref", prefix);
    f = fopen(name, "wb"); if (f) { fwrite(ref, 1, refSize, f); fclose(f); }
    snprintf(name, sizeof(name), "%s.out", prefix);
    f = fopen(name, "wb"); if (f) { fwrite(out, 1, outSize, f); fclose(f); }
}

static unsigned nextRand(unsigned* state)
{
    *state = *state * 1103515245u + 12345u;
    return *state >> 8;
}

typedef struct {
    unsigned stableSrc; /* LZ4F_stableSrc_t */
    int flushes;        /* LZ4F_flush() at random positions */
    int uncompressed;   /* some slices sent with LZ4F_uncompressedUpdate() */
} streamMode_t;

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

/* compresses @src as a stream, slices following @seed :
 * the same @seed produces the same slices, flushes and uncompressed segments, whatever nbWorkers.
 * Each invocation receives exactly LZ4F_compressBound() as dstCapacity.
 * LZ4F_srcStableUntilFlush : slices are read from a working copy of @src, in @scratch (>= @srcSize bytes),
 * scrambled after each flush (each invocation with autoFlush), except its last 64 KB.
 * Otherwise, slices are provided from @scratch, scrambled after each invocation.
 * With LZ4F_srcStable and nbWorkers >= 1, this is stricter than its contract (last 64 KB) :
 * jobs still running after an invocation returns must not read its input. */
static size_t streamCompress(LZ4F_cctx* cctx, char* dst,
                             const char* src, size_t srcSize,
                             const LZ4F_CDict* cdict, const LZ4F_preferences_t* prefs, unsigned nbWorkers,
                             streamMode_t mode, unsigned seed,
                             char* tmpOut, char* scratch)
{
    LZ4F_preferences_t p = *prefs;
    LZ4F_compressOptions_t cOpts;
    size_t const maxSlice = 3 MB;
    size_t pos = 0, consumed = 0;
    /* LZ4F_srcStable with nbWorkers == 0 : linked blocks reference the last 64 KB, which must remain */
    int const scrambled = (mode.stableSrc == LZ4F_srcUnstable)
                       || ((mode.stableSrc == LZ4F_srcStable) && (nbWorkers >= 1));
    int const untilFlush = (mode.stableSrc == LZ4F_srcStableUntilFlush);
    size_t scrambledEnd = 0;

    p.nbWorkers = nbWorkers;
    memset(&cOpts, 0, sizeof(cOpts));
    cOpts.stableSrc = mode.stableSrc;
    if (untilFlush) memcpy(scratch, src, srcSize);
    {   size_t const h = LZ4F_compressBegin_usingCDict(cctx, dst, LZ4F_HEADER_SIZE_MAX, cdict, &p);
        CHECK_LZ4F(h);
        pos += h;
    }
    while (consumed < srcSize) {
        unsigned const r = nextRand(&seed);
        size_t slice = (r & 7) ? (r % maxSlice) + 1 : (r % 100) + 1;   /* sometimes tiny */
        const char* in = (untilFlush ? scratch : src) + consumed;
        size_t cSize;
        int flushed = p.autoFlush;
        if (slice > srcSize - consumed) slice = srcSize - consumed;
        if (scrambled) {
            memcpy(scratch, in, slice);
            in = scratch;
        }
        if (mode.uncompressed && ((r >> 20) % 5 == 0)) {
            cSize = LZ4F_uncompressedUpdate(cctx, tmpOut, LZ4F_compressBound(slice, &p), in, slice, &cOpts);
        } else {
            cSize = LZ4F_compressUpdate(cctx, tmpOut, LZ4F_compressBound(slice, &p), in, slice, &cOpts);
        }
        CHECK_LZ4F(cSize);
        if (scrambled) memset(scratch, 0xA5, slice);   /* input must not be read after invocation */
        memcpy(dst + pos, tmpOut, cSize);
        pos += cSize;
        consumed += slice;
        if (mode.flushes && ((r >> 16) % 4 == 0)) {
            size_t const cap = LZ4F_compressBound(0, &p);
            size_t fSize;
            do {   /* multi-threaded : at least one job per invocation ; single-threaded : a single invocation */
                fSize = LZ4F_flush(cctx, tmpOut, cap, NULL);
                CHECK_LZ4F(fSize);
                CHECK((fSize < cap) || (nbWorkers && !p.autoFlush), "flush fills LZ4F_compressBound(0) in single-threaded mode");
                memcpy(dst + pos, tmpOut, fSize);
                pos += fSize;
            } while (fSize == cap);
            flushed = 1;
        }
        if (untilFlush && flushed) scrambleFlushed(scratch, &scrambledEnd, consumed, src, srcSize);
    }
    {   size_t const cap = LZ4F_compressBound(0, &p);
        size_t eSize;
        do {
            eSize = LZ4F_compressEnd(cctx, tmpOut, cap, NULL);
            CHECK_LZ4F(eSize);
            CHECK((eSize < cap) || (nbWorkers && !p.autoFlush), "compressEnd fills LZ4F_compressBound(0) in single-threaded mode");
            memcpy(dst + pos, tmpOut, eSize);
            pos += eSize;
        } while (eSize == cap);
    }
    return pos;
}

static int validateStreaming(XXH64_state_t* digest)
{
    static const int levels[] = { -2, 1, 2, 5, 9, 10 };
    static const unsigned nbWorkersList[] = { 1, 2, 4 };
    static const streamMode_t copyMode = { LZ4F_srcUnstable, 0, 0 };
    static const streamMode_t stableMode = { LZ4F_srcStable, 0, 0 };
    static const streamMode_t untilFlushMode = { LZ4F_srcStableUntilFlush, 0, 0 };
    static const unsigned flushStability[] = { LZ4F_srcStableUntilFlush, LZ4F_srcUnstable, LZ4F_srcStable };
    size_t const srcSize = 9 MB + 777;
    size_t const dictSize = 64 KB;
    size_t const dstCapacity = 2 * LZ4F_compressFrameBound(srcSize, NULL);
    char* const src = (char*)malloc(srcSize);
    char* const dict = (char*)malloc(dictSize);
    char* const scratch = (char*)malloc(srcSize);   /* slices, or working copy (LZ4F_srcStableUntilFlush) */
    char* const ref = (char*)malloc(dstCapacity);
    char* const refFlush = (char*)malloc(dstCapacity);
    char* const refAuto = (char*)malloc(dstCapacity);
    char* const refUnc = (char*)malloc(dstCapacity);
    char* const refRaw = (char*)malloc(dstCapacity);
    char* const out = (char*)malloc(dstCapacity);
    char* const decoded = (char*)malloc(srcSize);
    char* tmpOut;
    size_t tmpOutCapacity;
    LZ4F_CustomMem const cmem = { countingAlloc, NULL, countingFree, NULL };
    LZ4F_cctx* const cctx = LZ4F_createCompressionContext_advanced(cmem, LZ4F_VERSION);
    LZ4F_CDict* cdict;   /* created once dict is filled */
    unsigned seed = 77;
    unsigned nbConfigs = 0;
    size_t li;
    int linked;

    {   LZ4F_preferences_t worst = LZ4F_INIT_PREFERENCES;
        int bsid;
        worst.frameInfo.blockChecksumFlag = LZ4F_blockChecksumEnabled;
        worst.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        worst.nbWorkers = 4;
        tmpOutCapacity = 0;
        for (bsid = LZ4F_max64KB; bsid <= LZ4F_max4MB; bsid++) {
            size_t b;
            worst.frameInfo.blockSizeID = (LZ4F_blockSizeID_t)bsid;
            b = LZ4F_compressBound(3 MB, &worst);
            if (b > tmpOutCapacity) tmpOutCapacity = b;
        }
        tmpOut = (char*)malloc(tmpOutCapacity);
    }
    CHECK(src && dict && scratch && ref && refFlush && refAuto && refUnc && refRaw && out && decoded && tmpOut && cctx, "allocation failed");
    RDG_genBuffer(src, srcSize, 0.5, 0.0, 9);
    RDG_genBuffer(dict, dictSize, 0.5, 0.0, 8);
    cdict = LZ4F_createCDict(dict, dictSize);
    CHECK(cdict != NULL, "allocation failed");

    for (li = 0; li < sizeof(levels)/sizeof(levels[0]); li++) {
        for (linked = 0; linked <= 1; linked++) {
            LZ4F_preferences_t prefs;
            const LZ4F_CDict* cd;
            size_t oneShot, flushRef = 0, autoRef = 0, uncRef = 0, rawRef = 0;
            unsigned const sliceSeed = nextRand(&seed);
            size_t wi;

            if (g_quick && ((levels[li] == -2) || (levels[li] == 2) || (levels[li] == 5))) continue;
            seed = seed * 1103515245u + 12345u;
            memset(&prefs, 0, sizeof(prefs));
            prefs.compressionLevel = levels[li];
            prefs.frameInfo.blockMode = linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
            prefs.frameInfo.blockSizeID = (LZ4F_blockSizeID_t)(LZ4F_max64KB + (int)((seed >> 8) & 3));
            prefs.frameInfo.blockChecksumFlag = (LZ4F_blockChecksum_t)((seed >> 10) & 1);
            prefs.frameInfo.contentChecksumFlag = (LZ4F_contentChecksum_t)((seed >> 11) & 1);
            prefs.frameInfo.contentSize = ((seed >> 12) & 1) ? srcSize : 0;
            prefs.favorDecSpeed = (seed >> 13) & 1;
            cd = ((seed >> 14) & 1) ? cdict : NULL;
            nbConfigs++;
            if (g_verbose)
                DISPLAY("stream : level=%3i %s bsid=%i bcrc=%u ccrc=%u csize=%u dict=%u \n",
                        levels[li], linked ? "linked" : "indep ", (int)prefs.frameInfo.blockSizeID,
                        prefs.frameInfo.blockChecksumFlag, prefs.frameInfo.contentChecksumFlag,
                        (unsigned)(prefs.frameInfo.contentSize!=0), cd!=NULL);

            /* reference : one-shot, cut into jobs */
            oneShot = compressMT(cctx, ref, dstCapacity, src, srcSize, cd, &prefs, 1);
            CHECK_LZ4F(oneShot);
            XXH64_update(digest, ref, oneShot);

            for (wi = 0; wi < sizeof(nbWorkersList)/sizeof(nbWorkersList[0]); wi++) {
                unsigned const nbWorkers = nbWorkersList[wi];
                size_t c;

                /* without flush : same jobs as one-shot, whatever slicing and stableSrc */
                c = streamCompress(cctx, out, src, srcSize, cd, &prefs, nbWorkers, copyMode, sliceSeed, tmpOut, scratch);
                CHECK(c == oneShot && !memcmp(out, ref, c), "stream (copy, %u workers) differs from one-shot", nbWorkers);
                c = streamCompress(cctx, out, src, srcSize, cd, &prefs, nbWorkers, stableMode, sliceSeed + 1, tmpOut, scratch);
                CHECK(c == oneShot && !memcmp(out, ref, c), "stream (srcStable, %u workers) differs from one-shot", nbWorkers);
                c = streamCompress(cctx, out, src, srcSize, cd, &prefs, nbWorkers, untilFlushMode, sliceSeed + 1, tmpOut, scratch);
                CHECK(c == oneShot && !memcmp(out, ref, c), "stream (srcStableUntilFlush, %u workers) differs from one-shot", nbWorkers);

                /* raw dictionary : same output whatever nbWorkers ; HC levels >= 3 : same frame as its CDict */
                if (cd) {
                    c = compressRawDict(cctx, out, dstCapacity, src, srcSize, dict, dictSize, &prefs, nbWorkers);
                    CHECK_LZ4F(c);
                    if (wi == 0) {
                        rawRef = c;
                        memcpy(refRaw, out, c);
                        XXH64_update(digest, out, c);
                        checkRoundTrip(out, c, src, srcSize, dict, dictSize, decoded);
                        if (prefs.compressionLevel > LZ4HC_CLEVEL_MIN)   /* level 2 : digested in its own format */
                            CHECK(c == oneShot && !memcmp(out, ref, c), "raw dictionary differs from CDict");
                        if (!linked) {   /* independent blocks : same as single-threaded */
                            size_t const c0 = compressRawDict(cctx, out, dstCapacity, src, srcSize, dict, dictSize, &prefs, 0);
                            CHECK(c0 == rawRef && !memcmp(out, refRaw, c0), "raw dictionary differs from single-threaded");
                        }
                    } else {
                        CHECK(c == rawRef && !memcmp(out, refRaw, c), "raw dictionary differs with %u workers", nbWorkers);
                    }
                }

                /* with flushes : same output whatever nbWorkers */
                {   static char step[64];
                    snprintf(step, sizeof(step), "stream with flushes, %u workers", nbWorkers);
                    g_step = step;
                }
                {   streamMode_t const flushMode = { flushStability[wi % 3], 1, 0 };
                    c = streamCompress(cctx, out, src, srcSize, cd, &prefs, nbWorkers, flushMode, sliceSeed + 2, tmpOut, scratch);
                    checkRoundTrip(out, c, src, srcSize, cd ? dict : NULL, cd ? dictSize : 0, decoded);
                    if (wi == 0) {
                        flushRef = c;
                        memcpy(refFlush, out, c);
                        XXH64_update(digest, out, c);
                        {   size_t const c0 = streamCompress(cctx, out, src, srcSize, cd, &prefs, 0, flushMode, sliceSeed + 2, tmpOut, scratch);
                            if (!linked) {   /* independent blocks : same as single-threaded */
                                CHECK(c0 == flushRef && !memcmp(out, refFlush, c0), "stream with flushes differs from single-threaded");
                            } else {
                                checkRoundTrip(out, c0, src, srcSize, cd ? dict : NULL, cd ? dictSize : 0, decoded);
                            }
                        }
                    } else {
                        if (c != flushRef || memcmp(out, refFlush, c)) dumpFrames(refFlush, flushRef, out, c);
                        CHECK(c == flushRef && !memcmp(out, refFlush, c), "stream with flushes differs with %u workers", nbWorkers);
                    }
                }

                /* autoFlush : same output whatever nbWorkers */
                g_step = "autoFlush stream";
                {   LZ4F_preferences_t autoPrefs = prefs;
                    streamMode_t const autoMode = { flushStability[wi % 3], 0, 0 };
                    autoPrefs.autoFlush = 1;
                    c = streamCompress(cctx, out, src, srcSize, cd, &autoPrefs, nbWorkers, autoMode, sliceSeed + 3, tmpOut, scratch);
                    checkRoundTrip(out, c, src, srcSize, cd ? dict : NULL, cd ? dictSize : 0, decoded);
                    if (wi == 0) {
                        autoRef = c;
                        memcpy(refAuto, out, c);
                        XXH64_update(digest, out, c);
                        if (!linked) {
                            size_t const c0 = streamCompress(cctx, out, src, srcSize, cd, &autoPrefs, 0, autoMode, sliceSeed + 3, tmpOut, scratch);
                            CHECK(c0 == autoRef && !memcmp(out, refAuto, c0), "autoFlush stream differs from single-threaded");
                        }
                    } else {
                        CHECK(c == autoRef && !memcmp(out, refAuto, c), "autoFlush stream differs with %u workers", nbWorkers);
                    }
                }

                /* uncompressed segments : same output whatever nbWorkers.
                 * Linked blocks : compressed blocks reference preceding uncompressed data in multi-threaded mode only */
                g_step = "stream with uncompressed segments";
                {   streamMode_t const uncMode = { LZ4F_srcUnstable, 1, 1 };
                    c = streamCompress(cctx, out, src, srcSize, cd, &prefs, nbWorkers, uncMode, sliceSeed + 4, tmpOut, scratch);
                    checkRoundTrip(out, c, src, srcSize, cd ? dict : NULL, cd ? dictSize : 0, decoded);
                    if (wi == 0) {
                        uncRef = c;
                        memcpy(refUnc, out, c);
                        XXH64_update(digest, out, c);
                        {   size_t const c0 = streamCompress(cctx, out, src, srcSize, cd, &prefs, 0, uncMode, sliceSeed + 4, tmpOut, scratch);
                            if (!linked) {
                                CHECK(c0 == uncRef && !memcmp(out, refUnc, c0), "stream with uncompressed segments differs from single-threaded");
                            } else {
                                checkRoundTrip(out, c0, src, srcSize, cd ? dict : NULL, cd ? dictSize : 0, decoded);
                            }
                        }
                    } else {
                        CHECK(c == uncRef && !memcmp(out, refUnc, c), "stream with uncompressed segments differs with %u workers", nbWorkers);
                    }
                }
            }
    }   }

    /* abandoned frames : jobs in flight when a new frame starts, or when cctx is freed */
    {   LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        LZ4F_cctx* cctx2;
        prefs.nbWorkers = 4;
        prefs.compressionLevel = 9;
        CHECK_LZ4F(LZ4F_compressBegin(cctx, out, dstCapacity, &prefs));
        CHECK_LZ4F(LZ4F_compressUpdate(cctx, out, dstCapacity, src, srcSize, NULL));
        {   size_t const c = compressMT(cctx, out, dstCapacity, src, srcSize, NULL, &prefs, 3);
            CHECK_LZ4F(c);
            checkRoundTrip(out, c, src, srcSize, NULL, 0, decoded);
        }
        cctx2 = LZ4F_createCompressionContext_advanced(cmem, LZ4F_VERSION);
        CHECK(cctx2 != NULL, "allocation failed");
        CHECK_LZ4F(LZ4F_compressBegin(cctx2, out, dstCapacity, &prefs));
        CHECK_LZ4F(LZ4F_compressUpdate(cctx2, out, dstCapacity, src, srcSize, NULL));
        LZ4F_freeCompressionContext(cctx2);
    }

    LZ4F_freeCompressionContext(cctx);
    LZ4F_freeCDict(cdict);
    CHECK(g_nbAllocs == g_nbFrees, "memory leak : %u allocations, %u frees", (unsigned)g_nbAllocs, (unsigned)g_nbFrees);
    DISPLAY("streaming : %u configurations validated \n", nbConfigs);
    free(src); free(dict); free(scratch); free(ref); free(refFlush); free(refAuto); free(refUnc); free(refRaw); free(out); free(decoded); free(tmpOut);
    return 0;
}


/*-************************************
*  Allocation failures
**************************************/
/* fails the N-th allocation (g_failAt), counting from 1 ; 0 : never */
static size_t g_failAt = 0;
static size_t g_allocNb = 0;

static void* failingAlloc(void* opaque, size_t size)
{
    (void)opaque;
    if (++g_allocNb == g_failAt) return NULL;
    g_nbAllocs++;
    return malloc(size);
}

/* compresses @src : one-shot (mode 0), or streaming, copying input (mode 1), or read in place (mode 2), with a flush.
 * @return : frame size, or the first error */
static size_t compressScenario(LZ4F_cctx* cctx, int mode, const LZ4F_preferences_t* prefs,
                               char* dst, size_t dstCapacity,
                               const char* src, size_t srcSize, char* scratch)
{
    size_t const slice = 1 MB + 777;
    LZ4F_compressOptions_t cOpts;
    size_t pos, consumed = 0, r;
    int flushed = 0;
    if (mode == 0) return LZ4F_compressFrame_usingCDict(cctx, dst, dstCapacity, src, srcSize, NULL, prefs);
    memset(&cOpts, 0, sizeof(cOpts));
    cOpts.stableSrc = (mode == 2) ? LZ4F_srcStableUntilFlush : LZ4F_srcUnstable;
    r = LZ4F_compressBegin(cctx, dst, dstCapacity, prefs);
    if (LZ4F_isError(r)) return r;
    pos = r;
    while (consumed < srcSize) {
        size_t const size = MIN(slice, srcSize - consumed);
        const char* in = src + consumed;
        if (mode == 1) { memcpy(scratch, in, size); in = scratch; }
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
static int validateAllocFailures(void)
{
    static const unsigned nbWorkersList[] = { 1, 4 };
    static const int levels[] = { 1, 9 };
    size_t const srcSize = 5 MB + 333;
    size_t const dstCapacity = 2 * LZ4F_compressFrameBound(srcSize, NULL);
    char* const src = (char*)malloc(srcSize);
    char* const scratch = (char*)malloc(1 MB + 777);
    char* const ref = (char*)malloc(dstCapacity);
    char* const out = (char*)malloc(dstCapacity);
    LZ4F_CustomMem const failingMem = { failingAlloc, NULL, countingFree, NULL };
    unsigned nbScenarios = 0, nbFailures = 0, nbAbsorbed = 0;
    size_t wi, li;
    int linked, mode;

    CHECK(src && scratch && ref && out, "allocation failed");
    RDG_genBuffer(src, srcSize, 0.5, 0.0, 11);
    for (li = 0; li < sizeof(levels)/sizeof(levels[0]); li++)
    for (wi = 0; wi < sizeof(nbWorkersList)/sizeof(nbWorkersList[0]); wi++)
    for (linked = 0; linked <= 1; linked++)
    for (mode = 0; mode <= 2; mode++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        LZ4F_cctx* cctx;
        size_t refSize, maxN, n;
        if (g_quick && ((levels[li] != 1) || (nbWorkersList[wi] != 4))) continue;
        prefs.compressionLevel = levels[li];
        prefs.nbWorkers = nbWorkersList[wi];
        prefs.frameInfo.blockMode = linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
        prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        nbScenarios++;

        /* reference, and nb of allocations */
        g_failAt = 0;
        g_allocNb = 0;
        cctx = LZ4F_createCompressionContext_advanced(failingMem, LZ4F_VERSION);
        CHECK(cctx != NULL, "allocation failed");
        refSize = compressScenario(cctx, mode, &prefs, ref, dstCapacity, src, srcSize, scratch);
        CHECK_LZ4F(refSize);
        LZ4F_freeCompressionContext(cctx);
        maxN = g_allocNb;   /* can vary slightly with scheduling : try a few more */

        for (n = 1; n <= maxN + 2; n++) {
            size_t r;
            g_allocNb = 0;
            g_failAt = n;
            cctx = LZ4F_createCompressionContext_advanced(failingMem, LZ4F_VERSION);
            if (cctx == NULL) {
                CHECK(n == 1, "context creation failed at allocation %u", (unsigned)n);
                continue;
            }
            r = compressScenario(cctx, mode, &prefs, out, dstCapacity, src, srcSize, scratch);
            if (LZ4F_isError(r)) {
                CHECK(LZ4F_getErrorCode(r) == LZ4F_ERROR_allocation_failed,
                      "allocation %u : unexpected error %s", (unsigned)n, LZ4F_getErrorName(r));
                nbFailures++;
            } else {   /* additional worker thread not started, or this run needed fewer allocations (scheduling) */
                CHECK(r == refSize && !memcmp(out, ref, r), "allocation %u : output differs", (unsigned)n);
                if (g_allocNb >= n) nbAbsorbed++;
            }
            /* context remains usable */
            g_failAt = 0;
            r = compressScenario(cctx, mode, &prefs, out, dstCapacity, src, srcSize, scratch);
            CHECK(!LZ4F_isError(r) && (r == refSize) && !memcmp(out, ref, r),
                  "allocation %u : context not reusable", (unsigned)n);
            LZ4F_freeCompressionContext(cctx);
            CHECK(g_nbAllocs == g_nbFrees, "allocation %u : memory leak (%u allocations, %u frees)",
                  (unsigned)n, (unsigned)g_nbAllocs, (unsigned)g_nbFrees);
        }
    }
    g_failAt = 0;
    DISPLAY("allocation failures : %u scenarios, %u failures reported, %u fewer worker threads \n", nbScenarios, nbFailures, nbAbsorbed);
    free(src); free(scratch); free(ref); free(out);
    return 0;
}

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
static int validateDstTooSmall(void)
{
    static const unsigned nbWorkersList[] = { 0, 1, 3 };   /* 1 : job slots fill up within LZ4F_compressUpdate() */
    size_t const srcSize = 13 MB + 50 KB;   /* incomplete last job, and last block */
    size_t const smallCapacity = 1 KB;
    size_t const dstCapacity = 2 * LZ4F_compressFrameBound(srcSize, NULL);
    char* const src = (char*)malloc(srcSize);
    char* const ref = (char*)malloc(dstCapacity);
    char* const out = (char*)malloc(dstCapacity);
    LZ4F_CustomMem const countingMem = { countingAlloc, NULL, countingFree, NULL };
    unsigned nbConfigs = 0, nbFailures = 0, nbPartial = 0;
    size_t wi;
    int linked, autoFlush, smallFrom;

    g_step = "dstCapacity too small";
    CHECK(src && ref && out, "allocation failed");
    RDG_genBuffer(src, srcSize, 0.5, 0.0, 19);
    for (wi = 0; wi < sizeof(nbWorkersList)/sizeof(nbWorkersList[0]); wi++)
    for (linked = 0; linked <= 1; linked++)
    for (autoFlush = 0; autoFlush <= 1; autoFlush++)
    for (smallFrom = 0; smallFrom <= 2; smallFrom++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        /* compressUpdate() must write output into too little capacity ; otherwise, it can defer it (jobs) */
        int const mustFail = (smallFrom == 0) && (autoFlush || (nbWorkersList[wi] == 0));
        LZ4F_cctx* cctx;
        size_t refSize, r;
        int failedOp;
        if (g_quick && (smallFrom == 1)) continue;
        cctx = LZ4F_createCompressionContext_advanced(countingMem, LZ4F_VERSION);
        CHECK(cctx != NULL, "allocation failed");
        prefs.compressionLevel = 1;
        prefs.nbWorkers = nbWorkersList[wi];
        prefs.autoFlush = (unsigned)autoFlush;
        prefs.frameInfo.blockMode = linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
        prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        refSize = streamOps(cctx, &prefs, ref, dstCapacity, src, srcSize, 3, 0, &failedOp);

        CHECK_LZ4F(refSize);

        r = streamOps(cctx, &prefs, out, dstCapacity, src, srcSize, smallFrom, smallCapacity, &failedOp);
        if (!LZ4F_isError(r)) {
            CHECK(!mustFail, "%u workers, smallFrom=%i : should have failed", prefs.nbWorkers, smallFrom);
            CHECK(r == refSize && !memcmp(out, ref, r), "%u workers, smallFrom=%i : output differs", prefs.nbWorkers, smallFrom);
            nbPartial++;
        } else {
            CHECK(LZ4F_getErrorCode(r) == LZ4F_ERROR_dstCapacity_tooSmall,
                  "%u workers : unexpected error %s", prefs.nbWorkers, LZ4F_getErrorName(r));
            CHECK(failedOp == 0, "%u workers : %s failed", prefs.nbWorkers, (failedOp == 1) ? "flush" : "end");
            CHECK(smallFrom == 0, "%u workers : failed with enough capacity", prefs.nbWorkers);
            nbFailures++;
            /* frame invalidated */
            r = LZ4F_compressUpdate(cctx, out, dstCapacity, src, 1 KB, NULL);
            CHECK(LZ4F_getErrorCode(r) == LZ4F_ERROR_compressionState_uninitialized,
                  "%u workers : update after failure : %s", prefs.nbWorkers, LZ4F_getErrorName(r));
            r = LZ4F_flush(cctx, out, dstCapacity, NULL);
            CHECK(LZ4F_getErrorCode(r) == LZ4F_ERROR_compressionState_uninitialized,
                  "%u workers : flush after failure : %s", prefs.nbWorkers, LZ4F_getErrorName(r));
            r = LZ4F_compressEnd(cctx, out, dstCapacity, NULL);
            CHECK(LZ4F_getErrorCode(r) == LZ4F_ERROR_compressionState_uninitialized,
                  "%u workers : end after failure : %s", prefs.nbWorkers, LZ4F_getErrorName(r));
        }
        /* context remains usable */
        r = streamOps(cctx, &prefs, out, dstCapacity, src, srcSize, 3, 0, &failedOp);
        CHECK(r == refSize && !memcmp(out, ref, r), "%u workers : next frame differs", prefs.nbWorkers);
        LZ4F_freeCompressionContext(cctx);
        CHECK(g_nbAllocs == g_nbFrees, "memory leak (%u allocations, %u frees)", (unsigned)g_nbAllocs, (unsigned)g_nbFrees);
        nbConfigs++;
    }
    DISPLAY("dstCapacity too small : %u configurations validated, %u compressUpdate failures, %u frames in pieces \n",
            nbConfigs, nbFailures, nbPartial);
    free(src); free(ref); free(out);
    return 0;
}

/* invokes LZ4F_flush() (@end == 0) or LZ4F_compressEnd() with @capacity, until it doesn't fill it,
 * or up to @maxCalls invocations (0 : no limit).
 * @return : nb of bytes written into @dst, or an error code */
static size_t flushPieces(LZ4F_cctx* cctx, int end, char* dst, size_t capacity, unsigned maxCalls, unsigned* nbCalls)
{
    size_t pos = 0;
    unsigned n = 0;
    if (nbCalls) *nbCalls = 0;
    for (;;) {
        size_t const r = end ? LZ4F_compressEnd(cctx, dst + pos, capacity, NULL)
                             : LZ4F_flush(cctx, dst + pos, capacity, NULL);
        if (LZ4F_isError(r)) return r;
        pos += r;
        n++;
        if (r < capacity) break;
        if (maxCalls && (n >= maxCalls)) break;
    }
    if (nbCalls) *nbCalls = n;
    return pos;
}

/* reference : begin, update(@part1), flush, then (@uncSize > 0) uncompressedUpdate(@uncSize), update(rest), end,
 * each invocation receiving its bound. Position and size of flush output are reported. */
static size_t partialRef(LZ4F_cctx* cctx, const LZ4F_preferences_t* p, const LZ4F_compressOptions_t* cOpts,
                         char* dst, const char* src, size_t srcSize,
                         size_t part1, size_t uncSize, size_t* flushPos, size_t* flushSize)
{
    size_t pos, r;
    pos = LZ4F_compressBegin(cctx, dst, LZ4F_HEADER_SIZE_MAX, p);
    CHECK_LZ4F(pos);
    r = LZ4F_compressUpdate(cctx, dst + pos, LZ4F_compressBound(part1, p), src, part1, cOpts);
    CHECK_LZ4F(r); pos += r;
    *flushPos = pos;
    r = flushPieces(cctx, 0, dst + pos, LZ4F_compressBound(0, p), 0, NULL);
    CHECK_LZ4F(r); pos += r;
    *flushSize = r;
    if (uncSize) {
        r = LZ4F_uncompressedUpdate(cctx, dst + pos, LZ4F_compressBound(uncSize, p), src + part1, uncSize, cOpts);
        CHECK_LZ4F(r); pos += r;
    }
    r = LZ4F_compressUpdate(cctx, dst + pos, LZ4F_compressBound(srcSize - part1 - uncSize, p),
                            src + part1 + uncSize, srcSize - part1 - uncSize, cOpts);
    CHECK_LZ4F(r); pos += r;
    r = flushPieces(cctx, 1, dst + pos, LZ4F_compressBound(0, p), 0, NULL);
    CHECK_LZ4F(r);
    return pos + r;
}

/* LZ4F_flush() and LZ4F_compressEnd() fill dstBuffer, or complete :
 * output produced in pieces is identical to the reference, for any capacity, even 1 byte.
 * - the first invocation of a flush sets the flush point : input is no longer read (LZ4F_srcStableUntilFlush) ;
 * - capacity exactly equal to the remaining output : the next invocation returns 0 ;
 * - compressUpdate() or uncompressedUpdate() after an incomplete flush first emit the remaining output ;
 * - compressBegin() abandons an incomplete end ;
 * - capacity 0 is rejected when output remains, without modifying the frame. */
static int validatePartialFlush(void)
{
    static const unsigned nbWorkersList[] = { 0, 1, 3 };
    static const size_t capacities[] = { 1, 7, 4 KB + 1, 1 MB - 3 };
    size_t const srcSize = 9 MB + 123;
    size_t const part1 = 5 MB + 7;   /* jobs in flight at flush point */
    size_t const uncSize = 64 KB + 5;
    size_t const dstCapacity = 2 * LZ4F_compressFrameBound(srcSize, NULL) + 64 MB;
    char* const src = (char*)malloc(srcSize);
    char* const work = (char*)malloc(srcSize);
    char* const ref = (char*)malloc(dstCapacity);
    char* const refFull = (char*)malloc(dstCapacity);
    char* const out = (char*)malloc(dstCapacity);
    LZ4F_CustomMem const countingMem = { countingAlloc, NULL, countingFree, NULL };
    unsigned nbConfigs = 0, nbCalls = 0;
    size_t wi, ci;
    int linked, variant;

    g_step = "partial flush";
    CHECK(src && work && ref && refFull && out, "allocation failed");
    RDG_genBuffer(src, srcSize, 0.5, 0.0, 31);
    for (wi = 0; wi < sizeof(nbWorkersList)/sizeof(nbWorkersList[0]); wi++)
    for (linked = 0; linked <= 1; linked++) {
        LZ4F_preferences_t p = LZ4F_INIT_PREFERENCES;
        LZ4F_cctx* const cctx = LZ4F_createCompressionContext_advanced(countingMem, LZ4F_VERSION);
        size_t refSize, refFullSize, flushPos, flushSize;
        CHECK(cctx != NULL, "allocation failed");
        p.nbWorkers = nbWorkersList[wi];
        p.frameInfo.blockMode = linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
        p.frameInfo.blockChecksumFlag = (LZ4F_blockChecksum_t)linked;
        p.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        {   LZ4F_compressOptions_t refOpts;
            size_t r;
            memset(&refOpts, 0, sizeof(refOpts));
            refOpts.stableSrc = LZ4F_srcStableUntilFlush;   /* single-threaded linked output depends on stableSrc */
            refSize = partialRef(cctx, &p, &refOpts, ref, src, srcSize, part1, uncSize, &flushPos, &flushSize);
            refFullSize = LZ4F_compressBegin(cctx, refFull, LZ4F_HEADER_SIZE_MAX, &p);
            CHECK_LZ4F(refFullSize);
            r = LZ4F_compressUpdate(cctx, refFull + refFullSize, LZ4F_compressBound(srcSize, &p), src, srcSize, NULL);
            CHECK_LZ4F(r); refFullSize += r;
            r = flushPieces(cctx, 1, refFull + refFullSize, LZ4F_compressBound(0, &p), 0, NULL);
            CHECK_LZ4F(r); refFullSize += r;
        }

        for (ci = 0; ci < sizeof(capacities)/sizeof(capacities[0]); ci++)
        for (variant = 0; variant < 5; variant++) {
            size_t const cap = capacities[ci];
            LZ4F_compressOptions_t cOpts;
            size_t pos, r, flushRemaining;
            unsigned n = 0;
            if (g_quick && (ci == 1)) continue;
            if ((variant >= 2) && (ci != 2)) continue;   /* one capacity is enough */
            memset(&cOpts, 0, sizeof(cOpts));
            cOpts.stableSrc = LZ4F_srcStableUntilFlush;
            memcpy(work, src, srcSize);
            pos = LZ4F_compressBegin(cctx, out, LZ4F_HEADER_SIZE_MAX, &p);
            CHECK_LZ4F(pos);
            r = LZ4F_compressUpdate(cctx, out + pos, LZ4F_compressBound(part1, &p), work, part1, &cOpts);
            CHECK_LZ4F(r); pos += r;
            /* output split between update and flush depends on scheduling (jobs) */
            CHECK((pos <= flushPos + flushSize) && !memcmp(out, ref, pos), "update output differs");
            flushRemaining = flushPos + flushSize - pos;
            if (variant == 4) {
                r = LZ4F_flush(cctx, out + pos, 0, NULL);
                CHECK(LZ4F_getErrorCode(r) == LZ4F_ERROR_dstCapacity_tooSmall, "flush into 0 byte : %s",
                      LZ4F_isError(r) ? LZ4F_getErrorName(r) : "should have been rejected");
            }
            if (variant == 1) {
                /* capacity exactly equal to remaining output */
                r = LZ4F_flush(cctx, out + pos, flushRemaining, NULL);
                CHECK(r == flushRemaining, "flush into exact capacity : %s", LZ4F_isError(r) ? LZ4F_getErrorName(r) : "wrong size");
                r = LZ4F_flush(cctx, out + pos + flushRemaining, cap, NULL);
                CHECK(r == 0, "flush after exact fill : %s", LZ4F_isError(r) ? LZ4F_getErrorName(r) : "should be empty");
                r = flushRemaining;
            } else {
                r = flushPieces(cctx, 0, out + pos, cap, (variant == 2) ? 1 : 0, &n);
                CHECK_LZ4F(r);
                nbCalls += n;
            }
            memset(work, 0xA5, part1 - 64 KB);   /* flush point passed : input no longer read, except the last 64 KB (history) */
            if (variant == 2) {
                /* update before the flush completes : remaining flush output comes first */
                size_t const flushed = r;
                CHECK(flushed <= flushRemaining, "flush output beyond reference");
                r = LZ4F_uncompressedUpdate(cctx, out + pos + flushed, LZ4F_compressBound(uncSize, &p), work + part1, uncSize, &cOpts);
                CHECK_LZ4F(r);
                r += flushed;
                /* uncompressed blocks follow all compressed ones : no more output pending */
            } else {
                CHECK(r == flushRemaining && !memcmp(out + pos, ref + pos, r), "%u workers, capacity %u : flush output differs",
                      p.nbWorkers, (unsigned)cap);
                {   size_t const u = LZ4F_uncompressedUpdate(cctx, out + pos + r, LZ4F_compressBound(uncSize, &p), work + part1, uncSize, &cOpts);
                    CHECK_LZ4F(u);
                    r += u;
                }
            }
            pos += r;
            r = LZ4F_compressUpdate(cctx, out + pos, LZ4F_compressBound(srcSize - part1 - uncSize, &p),
                                    work + part1 + uncSize, srcSize - part1 - uncSize, &cOpts);
            CHECK_LZ4F(r); pos += r;
            if (variant == 3) {
                /* compressBegin() abandons an incomplete end */
                r = flushPieces(cctx, 1, out + pos, cap, 1, NULL);
                CHECK_LZ4F(r);
                pos = LZ4F_compressBegin(cctx, out, LZ4F_HEADER_SIZE_MAX, &p);
                CHECK_LZ4F(pos);
                r = LZ4F_compressUpdate(cctx, out + pos, LZ4F_compressBound(srcSize, &p), src, srcSize, NULL);
                CHECK_LZ4F(r); pos += r;
                r = flushPieces(cctx, 1, out + pos, LZ4F_compressBound(0, &p), 0, NULL);
                CHECK_LZ4F(r); pos += r;
                CHECK(pos == refFullSize && !memcmp(out, refFull, pos), "%u workers : frame after abandoned end differs", p.nbWorkers);
                nbConfigs++;
                continue;
            }
            if (variant == 4) {
                r = LZ4F_compressEnd(cctx, out + pos, 0, NULL);
                CHECK(LZ4F_getErrorCode(r) == LZ4F_ERROR_dstCapacity_tooSmall, "end into 0 byte : %s",
                      LZ4F_isError(r) ? LZ4F_getErrorName(r) : "should have been rejected");
            }
            if (variant == 1) {
                size_t const endSize = refSize - pos;   /* output split between update and end depends on scheduling */
                r = LZ4F_compressEnd(cctx, out + pos, endSize, NULL);
                CHECK(r == endSize, "end into exact capacity : %s", LZ4F_isError(r) ? LZ4F_getErrorName(r) : "wrong size");
                r = LZ4F_compressEnd(cctx, out + pos + endSize, cap, NULL);
                CHECK(r == 0, "end after exact fill : %s", LZ4F_isError(r) ? LZ4F_getErrorName(r) : "should be empty");
                r = endSize;
            } else {
                r = flushPieces(cctx, 1, out + pos, cap, 0, &n);
                CHECK_LZ4F(r);
                nbCalls += n;
            }
            pos += r;
            CHECK(pos == refSize && !memcmp(out, ref, pos), "%u workers, linked=%i, capacity %u, variant %i : frame differs",
                  p.nbWorkers, linked, (unsigned)cap, variant);
            r = LZ4F_compressEnd(cctx, out, cap, NULL);   /* completed : the frame is over */
            CHECK(LZ4F_getErrorCode(r) == LZ4F_ERROR_compressionState_uninitialized, "end after completed end : %s",
                  LZ4F_isError(r) ? LZ4F_getErrorName(r) : "should have failed");
            nbConfigs++;
        }
        LZ4F_freeCompressionContext(cctx);
        CHECK(g_nbAllocs == g_nbFrees, "memory leak (%u allocations, %u frees)", (unsigned)g_nbAllocs, (unsigned)g_nbFrees);
    }
    DISPLAY("partial flush : %u configurations validated (%u invocations) \n", nbConfigs, nbCalls);
    free(src); free(work); free(ref); free(refFull); free(out);
    return 0;
}

/* Without autoFlush, completed jobs are only emitted when they fit :
 * a dstCapacity too small for them doesn't fail, as long as no job must be emitted (free job slot).
 * Deterministic without worker threads (jobs complete within LZ4F_compressUpdate()), otherwise depends on scheduling. */
static int validateDeferredEmission(void)
{
    static const unsigned nbWorkersList[] = { 1, 3 };
    size_t const maxJobSize = 1 MB;   /* jobs (64 KB blocks) */
    size_t const smallCapacity = 1 KB;
    size_t const dstCapacity = 2 * LZ4F_compressFrameBound(maxJobSize, NULL);
    char* const src = (char*)malloc(maxJobSize);
    char* const ref = (char*)malloc(dstCapacity);
    char* const out = (char*)malloc(dstCapacity);
    unsigned nbConfigs = 0, nbDeferred = 0;
    size_t wi;
    int linked;

    g_step = "deferred emission";
    CHECK(src && ref && out, "allocation failed");
    RDG_genBuffer(src, maxJobSize, 0.5, 0.0, 29);
    for (wi = 0; wi < sizeof(nbWorkersList)/sizeof(nbWorkersList[0]); wi++)
    for (linked = 0; linked <= 1; linked++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        size_t const jobSize = maxJobSize;   /* a single job, submitted once full */
        LZ4F_cctx* cctx = NULL;
        size_t refSize, pos, r;
#if !(defined(LZ4F_MULTITHREAD) && LZ4F_MULTITHREAD)
        if (!linked) continue;   /* independent blocks : single-threaded mode, writing blocks immediately */
#endif
        prefs.nbWorkers = nbWorkersList[wi];
        prefs.frameInfo.blockMode = linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
        CHECK_LZ4F(LZ4F_createCompressionContext(&cctx, LZ4F_VERSION));
        refSize = LZ4F_compressBegin(cctx, ref, dstCapacity, &prefs);
        CHECK_LZ4F(refSize);
        r = LZ4F_compressUpdate(cctx, ref + refSize, dstCapacity - refSize, src, jobSize, NULL);
        CHECK_LZ4F(r); refSize += r;
        r = LZ4F_compressEnd(cctx, ref + refSize, dstCapacity - refSize, NULL);
        CHECK_LZ4F(r); refSize += r;

        pos = LZ4F_compressBegin(cctx, out, dstCapacity, &prefs);
        CHECK_LZ4F(pos);
        r = LZ4F_compressUpdate(cctx, out + pos, smallCapacity, src, jobSize, NULL);
        CHECK(r == 0, "%u workers, linked=%i : %s", prefs.nbWorkers, linked, LZ4F_isError(r) ? LZ4F_getErrorName(r) : "job emitted");
        {   TIME_t const start = TIME_getTime();   /* meanwhile, the job completes */
            while (TIME_clockSpan_ns(start) < 100000000ULL) {
                r = LZ4F_compressUpdate(cctx, out + pos, smallCapacity, NULL, 0, NULL);
                CHECK(r == 0, "%u workers, linked=%i : %s", prefs.nbWorkers, linked, LZ4F_isError(r) ? LZ4F_getErrorName(r) : "job emitted");
        }   }
        r = LZ4F_compressUpdate(cctx, out + pos, dstCapacity - pos, NULL, 0, NULL);
        CHECK_LZ4F(r); pos += r;
        if (r) nbDeferred++;   /* the completed job was deferred */
        r = LZ4F_compressEnd(cctx, out + pos, dstCapacity - pos, NULL);
        CHECK_LZ4F(r); pos += r;
        CHECK(pos == refSize && !memcmp(out, ref, pos), "%u workers, linked=%i : output differs", prefs.nbWorkers, linked);
        LZ4F_freeCompressionContext(cctx);
        nbConfigs++;
    }
    DISPLAY("deferred emission : %u configurations validated, %u completed jobs deferred \n", nbConfigs, nbDeferred);
    free(src); free(ref); free(out);
    return 0;
}

/* LZ4F_compressBegin*() validates its arguments first : rejecting them leaves a frame in progress untouched,
 * including its jobs in flight, still reading input in place (LZ4F_srcStableUntilFlush). Same contract as nbWorkers == 0.
 * Other failures (allocation) invalidate the frame in progress : next operations fail with compressionState_uninitialized. */
static int validateBeginFailures(void)
{
    static const unsigned nbWorkersList[] = { 0, 1, 3 };
    size_t const srcSize = 6 MB + 123;
    size_t const firstPart = 2 MB + 5;   /* jobs in flight */
    size_t const nextSrcSize = 2 MB + 77;
    size_t const dstCapacity = 2 * LZ4F_compressFrameBound(srcSize, NULL);
    char* const src = (char*)malloc(srcSize);
    char* const ref = (char*)malloc(dstCapacity);
    char* const nextRef = (char*)malloc(dstCapacity);
    char* const out = (char*)malloc(dstCapacity);
    char header[LZ4F_HEADER_SIZE_MAX];
    LZ4F_CustomMem const failingMem = { failingAlloc, NULL, countingFree, NULL };
    LZ4F_compressOptions_t cOpts;
    unsigned nbConfigs = 0, nbFailures = 0;
    size_t wi;
    int linked;

    g_step = "begin failures";
    CHECK(src && ref && nextRef && out, "allocation failed");
    RDG_genBuffer(src, srcSize, 0.5, 0.0, 23);
    memset(&cOpts, 0, sizeof(cOpts));
    cOpts.stableSrc = LZ4F_srcStableUntilFlush;
    g_failAt = 0;
    for (wi = 0; wi < sizeof(nbWorkersList)/sizeof(nbWorkersList[0]); wi++)
    for (linked = 0; linked <= 1; linked++) {
        LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
        LZ4F_preferences_t nextPrefs = LZ4F_INIT_PREFERENCES;   /* HC state, larger blocks : resources are reallocated */
        LZ4F_preferences_t badPrefs;
        LZ4F_cctx* cctx;
        size_t refSize, nextRefSize, pos, r, n;
        prefs.nbWorkers = nextPrefs.nbWorkers = nbWorkersList[wi];
        prefs.frameInfo.blockMode = nextPrefs.frameInfo.blockMode = linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
        prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
        nextPrefs.compressionLevel = LZ4HC_CLEVEL_MIN;
        nextPrefs.frameInfo.blockSizeID = LZ4F_max256KB;
        badPrefs = prefs;
        badPrefs.frameInfo.blockSizeID = (LZ4F_blockSizeID_t)3;

        /* references */
        cctx = LZ4F_createCompressionContext_advanced(failingMem, LZ4F_VERSION);
        CHECK(cctx != NULL, "allocation failed");
        refSize = LZ4F_compressBegin(cctx, ref, dstCapacity, &prefs);
        CHECK_LZ4F(refSize);
        r = LZ4F_compressUpdate(cctx, ref + refSize, dstCapacity - refSize, src, firstPart, &cOpts);
        CHECK_LZ4F(r); refSize += r;
        r = LZ4F_compressUpdate(cctx, ref + refSize, dstCapacity - refSize, src + firstPart, srcSize - firstPart, &cOpts);
        CHECK_LZ4F(r); refSize += r;
        r = LZ4F_compressEnd(cctx, ref + refSize, dstCapacity - refSize, NULL);
        CHECK_LZ4F(r); refSize += r;
        nextRefSize = LZ4F_compressBegin(cctx, nextRef, dstCapacity, &nextPrefs);
        CHECK_LZ4F(nextRefSize);
        r = LZ4F_compressUpdate(cctx, nextRef + nextRefSize, dstCapacity - nextRefSize, src, nextSrcSize, NULL);
        CHECK_LZ4F(r); nextRefSize += r;
        r = LZ4F_compressEnd(cctx, nextRef + nextRefSize, dstCapacity - nextRefSize, NULL);
        CHECK_LZ4F(r); nextRefSize += r;

        /* rejected arguments : the frame in progress continues */
        pos = LZ4F_compressBegin(cctx, out, dstCapacity, &prefs);
        CHECK_LZ4F(pos);
        r = LZ4F_compressUpdate(cctx, out + pos, dstCapacity - pos, src, firstPart, &cOpts);
        CHECK_LZ4F(r); pos += r;
        r = LZ4F_compressBegin(cctx, header, 3, &prefs);
        CHECK(LZ4F_getErrorCode(r) == LZ4F_ERROR_dstCapacity_tooSmall, "dstCapacity : %s", LZ4F_getErrorName(r));
        r = LZ4F_compressBegin(cctx, header, sizeof(header), &badPrefs);
        CHECK(LZ4F_getErrorCode(r) == LZ4F_ERROR_maxBlockSize_invalid, "blockSizeID : %s", LZ4F_getErrorName(r));
        r = LZ4F_compressBegin_usingDict(cctx, header, sizeof(header), src, (size_t)INT_MAX + 1, &prefs);
        CHECK(LZ4F_getErrorCode(r) == LZ4F_ERROR_parameter_invalid, "dictSize : %s", LZ4F_getErrorName(r));
        r = LZ4F_compressUpdate(cctx, out + pos, dstCapacity - pos, src + firstPart, srcSize - firstPart, &cOpts);
        CHECK_LZ4F(r); pos += r;
        r = LZ4F_compressEnd(cctx, out + pos, dstCapacity - pos, NULL);
        CHECK_LZ4F(r); pos += r;
        CHECK(pos == refSize && !memcmp(out, ref, pos), "%u workers : frame disturbed by a rejected begin", prefs.nbWorkers);
        LZ4F_freeCompressionContext(cctx);

        /* each allocation of next frame's begin fails in turn, while a frame is in progress */
        for (n = 1; ; n++) {
            cctx = LZ4F_createCompressionContext_advanced(failingMem, LZ4F_VERSION);
            CHECK(cctx != NULL, "allocation failed");
            pos = LZ4F_compressBegin(cctx, out, dstCapacity, &prefs);
            CHECK_LZ4F(pos);
            r = LZ4F_compressUpdate(cctx, out + pos, dstCapacity - pos, src, firstPart, &cOpts);
            CHECK_LZ4F(r);
            g_allocNb = 0;
            g_failAt = n;
            pos = LZ4F_compressBegin(cctx, out, dstCapacity, &nextPrefs);
            g_failAt = 0;
            if (g_allocNb < n) {   /* begin needed fewer allocations : all failure points tested */
                LZ4F_freeCompressionContext(cctx);
                break;
            }
            if (LZ4F_isError(pos)) {
                CHECK(LZ4F_getErrorCode(pos) == LZ4F_ERROR_allocation_failed,
                      "allocation %u : unexpected error %s", (unsigned)n, LZ4F_getErrorName(pos));
                nbFailures++;
                r = LZ4F_compressUpdate(cctx, out, dstCapacity, src, 1 KB, &cOpts);
                CHECK(LZ4F_getErrorCode(r) == LZ4F_ERROR_compressionState_uninitialized,
                      "allocation %u : update after failure : %s", (unsigned)n, LZ4F_getErrorName(r));
                r = LZ4F_uncompressedUpdate(cctx, out, dstCapacity, src, 1 KB, &cOpts);
                CHECK(LZ4F_getErrorCode(r) == LZ4F_ERROR_compressionState_uninitialized,
                      "allocation %u : uncompressed update after failure : %s", (unsigned)n, LZ4F_getErrorName(r));
                r = LZ4F_flush(cctx, out, dstCapacity, NULL);
                CHECK(LZ4F_getErrorCode(r) == LZ4F_ERROR_compressionState_uninitialized,
                      "allocation %u : flush after failure : %s", (unsigned)n, LZ4F_getErrorName(r));
                r = LZ4F_compressEnd(cctx, out, dstCapacity, NULL);
                CHECK(LZ4F_getErrorCode(r) == LZ4F_ERROR_compressionState_uninitialized,
                      "allocation %u : end after failure : %s", (unsigned)n, LZ4F_getErrorName(r));
                pos = LZ4F_compressBegin(cctx, out, dstCapacity, &nextPrefs);   /* context remains usable */
                CHECK_LZ4F(pos);
            }
            r = LZ4F_compressUpdate(cctx, out + pos, dstCapacity - pos, src, nextSrcSize, NULL);
            CHECK_LZ4F(r); pos += r;
            r = LZ4F_compressEnd(cctx, out + pos, dstCapacity - pos, NULL);
            CHECK_LZ4F(r); pos += r;
            CHECK(pos == nextRefSize && !memcmp(out, nextRef, pos), "allocation %u : next frame differs", (unsigned)n);
            LZ4F_freeCompressionContext(cctx);
            CHECK(g_nbAllocs == g_nbFrees, "allocation %u : memory leak (%u allocations, %u frees)",
                  (unsigned)n, (unsigned)g_nbAllocs, (unsigned)g_nbFrees);
        }
        nbConfigs++;
    }
    DISPLAY("begin failures : %u configurations validated, %u allocation failures \n", nbConfigs, nbFailures);
    free(src); free(ref); free(nextRef); free(out);
    return 0;
}


/* An allocation failure preventing the first worker thread fails the frame (allocation_failed) :
 * jobs are not processed serially instead. Failures preventing additional worker threads only reduce their nb.
 * The context remains usable : next frames create resources again. */
static int validatePoolRetry(void)
{
    size_t const srcSize = 9 MB;
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    LZ4F_CustomMem const failingMem = { failingAlloc, NULL, countingFree, NULL };
    char* const src = (char*)malloc(srcSize);
    size_t dstCapacity, refSize, n;
    char* ref;
    char* out;
    unsigned nbFailures = 0, nbAbsorbed = 0;

    g_step = "pool retry";
    prefs.nbWorkers = 2;
    prefs.frameInfo.blockMode = LZ4F_blockIndependent;
    dstCapacity = LZ4F_compressFrameBound(srcSize, &prefs);
    ref = (char*)malloc(dstCapacity);
    out = (char*)malloc(dstCapacity);
    CHECK(src && ref && out, "allocation failed");
    RDG_genBuffer(src, srcSize, 0.5, 0.0, 17);
    refSize = LZ4F_compressFrame(ref, dstCapacity, src, srcSize, &prefs);
    CHECK_LZ4F(refSize);

    for (n = 1; ; n++) {
        LZ4F_cctx* cctx;
        size_t r;
        g_failAt = 0;
        cctx = LZ4F_createCompressionContext_advanced(failingMem, LZ4F_VERSION);
        CHECK(cctx != NULL, "allocation failed");
        g_allocNb = 0;
        g_failAt = n;
        r = LZ4F_compressFrame_usingCDict(cctx, out, dstCapacity, src, srcSize, NULL, &prefs);
        g_failAt = 0;
        if (g_allocNb < n) {   /* the frame needed fewer allocations : all failure points tested */
            LZ4F_freeCompressionContext(cctx);
            break;
        }
        if (LZ4F_isError(r)) {
            CHECK(LZ4F_getErrorCode(r) == LZ4F_ERROR_allocation_failed, "allocation %u : %s", (unsigned)n, LZ4F_getErrorName(r));
            nbFailures++;
        } else {   /* additional worker thread not started */
            CHECK(r == refSize && !memcmp(out, ref, r), "allocation %u absorbed : output differs", (unsigned)n);
            nbAbsorbed++;
        }
        r = LZ4F_compressFrame_usingCDict(cctx, out, dstCapacity, src, srcSize, NULL, &prefs);
        CHECK(r == refSize && !memcmp(out, ref, r), "allocation %u : next frame differs", (unsigned)n);
        LZ4F_freeCompressionContext(cctx);
    }
    CHECK(nbFailures > 0, "no allocation failure reported");
    CHECK(g_nbAllocs == g_nbFrees, "memory leak : %u allocations, %u frees", (unsigned)g_nbAllocs, (unsigned)g_nbFrees);
    free(src); free(ref); free(out);
    DISPLAY("pool retry : %u allocation failures reported, %u fewer worker threads, next frames succeed \n", nbFailures, nbAbsorbed);
    return 0;
}


/*-************************************
*  Benchmark
**************************************/
static void* loadFile(const char* fileName, size_t* sizePtr)
{
    FILE* const f = fopen(fileName, "rb");
    char* buffer;
    long size;
    CHECK(f != NULL, "cannot open %s", fileName);
    CHECK(!fseek(f, 0, SEEK_END), "fseek");
    size = ftell(f);
    CHECK(size > 0, "empty file %s", fileName);
    CHECK(!fseek(f, 0, SEEK_SET), "fseek");
    buffer = (char*)malloc((size_t)size);
    CHECK(buffer != NULL, "allocation");
    CHECK(fread(buffer, 1, (size_t)size, f) == (size_t)size, "read error");
    fclose(f);
    *sizePtr = (size_t)size;
    return buffer;
}

/* compress @fileName once, with @nbWorkers, into @outFileName : allows comparison with the CLI */
static int compressFile(const char* fileName, const char* outFileName,
                        int level, int bsid, int linked, int checksum, unsigned nbWorkers)
{
    size_t srcSize;
    void* const src = loadFile(fileName, &srcSize);
    LZ4F_preferences_t prefs;
    size_t dstCapacity, cSize;
    void* dst;
    LZ4F_cctx* cctx;
    FILE* f;

    memset(&prefs, 0, sizeof(prefs));
    prefs.compressionLevel = level;
    prefs.frameInfo.blockSizeID = (LZ4F_blockSizeID_t)bsid;
    prefs.frameInfo.blockMode = linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
    prefs.frameInfo.contentChecksumFlag = (LZ4F_contentChecksum_t)checksum;
    dstCapacity = LZ4F_compressFrameBound(srcSize, &prefs);
    dst = malloc(dstCapacity);
    CHECK(dst != NULL, "allocation");
    CHECK_LZ4F(LZ4F_createCompressionContext(&cctx, LZ4F_VERSION));
    cSize = compressMT(cctx, dst, dstCapacity, src, srcSize, NULL, &prefs, nbWorkers);
    CHECK_LZ4F(cSize);
    f = fopen(outFileName, "wb");
    CHECK(f != NULL, "cannot open %s", outFileName);
    CHECK(fwrite(dst, 1, cSize, f) == cSize, "write error");
    fclose(f);
    LZ4F_freeCompressionContext(cctx);
    free(dst);
    free(src);
    return 0;
}

static int bench(const char* fileName, int level, int bsid, int linked, int checksum, unsigned maxWorkers)
{
    size_t srcSize;
    void* const src = loadFile(fileName, &srcSize);
    LZ4F_preferences_t prefs;
    size_t dstCapacity;
    void* dst;
    LZ4F_cctx* cctx;
    unsigned nbWorkers = 0;
    size_t ref = 0;

    memset(&prefs, 0, sizeof(prefs));
    prefs.compressionLevel = level;
    prefs.frameInfo.blockSizeID = (LZ4F_blockSizeID_t)bsid;
    prefs.frameInfo.blockMode = linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
    prefs.frameInfo.contentChecksumFlag = (LZ4F_contentChecksum_t)checksum;
    dstCapacity = LZ4F_compressFrameBound(srcSize, &prefs);
    dst = malloc(dstCapacity);
    CHECK(dst != NULL, "allocation");
    CHECK_LZ4F(LZ4F_createCompressionContext(&cctx, LZ4F_VERSION));
    DISPLAY("%s : %u MB, level %i, blockSizeID %i, %s blocks, content checksum %s \n",
            fileName, (unsigned)(srcSize >> 20), level, bsid, linked ? "linked" : "independent", checksum ? "on" : "off");

    while (nbWorkers <= maxWorkers) {
        size_t cSize = 0;
        unsigned nbRuns = 0;
        Duration_ns best = (Duration_ns)-1;
        TIME_t const benchStart = TIME_getTime();
        /* at least 3 runs, and at least 1 second */
        while (nbRuns < 3 || TIME_clockSpan_ns(benchStart) < 1000000000ULL) {
            TIME_t const start = TIME_getTime();
            Duration_ns d;
            cSize = compressMT(cctx, dst, dstCapacity, src, srcSize, NULL, &prefs, nbWorkers);
            CHECK_LZ4F(cSize);
            d = TIME_clockSpan_ns(start);
            if (d < best) best = d;
            nbRuns++;
        }
        if (nbWorkers == 0) ref = cSize;
        DISPLAY("nbWorkers %3u : %8.1f MB/s   ratio %.4f %s \n",
                nbWorkers, (double)srcSize / (double)best * 1000.0,
                (double)srcSize / (double)cSize,
                (cSize == ref) ? "" : "(size differs from single-threaded)");
        nbWorkers = (nbWorkers < 2) ? nbWorkers + 1 : nbWorkers * 2;
    }

    LZ4F_freeCompressionContext(cctx);
    free(dst);
    free(src);
    return 0;
}


/* LZ4F_srcStableUntilFlush : once LZ4F_flush() returns, input can be modified, except its last 64 KB.
 * One buffer, sent in 2 invocations separated by a flush : the second one starts right where the first one ended,
 * so that, with linked blocks, its first job reads its history in place (multi-threaded),
 * and its first block references the previous invocation's input (single-threaded). */
static int validateFlushContract(void)
{
    static const unsigned nbWorkersList[] = { 0, 2 };
    static const int levels[] = { 1, 9 };
    size_t const part1 = 5 MB, srcSize = 13 MB;
    char* const src = (char*)malloc(srcSize);
    char* const work = (char*)malloc(srcSize);
    char* const decoded = (char*)malloc(srcSize);
    LZ4F_cctx* cctx = NULL;
    LZ4F_preferences_t worst = LZ4F_INIT_PREFERENCES;
    size_t dstCapacity;
    char* dst;
    unsigned nbConfigs = 0;
    size_t wi, li;
    int autoFlush;

    worst.nbWorkers = 2;
    worst.frameInfo.blockSizeID = LZ4F_max64KB;
    worst.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    dstCapacity = 2 * LZ4F_compressBound(srcSize, &worst);
    dst = (char*)malloc(dstCapacity);
    g_step = "flush contract";
    CHECK_LZ4F(LZ4F_createCompressionContext(&cctx, LZ4F_VERSION));
    CHECK(src && work && decoded && dst && cctx, "allocation failed");
    RDG_genBuffer(src, srcSize, 0.5, 0.0, 13);

    for (wi = 0; wi < sizeof(nbWorkersList)/sizeof(nbWorkersList[0]); wi++)
    for (li = 0; li < sizeof(levels)/sizeof(levels[0]); li++)
    for (autoFlush = 0; autoFlush <= 1; autoFlush++) {
        LZ4F_preferences_t p = worst;
        LZ4F_compressOptions_t cOpts;
        size_t pos, r, scrambledEnd = 0;
        if (g_quick && (levels[li] != 1)) continue;
        p.compressionLevel = levels[li];
        p.autoFlush = (unsigned)autoFlush;
        p.nbWorkers = nbWorkersList[wi];
        memset(&cOpts, 0, sizeof(cOpts));
        cOpts.stableSrc = LZ4F_srcStableUntilFlush;
        memcpy(work, src, srcSize);
        pos = LZ4F_compressBegin(cctx, dst, dstCapacity, &p);
        CHECK_LZ4F(pos);
        r = LZ4F_compressUpdate(cctx, dst + pos, dstCapacity - pos, work, part1, &cOpts);
        CHECK_LZ4F(r); pos += r;
        r = LZ4F_flush(cctx, dst + pos, dstCapacity - pos, NULL);
        CHECK_LZ4F(r); pos += r;
        scrambleFlushed(work, &scrambledEnd, part1, src, srcSize);
        r = LZ4F_compressUpdate(cctx, dst + pos, dstCapacity - pos, work + part1, srcSize - part1, &cOpts);
        CHECK_LZ4F(r); pos += r;
        r = LZ4F_compressEnd(cctx, dst + pos, dstCapacity - pos, NULL);
        CHECK_LZ4F(r); pos += r;
        checkRoundTrip(dst, pos, src, srcSize, NULL, 0, decoded);
        nbConfigs++;
    }

    LZ4F_freeCompressionContext(cctx);
    free(src); free(work); free(decoded); free(dst);
    DISPLAY("flush contract : %u configurations validated \n", nbConfigs);
    return 0;
}

int main(int argc, const char** argv)
{
    const char* benchFile = NULL;
    const char* outFile = NULL;
    int level = 1, bsid = 7, linked = 0, checksum = 0;
    unsigned maxWorkers = 64;
    int i;

    for (i = 1; i < argc; i++) {
        const char* const arg = argv[i];
        if (!strcmp(arg, "-v")) { g_verbose = 1; continue; }
        if (!strcmp(arg, "-q")) { g_quick = 1; continue; }
        if (!strcmp(arg, "-b") && i+1 < argc) { benchFile = argv[++i]; continue; }
        if (!strcmp(arg, "-o") && i+1 < argc) { outFile = argv[++i]; continue; }
        if (!strcmp(arg, "-D")) { linked = 1; continue; }
        if (!strcmp(arg, "-c")) { checksum = 1; continue; }
        if (!strncmp(arg, "-l", 2)) { level = atoi(arg+2); continue; }
        if (!strncmp(arg, "-B", 2)) { bsid = atoi(arg+2); continue; }
        if (!strncmp(arg, "-T", 2)) { maxWorkers = (unsigned)atoi(arg+2); continue; }
        DISPLAY("unknown argument : %s \n", arg);
        return 1;
    }

    if (benchFile && outFile) return compressFile(benchFile, outFile, level, bsid, linked, checksum, maxWorkers);
    if (benchFile) return bench(benchFile, level, bsid, linked, checksum, maxWorkers);
    {   XXH64_state_t digest;
        XXH64_reset(&digest, 0);
        validate(&digest);
        validateStreaming(&digest);
        validateFlushContract();
        validateAllocFailures();
        validateDstTooSmall();
        validatePartialFlush();
        validateDeferredEmission();
        validateBeginFailures();
        validatePoolRetry();
        printf("digest : %016llx \n", (unsigned long long)XXH64_digest(&digest));
    }
    return 0;
}
