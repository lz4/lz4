/*
    lz4frame_helpers - helpers of lz4frame unit tests
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
#include "lz4frame_helpers.h"
#include <stdio.h>    /* tmpfile, fclose */
#include <stdlib.h>   /* malloc, free */
#include <string.h>   /* memset */

size_t checkLZ4F(size_t r, const char* expr, const char* file, int line)
{
    if (LZ4F_isError(r)) UT_fail(file, line, "%s \n  error : %s", expr, LZ4F_getErrorName(r));
    return r;
}

void checkLZ4FError(size_t r, LZ4F_errorCodes code,
                    const char* expr, const char* codeName, const char* file, int line)
{
    if (LZ4F_getErrorCode(r) != code)
        UT_fail(file, line, "%s \n  expected : %s \n  result : %s", expr, codeName,
                LZ4F_isError(r) ? LZ4F_getErrorName(r) : "no error");
}

void freeCctx(void* cctx) { LZ4F_freeCompressionContext((LZ4F_cctx*)cctx); }
void freeDctx(void* dctx) { LZ4F_freeDecompressionContext((LZ4F_dctx*)dctx); }
void freeCDict(void* cdict) { LZ4F_freeCDict((LZ4F_CDict*)cdict); }
void closeFile(void* file) { if (file != NULL) fclose((FILE*)file); }

LZ4F_cctx* newCctx(void)
{
    LZ4F_cctx* cctx = NULL;
    LZ4F_OK(LZ4F_createCompressionContext(&cctx, LZ4F_VERSION));
    return (LZ4F_cctx*)UT_defer(freeCctx, cctx);
}

LZ4F_dctx* newDctx(void)
{
    LZ4F_dctx* dctx = NULL;
    LZ4F_OK(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION));
    return (LZ4F_dctx*)UT_defer(freeDctx, dctx);
}

LZ4F_CDict* newCDict(const void* dict, size_t dictSize)
{
    LZ4F_CDict* const cdict = (LZ4F_CDict*)UT_defer(freeCDict, LZ4F_createCDict(dict, dictSize));
    ASSERT(cdict != NULL);
    return cdict;
}

FILE* newTmpFile(void)
{
    FILE* const file = (FILE*)UT_defer(closeFile, tmpfile());
    ASSERT(file != NULL);
    return file;
}

void* failingAlloc(void* state, size_t size)
{
    allocState_t* const s = (allocState_t*)state;
    if (++s->nbAllocs == s->failAt) return NULL;
    s->live++;
    return malloc(size);
}

void countingFree(void* state, void* address)
{
    if (address == NULL) return;
    ((allocState_t*)state)->live--;
    free(address);
}

allocState_t* newAllocState(void)
{
    allocState_t* const s = (allocState_t*)UT_malloc(sizeof(*s));
    memset(s, 0, sizeof(*s));
    return s;
}

LZ4F_cctx* createCctx(allocState_t* mem)
{
    LZ4F_CustomMem const cmem = { failingAlloc, NULL, countingFree, mem };
    LZ4F_cctx* const cctx = LZ4F_createCompressionContext_advanced(cmem, LZ4F_VERSION);
    return cctx ? (LZ4F_cctx*)UT_defer(freeCctx, cctx) : NULL;
}

LZ4F_dctx* createDctx(allocState_t* mem)
{
    LZ4F_CustomMem const cmem = { failingAlloc, NULL, countingFree, mem };
    LZ4F_dctx* const dctx = LZ4F_createDecompressionContext_advanced(cmem, LZ4F_VERSION);
    return dctx ? (LZ4F_dctx*)UT_defer(freeDctx, dctx) : NULL;
}

void assertDecodesTo(const void* frame, size_t frameSize,
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
