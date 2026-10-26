/*
    unittest - minimal unit test framework, in the spirit of googletest
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

/* Each test is an autonomous function, with its own context :
 * - resources obtained from UT_malloc() or registered with UT_defer()
 *   are released at the end of the test, whether it succeeds or fails ;
 * - UT_rand() restarts from the same seed for each test,
 *   so a test behaves the same, whether it runs alone or with others.
 * A failed assertion ends the current test : the runner reports it, and continues with the next test.
 * Assertions can be used anywhere, including in helper functions called by tests,
 * except in callbacks invoked by the library (such as custom allocators) and in other threads :
 * the test ends with longjmp(), which would leave the library in the middle of an operation.
 *
 * Adding a test :
 *     TEST(myTest)
 *     {
 *         ASSERT_EQ(LZ4F_getBlockSize(LZ4F_max64KB), 64 KB);
 *     }
 * then add it to the table at the end of the file :
 *     UT_TEST(myTest),
 * The compiler warns about an unused function when a test is missing from the table.
 *
 * Adding a test file :
 * end it with UT_SUITE(name, tests), add the suite to the list in unittest.c,
 * and the file to the build (tests/Makefile, build/meson/meson/tests/meson.build).
 *
 * Running tests : see `unittest -h`
 */

#ifndef LZ4_UNITTEST_H
#define LZ4_UNITTEST_H

#include <stddef.h>   /* size_t */

#if defined(__GNUC__)
#  define UT_NORETURN __attribute__((noreturn))
#  define UT_PRINTF(f, a) __attribute__((format(printf, f, a)))
#elif defined(_MSC_VER)
#  define UT_NORETURN __declspec(noreturn)
#  define UT_PRINTF(f, a)
#else
#  define UT_NORETURN
#  define UT_PRINTF(f, a)
#endif


/*-************************************
*  Common definitions
**************************************/
#define KB *(1U<<10)
#define MB *(1U<<20)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))


/*-************************************
*  Tests
**************************************/
typedef struct { const char* name; void (*fn)(void); } UT_test;
typedef struct { const char* name; const UT_test* tests; size_t nbTests; } UT_suite;

#define TEST(name) static void UT_test_##name(void)
#define UT_TEST(name) { #name, UT_test_##name }
#define UT_SUITE(name, tests)                                 \
    extern const UT_suite UT_suite_##name;  /* C++ : const objects have internal linkage by default */ \
    const UT_suite UT_suite_##name = { #name, tests, sizeof(tests) / sizeof((tests)[0]) }


/*-************************************
*  Assertions : on failure, report and end the current test
**************************************/
#define ASSERT(cond) \
    do { if (!(cond)) UT_fail(__FILE__, __LINE__, "%s", #cond); } while (0)
#define ASSERT_MSG(cond, ...) \
    do { if (!(cond)) UT_fail(__FILE__, __LINE__, __VA_ARGS__); } while (0)
/* integers, of the same signedness (compared as long long) */
#define ASSERT_EQ(a, b) \
    UT_assertEq((long long)(a), (long long)(b), #a, #b, __FILE__, __LINE__)
#define ASSERT_MEMEQ(a, b, size) \
    UT_assertMemEq((a), (b), (size), #a, #b, __FILE__, __LINE__)


/*-************************************
*  Test context
**************************************/
/* UT_malloc() :
 * @return : a buffer of @size bytes, released at the end of the test.
 *  Never returns NULL : an allocation failure fails the test. */
void* UT_malloc(size_t size);

/* UT_defer() :
 * registers @release(@ptr), to be invoked at the end of the test (even if @ptr == NULL).
 * @release must not use assertions.
 * @return : @ptr */
void* UT_defer(void (*release)(void*), void* ptr);

/* UT_rand() :
 * pseudo-random generator, restarted from the same seed (-s#) for each test */
unsigned UT_rand(void);

/* UT_sample() :
 * @return : @size bytes of compressible data, generated from UT_rand(), released at the end of the test */
void* UT_sample(size_t size);

/* UT_noise() :
 * @return : @size bytes of incompressible data, generated from UT_rand(), released at the end of the test */
void* UT_noise(size_t size);


/*-************************************
*  Implementation of assertions
**************************************/
UT_NORETURN UT_PRINTF(3, 4)
void UT_fail(const char* file, int line, const char* format, ...);
void UT_assertEq(long long a, long long b, const char* aExpr, const char* bExpr, const char* file, int line);
void UT_assertMemEq(const void* a, const void* b, size_t size,
                    const char* aExpr, const char* bExpr, const char* file, int line);

#endif /* LZ4_UNITTEST_H */
