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

/* Validates LZ4F_preferences_t.nbWorkers with LZ4F_compressFrame_usingCDict() and LZ4F_compressFrame() :
 * - round trip
 * - output is identical for any nbWorkers >= 1
 * - with independent blocks, output is identical to nbWorkers == 0
 * - serial processing (dstCapacity too small for parallel jobs) produces the same output
 * - all allocations happen in the calling thread (custom allocator is not thread-safe : see TSAN)
 * - prints a digest of produced frames, to compare builds with and without LZ4F_MULTITHREAD
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
        DISPLAY("Error at line %i : ", __LINE__);              \
        DISPLAY(__VA_ARGS__);                                  \
        DISPLAY(" \n");                                        \
        exit(1);                                               \
    } } while (0)
#define CHECK_LZ4F(r) CHECK(!LZ4F_isError(r), "%s", LZ4F_getErrorName(r))

static int g_verbose = 0;


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
    CHECK(!memcmp(tmp, src, srcSize), "decoded content differs");
    LZ4F_freeDecompressionContext(dctx);
}


/*-************************************
*  Validation
**************************************/
static int validate(void)
{
    static const size_t srcSizes[] = { 1 MB + 1, 4 MB - 1, 4 MB + 1, 9 MB + 12345, 17 MB + 3 };
    static const int levels[] = { -3, 0, 1, 2, 4, 9, 10, 12 };
    static const unsigned nbWorkersList[] = { 2, 3, 8, 1000 /* capped */ };
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
    LZ4F_CDict* const cdict = LZ4F_createCDict(dict, dictSize);
    XXH64_state_t digest;
    unsigned nbConfigs = 0;
    unsigned seed = 1;
    size_t si, li;
    int linked;

    CHECK(src && dict && ref0 && ref1 && out && decoded && cctx && cdict, "allocation failed");
    RDG_genBuffer(src, srcSizeMax, 0.5, 0.0, 7);
    RDG_genBuffer(dict, dictSize, 0.5, 0.0, 8);
    XXH64_reset(&digest, 0);

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

            /* nbWorkers == 1 : jobs, processed serially */
            c1 = compressMT(cctx, ref1, dstCapacity, src, srcSize, cd, &prefs, 1);
            CHECK_LZ4F(c1);
            checkRoundTrip(ref1, c1, src, srcSize, cd ? dict : NULL, cd ? dictSize : 0, decoded);
            if (!linked)
                CHECK(c1 == c0 && !memcmp(ref1, ref0, c0), "independent blocks : output differs from single-threaded");
            XXH64_update(&digest, ref1, c1);

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
    DISPLAY("%u configurations validated (%u allocations) \n", nbConfigs, (unsigned)g_nbAllocs);
    printf("digest : %016llx \n", (unsigned long long)XXH64_digest(&digest));

    free(src); free(dict); free(ref0); free(ref1); free(out); free(decoded);
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
    return validate();
}
