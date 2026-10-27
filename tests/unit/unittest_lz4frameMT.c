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
 * tests/fuzz/frameMT.c checks that both builds produce the same frames. */

#include "unittest.h"
#include "lz4frame_helpers.h"   /* lz4frame.h, LZ4F_OK, newCctx, newTmpFile */
#include <stdio.h>      /* FILE, ftell, fseek, fread */
#include "lz4file.h"


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
