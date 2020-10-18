/*
  LZ4iomt.h - LZ4 File/Stream Interface with multi thread support
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

#ifndef LZ4IOMT_H_967887017
#define LZ4IOMT_H_967887017

#include <stdio.h>     /* fprintf, fopen, fread, stdin, stdout, fflush, getchar, FILE */

#ifdef __cplusplus
extern "C" {
#endif

#define LZ4F_STATIC_LINKING_ONLY
#include "lz4frame.h"

typedef struct {
    void*  srcBuffer;
    size_t srcBufferSize;
    void*  dstBuffer;
    size_t dstBufferSize;
    LZ4F_compressionContext_t ctx;
    LZ4F_CDict* cdict;
} cRess_t;

/*
 * parallel = 0 : use all (logical) cores, each core has a worker thread
 * parallel = 1 : disable multithread
 * parallel = X : use X threads
 */
void LZ4IO_setParallel(unsigned parallel);
int LZ4IO_multiThreadAvailable();

static int LZ4IO_compressUseMultiThread(const LZ4F_CDict* cdict, const LZ4F_preferences_t* prefsPtr) {
    return
        (cdict == NULL) && prefsPtr->frameInfo.dictID == 0 &&
        prefsPtr->frameInfo.blockMode == LZ4F_blockIndependent &&
        LZ4IO_multiThreadAvailable();
}

/*
 * LZ4IO_compressFilename_extRess_multiBlocks()
 * result : filesize
 */
unsigned long long LZ4IO_compressFilename_extRess_multithread(const cRess_t* pRess, FILE* srcFile, const char* srcFileName, FILE* dstFile,
                                                              const LZ4F_preferences_t* pPrefs, unsigned long long* pCompressedfilesize);


#ifdef __cplusplus
}
#endif

#endif  /* LZ4IOMT_H_967887017 */