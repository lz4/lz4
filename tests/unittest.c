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

#include "unittest.h"
#include <setjmp.h>   /* setjmp, longjmp */
#include <stdarg.h>   /* va_list */
#include <stdio.h>    /* printf, snprintf */
#include <stdlib.h>   /* malloc, free, strtoul */
#include <string.h>   /* strcmp, strstr, memcmp */
#include <time.h>     /* clock */


/*-************************************
*  Test suites : one per test file
**************************************/
extern const UT_suite UT_suite_lz4frame;

static const UT_suite* const g_suites[] = {
    &UT_suite_lz4frame,
};


/*-************************************
*  Context of the current test
**************************************/
#define UT_RESOURCES_MAX 64
static struct { void (*release)(void*); void* ptr; } g_resources[UT_RESOURCES_MAX];
static size_t g_nbResources = 0;
static unsigned g_seed = 1;
static unsigned g_randState;
static jmp_buf g_testEnd;

void* UT_defer(void (*release)(void*), void* ptr)
{
    if (g_nbResources == UT_RESOURCES_MAX) {
        release(ptr);
        UT_fail(__FILE__, __LINE__, "too many resources in a single test (max %i)", UT_RESOURCES_MAX);
    }
    g_resources[g_nbResources].release = release;
    g_resources[g_nbResources].ptr = ptr;
    g_nbResources++;
    return ptr;
}

void* UT_malloc(size_t size)
{
    void* const ptr = UT_defer(free, malloc(size ? size : 1));
    if (ptr == NULL) UT_fail(__FILE__, __LINE__, "UT_malloc(%lu) : allocation failed", (unsigned long)size);
    return ptr;
}

unsigned UT_rand(void)
{
    /* xorshift32 */
    g_randState ^= g_randState << 13;
    g_randState ^= g_randState >> 17;
    g_randState ^= g_randState << 5;
    return g_randState;
}


/*-************************************
*  Assertions
**************************************/
void UT_fail(const char* file, int line, const char* format, ...)
{
    va_list args;
    printf("%s:%i: Failure \n  ", file, line);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf(" \n");
    longjmp(g_testEnd, 1);
}

void UT_assertEq(long long a, long long b, const char* aExpr, const char* bExpr, const char* file, int line)
{
    if (a != b) UT_fail(file, line, "%s == %s \n  %lld != %lld", aExpr, bExpr, a, b);
}

void UT_assertMemEq(const void* a, const void* b, size_t size,
                    const char* aExpr, const char* bExpr, const char* file, int line)
{
    const unsigned char* const pa = (const unsigned char*)a;
    const unsigned char* const pb = (const unsigned char*)b;
    size_t pos = 0;
    if (memcmp(a, b, size) == 0) return;
    while (pa[pos] == pb[pos]) pos++;
    UT_fail(file, line, "%s and %s differ at position %lu (of %lu bytes)",
            aExpr, bExpr, (unsigned long)pos, (unsigned long)size);
}


/*-************************************
*  Runner
**************************************/
/* @return : 1 if the test fails, 0 otherwise */
static int UT_invoke(void (*fn)(void))
{
    if (setjmp(g_testEnd)) return 1;
    fn();
    return 0;
}

/* @return : 1 if the test fails, 0 otherwise */
static int UT_run(const char* name, void (*fn)(void))
{
    clock_t const start = clock();
    int failed;
    printf("[ RUN      ] %s \n", name);
    fflush(stdout);   /* in case the test crashes */
    g_randState = (g_seed * 2654435761U) | 1;   /* xorshift32 requires a non-zero state */
    failed = UT_invoke(fn);
    while (g_nbResources) {
        g_nbResources--;
        g_resources[g_nbResources].release(g_resources[g_nbResources].ptr);
    }
    printf("[ %s ] %s (%lu ms) \n", failed ? " FAILED " : "      OK", name,
           (unsigned long)((clock() - start) * 1000 / CLOCKS_PER_SEC));
    return failed;
}

/* a test is selected if its name contains one of the filters, or if there is no filter */
static int UT_isSelected(const char* name, int argc, const char** argv)
{
    int i, hasFilter = 0;
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        if (strstr(name, argv[i])) return 1;
        hasFilter = 1;
    }
    return !hasFilter;
}

static int UT_usage(const char* exeName)
{
    printf("Usage : %s [-l] [-s#] [filter...] \n", exeName);
    printf("Runs the tests whose name (suite.test) contains one of the filters, or all tests \n");
    printf(" -l  : list tests, without running them \n");
    printf(" -s# : seed of UT_rand() (default : %u) \n", g_seed);
    return 1;
}

int main(int argc, const char** argv)
{
    clock_t const start = clock();
    unsigned nbTests = 0, nbFailed = 0;
    int listOnly = 0, i;
    size_t s, t;

    for (i = 1; i < argc; i++) {
        const char* const arg = argv[i];
        if (arg[0] != '-') continue;   /* filter */
        if (!strcmp(arg, "-l")) listOnly = 1;
        else if (arg[1] == 's' && arg[2] != 0) g_seed = (unsigned)strtoul(arg + 2, NULL, 10);
        else return UT_usage(argv[0]);
    }

    for (s = 0; s < sizeof(g_suites) / sizeof(g_suites[0]); s++) {
        for (t = 0; t < g_suites[s]->nbTests; t++) {
            const UT_test* const test = g_suites[s]->tests + t;
            char name[256];
            snprintf(name, sizeof(name), "%s.%s", g_suites[s]->name, test->name);
            if (!UT_isSelected(name, argc, argv)) continue;
            nbTests++;
            if (listOnly) printf("%s \n", name);
            else nbFailed += (unsigned)UT_run(name, test->fn);
    }   }

    if (nbTests == 0) { printf("no test selected \n"); return 1; }
    if (listOnly) return 0;
    printf("[==========] %u tests, %u failed (%lu ms) \n", nbTests, nbFailed,
           (unsigned long)((clock() - start) * 1000 / CLOCKS_PER_SEC));
    return nbFailed != 0;
}
