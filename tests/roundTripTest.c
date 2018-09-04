/*
 * Copyright (c) 2016-present, Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

/*
 * This program takes a file in input,
 * performs an LZ4 round-trip test (compression - decompress)
 * compares the result with original
 * and generates an abort() on corruption detection,
 * in order for afl to register the event as a crash.
*/


/*===========================================
*   Dependencies
*==========================================*/
#include <stddef.h>     /* size_t */
#include <stdlib.h>     /* malloc, free, exit */
#include <stdio.h>      /* fprintf */
#include <string.h>     /* strcmp */
#include <assert.h>
#include <sys/types.h>  /* stat */
#include <sys/stat.h>   /* stat */
#include "xxhash.h"

#include "lz4.h"
#include "lz4hc.h"


/*===========================================
*   Macros
*==========================================*/
#define MIN(a,b)  ( (a) < (b) ? (a) : (b) )

#define MSG(...)    fprintf(stderr, __VA_ARGS__)

#define CONTROL_MSG(c, ...) {   \
    if ((c)) {                  \
        MSG(__VA_ARGS__);       \
        MSG(" \n");             \
        abort();                \
    }                           \
}


static size_t checkBuffers(const void* buff1, const void* buff2, size_t buffSize)
{
    const char* const ip1 = (const char*)buff1;
    const char* const ip2 = (const char*)buff2;
    size_t pos;

    for (pos=0; pos<buffSize; pos++)
        if (ip1[pos]!=ip2[pos])
            break;

    return pos;
}

/** roundTripTest() :
 *  Compresses `srcBuff` into `compressedBuff`,
 *  then decompresses `compressedBuff` into `resultBuff`.
 *  Compression level used is derived from content's head bytes.
 *  This function abort() if it detects any round-trip error,
 *  so if it returns, round trip is considered successfully validated.
 *  Note : `compressedBuffCapacity` should be `>= LZ4_compressBound(srcSize)`
 *         for compression to be guaranteed to work */
static void roundTripTest(void* resultBuff, size_t resultBuffCapacity,
                          void* compressedBuff, size_t compressedBuffCapacity,
                    const void* srcBuff, size_t srcSize)
{
    const int minCLevel = 1;
    const int maxClevel = 12;
    const int cLevelSpan = maxClevel - minCLevel;
    size_t const hashLength = MIN(16, srcSize);
    unsigned const h32 = XXH32(srcBuff, hashLength, 0);
    int const randL = h32 % (cLevelSpan+1);
    int const cLevel = minCLevel + randL;
    int const realCLevel = (cLevel * 0) + 9;    /*  <=== Currently : only test level 9 */
    int const cSize = LZ4_compress_HC(srcBuff, compressedBuff, srcSize, compressedBuffCapacity, realCLevel);
    CONTROL_MSG(cSize == 0, "Compression error !");

    {   int const dSize = LZ4_decompress_safe(compressedBuff, resultBuff, cSize, resultBuffCapacity);
        CONTROL_MSG(dSize < 0, "Decompression detected an error !");
        CONTROL_MSG(dSize != (int)srcSize, "Decompression corruption error : wrong decompressed size !");
    }

    /* check potential content corruption error */
    assert(resultBuffCapacity >= srcSize);
    {   size_t const errorPos = checkBuffers(srcBuff, resultBuff, srcSize);
        CONTROL_MSG(errorPos != srcSize,
                    "Silent decoding corruption, at pos %u !!!",
                    (unsigned)errorPos);
    }

}

static void roundTripCheck(const void* srcBuff, size_t srcSize)
{
    size_t const cBuffSize = LZ4_compressBound(srcSize);
    void* const cBuff = malloc(cBuffSize);
    void* const rBuff = malloc(cBuffSize);

    if (!cBuff || !rBuff) {
        fprintf(stderr, "not enough memory ! \n");
        exit(1);
    }

    roundTripTest(rBuff, cBuffSize,
                  cBuff, cBuffSize,
                  srcBuff, srcSize);

    free(rBuff);
    free(cBuff);
}


static size_t getFileSize(const char* infilename)
{
    int r;
#if defined(_MSC_VER)
    struct _stat64 statbuf;
    r = _stat64(infilename, &statbuf);
    if (r || !(statbuf.st_mode & S_IFREG)) return 0;   /* No good... */
#else
    struct stat statbuf;
    r = stat(infilename, &statbuf);
    if (r || !S_ISREG(statbuf.st_mode)) return 0;   /* No good... */
#endif
    return (size_t)statbuf.st_size;
}


static int isDirectory(const char* infilename)
{
    int r;
#if defined(_MSC_VER)
    struct _stat64 statbuf;
    r = _stat64(infilename, &statbuf);
    if (!r && (statbuf.st_mode & _S_IFDIR)) return 1;
#else
    struct stat statbuf;
    r = stat(infilename, &statbuf);
    if (!r && S_ISDIR(statbuf.st_mode)) return 1;
#endif
    return 0;
}


/** loadFile() :
 *  requirement : `buffer` size >= `fileSize` */
static void loadFile(void* buffer, const char* fileName, size_t fileSize)
{
    FILE* const f = fopen(fileName, "rb");
    if (isDirectory(fileName)) {
        fprintf(stderr, "Ignoring %s directory \n", fileName);
        exit(2);
    }
    if (f==NULL) {
        fprintf(stderr, "Impossible to open %s \n", fileName);
        exit(3);
    }
    {   size_t const readSize = fread(buffer, 1, fileSize, f);
        if (readSize != fileSize) {
            fprintf(stderr, "Error reading %s \n", fileName);
            exit(5);
    }   }
    fclose(f);
}


static void fileCheck(const char* fileName)
{
    size_t const fileSize = getFileSize(fileName);
    void* const buffer = malloc(fileSize + !fileSize /* avoid 0 */);
    if (!buffer) {
        fprintf(stderr, "not enough memory \n");
        exit(4);
    }
    loadFile(buffer, fileName, fileSize);
    roundTripCheck(buffer, fileSize);
    free (buffer);
}


int main(int argCount, const char** argv)
{
    int const argNb = 1;

    if (argCount < 2) {
        fprintf(stderr, "Error : no argument : need input file \n");
        exit(9);
    }

    fileCheck(argv[argNb]);
    fprintf(stderr, "no pb detected \n");
    return 0;
}
