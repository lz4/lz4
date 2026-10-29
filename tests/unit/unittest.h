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
 * Tests register themselves before main(), and run in the order of the source.
 * Their suite is the name of their file, without "unittest_" (unittest_lz4frame.c : lz4frame).
 *
 * Adding a test file :
 * add it to the build (tests/Makefile, build/meson/meson/tests/meson.build),
 * as an object file : linked from a static library, it could be dropped, since nothing references it.
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
#undef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#undef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))


/*-************************************
*  Tests
**************************************/
typedef struct UT_test_s {
    const char* name;
    const char* file;
    int line;
    void (*fn)(void);
    struct UT_test_s* next;
} UT_test;

void UT_register(UT_test* test);

#define TEST(name)                                                                         \
    static void UT_test_##name(void);                                                      \
    static UT_test UT_node_##name = { #name, __FILE__, __LINE__, UT_test_##name, NULL };   \
    UT_CONSTRUCTOR(UT_register_##name) { UT_register(&UT_node_##name); }                  \
    static void UT_test_##name(void)

/* UT_CONSTRUCTOR(f) { ... } : defines f(), invoked before main() */
#if defined(__cplusplus)   /* initialization of a static object */
#  define UT_CONSTRUCTOR(f)                                                \
    static void f(void);                                                   \
    namespace { struct f##_caller { f##_caller() { f(); } } f##_instance; } \
    static void f(void)
#elif defined(__GNUC__)    /* gcc, clang */
#  define UT_CONSTRUCTOR(f) static void f(void) __attribute__((constructor)); static void f(void)
#elif defined(_MSC_VER)    /* pointer in the table of initializers of the C runtime */
#  pragma section(".CRT$XCU", read)
#  if defined(_M_IX86)
#    define UT_SYMBOL_PREFIX "_"
#  else
#    define UT_SYMBOL_PREFIX ""
#  endif
#  define UT_CAT_(a, b) a##b
#  define UT_CAT(a, b) UT_CAT_(a, b)
#  define UT_STR_(a) #a
#  define UT_STR(a) UT_STR_(a)   /* expands a first */
   /* ptr has external linkage, and is named after __LINE__, to be unique,
    * so that the linker can be told to keep it (/include) */
#  define UT_CONSTRUCTOR(f) UT_CONSTRUCTOR_(f, UT_CAT(f##_ptr, __LINE__))
#  define UT_CONSTRUCTOR_(f, ptr)                                          \
    static void __cdecl f(void);                                           \
    __declspec(allocate(".CRT$XCU")) void (__cdecl* ptr)(void) = f;        \
    __pragma(comment(linker, "/include:" UT_SYMBOL_PREFIX UT_STR(ptr)))    \
    static void __cdecl f(void)
#else
#  error "unittest.h : registration of tests before main() is not supported by this compiler"
#endif


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

/* UT_release() :
 * releases @ptr, obtained from UT_malloc() or registered with UT_defer(), before the end of the test.
 * Useful within loops : a test can't hold more than 64 resources at once. */
void UT_release(void* ptr);

/* UT_rand() :
 * pseudo-random generator, restarted from the same seed (-s#, or random with -r) for each test */
unsigned UT_rand(void);

/* UT_fillSample(), UT_fillNoise() :
 * fill @buffer with @size bytes of compressible, or incompressible, data, generated from UT_rand() */
void UT_fillSample(void* buffer, size_t size);
void UT_fillNoise(void* buffer, size_t size);

/* UT_sample(), UT_noise() :
 * @return : @size bytes of compressible, or incompressible, data, released at the end of the test.
 * Content changes with the seed : only for checks valid with any content (round trips, returned values). */
void* UT_sample(size_t size);
void* UT_noise(size_t size);

/* UT_fixedSample(), UT_fixedNoise() :
 * same, with a content independent of the seed :
 * for checks depending on content (ratio, matches found, effect of a parameter on the output),
 * which must not depend on the seed. */
void* UT_fixedSample(size_t size);
void* UT_fixedNoise(size_t size);


/*-************************************
*  Implementation of assertions
**************************************/
UT_NORETURN UT_PRINTF(3, 4)
void UT_fail(const char* file, int line, const char* format, ...);
void UT_assertEq(long long a, long long b, const char* aExpr, const char* bExpr, const char* file, int line);
void UT_assertMemEq(const void* a, const void* b, size_t size,
                    const char* aExpr, const char* bExpr, const char* file, int line);

#endif /* LZ4_UNITTEST_H */
