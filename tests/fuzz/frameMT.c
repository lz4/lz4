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
 *   input isn't read after invocation, unless LZ4F_srcStableUntilFlush
 * - with independent blocks, output is identical to nbWorkers == 0
 * - raw dictionary (LZ4F_compressBegin_usingDict()) : same output whatever nbWorkers, the same as its CDict at levels >= 3
 * - serial processing (dstCapacity too small for parallel jobs) produces the same output
 * - all allocations happen in the calling thread (custom allocator is not thread-safe : see TSAN)
 * - prints a digest of produced frames, to compare builds with and without LZ4F_MULTITHREAD
 * Other properties (allocation failures, destination too small, flush in pieces, abandoned frames...) :
 * see unit tests, in tests/unit/unittest_lz4frameMT.c
 * Bench mode : frameMT -b FILE [-l#] [-B#] [-D] [-c] [-T#]
 * Compress once with # workers : frameMT -b FILE -o OUTFILE [-l#] [-B#] [-D] [-c] [-T#]
 */

/*-************************************
*  Includes
**************************************/
#include <stdlib.h>     /* malloc, free, exit */
#include <stdio.h>      /* fprintf, fopen */
#include <string.h>     /* memcmp, strcmp */
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
    int uncompressed;   /* some slices sent as uncompressed input (LZ4F_compressOptions_t.uncompressed) */
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
        cOpts.uncompressed = mode.uncompressed && ((r >> 20) % 5 == 0);
        cSize = LZ4F_compressUpdate(cctx, tmpOut, LZ4F_compressBound(slice, &p), in, slice, &cOpts);
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

    LZ4F_freeCompressionContext(cctx);
    LZ4F_freeCDict(cdict);
    CHECK(g_nbAllocs == g_nbFrees, "memory leak : %u allocations, %u frees", (unsigned)g_nbAllocs, (unsigned)g_nbFrees);
    DISPLAY("streaming : %u configurations validated \n", nbConfigs);
    free(src); free(dict); free(scratch); free(ref); free(refFlush); free(refAuto); free(refUnc); free(refRaw); free(out); free(decoded); free(tmpOut);
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
        printf("digest : %016llx \n", (unsigned long long)XXH64_digest(&digest));
    }
    return 0;
}
