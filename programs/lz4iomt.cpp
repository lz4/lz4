/*
  LZ4iomt.cpp - LZ4 File/Stream Interface with multi thread support
  Copyright (C) yumeyao 2020
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
  - LZ4 source repository : https://github.com/lz4/lz4
  - LZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
*/
/*
  Note : this is stand-alone program.
  It is not part of LZ4 compression library, it is a user code of the LZ4 library.
  - The license of LZ4 library is BSD.
  - The license of xxHash library is BSD.
  - The license of this source file is GPLv2.
*/

#include <thread>
extern "C" {
#include "lz4io.c"
}

static unsigned parallel = 0;

void LZ4IO_setParallel(unsigned n) {
    parallel = n;
}

static unsigned LZ4IO_getAvailableCores() {
    unsigned n = std::thread::hardware_concurrency();
    return n > 0 ? n : 1;
}

int LZ4IO_multiThreadAvailable() {
    if (parallel == 0)
    {
        parallel = LZ4IO_getAvailableCores();
    }
    return parallel - 1;
}

unsigned long long LZ4IO_compressFilename_extRess_multithread(const cRess_t* pRess, FILE* srcFile, const char* srcFileName, FILE* dstFile,
                                                              const LZ4F_preferences_t* pPrefs, unsigned long long* pCompressedfilesize)
{
    DISPLAYLEVEL(4, "Using Multithread mode\n");
    void* const srcBuffer = pRess->srcBuffer;
    size_t blockSize = pRess->srcBufferSize;
    void* const dstBuffer = pRess->dstBuffer;
    const size_t dstBufferSize = pRess->dstBufferSize;
    LZ4F_compressionContext_t ctx = pRess->ctx;   /* just a pointer */
    unsigned long long compressedfilesize = 0;
    size_t readSize = blockSize;
    unsigned long long filesize = readSize;

    /* Write Archive Header */
    size_t headerSize = LZ4F_compressBegin_usingCDict(ctx, dstBuffer, dstBufferSize, pRess->cdict, pPrefs);
    if (LZ4F_isError(headerSize)) EXM_THROW(33, "File header generation failed : %s", LZ4F_getErrorName(headerSize));
    { size_t const sizeCheck = fwrite(dstBuffer, 1, headerSize, dstFile);
        if (sizeCheck!=headerSize) EXM_THROW(34, "Write error : cannot write header"); }
    compressedfilesize += headerSize;

    /* Main Loop */
    while (readSize>0) {
        size_t outSize;

        /* Compress Block */
        outSize = LZ4F_compressUpdate(ctx, dstBuffer, dstBufferSize, srcBuffer, readSize, NULL);
        if (LZ4F_isError(outSize)) EXM_THROW(35, "Compression failed : %s", LZ4F_getErrorName(outSize));
        compressedfilesize += outSize;
        DISPLAYUPDATE(2, "\rRead : %u MB   ==> %.2f%%   ", (unsigned)(filesize>>20), (double)compressedfilesize/filesize*100);

        /* Write Block */
        { size_t const sizeCheck = fwrite(dstBuffer, 1, outSize, dstFile);
            if (sizeCheck!=outSize) EXM_THROW(36, "Write error : cannot write compressed block"); }

        /* Read next block */
        readSize  = fread(srcBuffer, (size_t)1, (size_t)blockSize, srcFile);
        filesize += readSize;
    }
    if (ferror(srcFile)) EXM_THROW(37, "Error reading %s ", srcFileName);

    /* End of Stream mark */
    headerSize = LZ4F_compressEnd(ctx, dstBuffer, dstBufferSize, NULL);
    if (LZ4F_isError(headerSize)) EXM_THROW(38, "End of file generation failed : %s", LZ4F_getErrorName(headerSize));

    { size_t const sizeCheck = fwrite(dstBuffer, 1, headerSize, dstFile);
        if (sizeCheck!=headerSize) EXM_THROW(39, "Write error : cannot write end of stream"); }
    compressedfilesize += headerSize;

    *pCompressedfilesize = compressedfilesize;
    return filesize;
}