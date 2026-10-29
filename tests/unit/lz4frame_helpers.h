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

/* Helpers shared by the unit tests of lz4frame (unittest_lz4frame.c, unittest_lz4frameMT.c).
 * Like assertions, they end the current test on failure. */

#ifndef LZ4FRAME_HELPERS_H
#define LZ4FRAME_HELPERS_H

#include <stddef.h>   /* size_t */
#include <stdio.h>    /* FILE */
#ifndef LZ4F_STATIC_LINKING_ONLY
#  define LZ4F_STATIC_LINKING_ONLY   /* LZ4F_errorCodes */
#endif
#include "lz4frame.h"

/* LZ4F_OK() : fails the test if @r is an error code
 * @return : @r */
#define LZ4F_OK(r) checkLZ4F((r), #r, __FILE__, __LINE__)
size_t checkLZ4F(size_t r, const char* expr, const char* file, int line);

/* ASSERT_LZ4F_ERROR() : fails the test unless @r is the error code @code */
#define ASSERT_LZ4F_ERROR(r, code) checkLZ4FError((r), code, #r, #code, __FILE__, __LINE__)
void checkLZ4FError(size_t r, LZ4F_errorCodes code,
                    const char* expr, const char* codeName, const char* file, int line);

/* release functions, for UT_defer() */
void freeCctx(void* cctx);
void freeDctx(void* dctx);
void freeCDict(void* cdict);
void closeFile(void* file);

/* contexts, released at the end of the test */
LZ4F_cctx* newCctx(void);
LZ4F_dctx* newDctx(void);
LZ4F_CDict* newCDict(const void* dict, size_t dictSize);

/* @return : a temporary file, closed at the end of the test */
FILE* newTmpFile(void);

/* allocator failing its N-th allocation (failAt, counting from 1 ; 0 : never),
 * and counting allocations not yet freed (live).
 * Counters are deliberately unprotected : an allocation from a worker thread would be reported by ThreadSanitizer. */
typedef struct {
    size_t failAt;
    size_t nbAllocs;   /* allocations requested */
    size_t live;
} allocState_t;
void* failingAlloc(void* state, size_t size);
void countingFree(void* state, void* address);

/* @return : allocator state, released at the end of the test :
 * after the contexts employing it, provided they are created afterwards (resources are released in reverse order) */
allocState_t* newAllocState(void);

/* @return : context employing the allocator of @mem, released at the end of the test,
 *           or NULL if its creation fails */
LZ4F_cctx* createCctx(allocState_t* mem);
LZ4F_dctx* createDctx(allocState_t* mem);

/* fails the test unless @frame decodes entirely into @expected, using @dict */
void assertDecodesTo(const void* frame, size_t frameSize,
                     const void* dict, size_t dictSize,
                     const void* expected, size_t expectedSize);

#endif /* LZ4FRAME_HELPERS_H */
