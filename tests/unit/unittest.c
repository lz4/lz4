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
#include "datagen.h"  /* RDG_genBuffer */
#include "xxhash.h"   /* XXH32 */
#include <setjmp.h>   /* setjmp, longjmp */
#include <stdarg.h>   /* va_list */
#include <stdio.h>    /* printf, snprintf */
#include <stdlib.h>   /* malloc, free, strtoul */
#include <string.h>   /* strcmp, strncmp, strstr, strcspn, memcmp */
#include <time.h>     /* clock, time */


/*-************************************
*  Registered tests
**************************************/
static UT_test* g_tests = NULL;

static int UT_compare(const UT_test* a, const UT_test* b)
{
    int const c = strcmp(a->file, b->file);
    return c ? c : a->line - b->line;
}

/* keeps tests sorted by file, then by line : the order of the source, whatever the order of registration */
void UT_register(UT_test* test)
{
    UT_test** p = &g_tests;
    while (*p != NULL && UT_compare(*p, test) < 0) p = &(*p)->next;
    test->next = *p;
    *p = test;
}

/* @return : suite.test, the suite being the name of the file, without "unittest_" and extension */
static void UT_fullName(char* dst, size_t dstSize, const UT_test* test)
{
    const char* suite = test->file;
    const char* p;
    for (p = test->file; *p; p++) if (*p == '/' || *p == '\\') suite = p + 1;
    if (!strncmp(suite, "unittest_", 9)) suite += 9;
    snprintf(dst, dstSize, "%.*s.%s", (int)strcspn(suite, "."), suite, test->name);
}


/*-************************************
*  Context of the current test
**************************************/
#define UT_RESOURCES_MAX 64
static struct { void (*release)(void*); void* ptr; } g_resources[UT_RESOURCES_MAX];
static size_t g_nbResources = 0;
static unsigned g_seed = 1;
static const char* g_testName = "";   /* current test, for failure messages */
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

void UT_release(void* ptr)
{
    size_t n = g_nbResources;
    while (n > 0 && g_resources[n-1].ptr != ptr) n--;
    if (n == 0) UT_fail(__FILE__, __LINE__, "UT_release() : unknown resource");
    g_resources[n-1].release(ptr);
    for ( ; n < g_nbResources; n++) g_resources[n-1] = g_resources[n];
    g_nbResources--;
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

void UT_fillSample(void* buffer, size_t size)
{
    RDG_genBuffer(buffer, size, 0.5, 0.0, UT_rand());
}

void UT_fillNoise(void* buffer, size_t size)
{
    unsigned char* const p = (unsigned char*)buffer;
    size_t n;
    for (n = 0; n < size; n++) p[n] = (unsigned char)UT_rand();
}

void* UT_sample(size_t size)
{
    void* const buffer = UT_malloc(size);
    UT_fillSample(buffer, size);
    return buffer;
}

void* UT_noise(size_t size)
{
    void* const buffer = UT_malloc(size);
    UT_fillNoise(buffer, size);
    return buffer;
}

void* UT_fixedSample(size_t size)
{
    void* const buffer = UT_malloc(size);
    RDG_genBuffer(buffer, size, 0.5, 0.0, 1);
    return buffer;
}

void* UT_fixedNoise(size_t size)
{
    unsigned char* const buffer = (unsigned char*)UT_malloc(size);
    unsigned state = 1;
    size_t n;
    for (n = 0; n < size; n++) {
        state = state * 1103515245u + 12345u;
        buffer[n] = (unsigned char)(state >> 16);
    }
    return buffer;
}


/*-************************************
*  Assertions
**************************************/
/* failures are written to stderr : visible even when stdout is discarded (ex: CI step `make test > /dev/null`) */
void UT_fail(const char* file, int line, const char* format, ...)
{
    va_list args;
    fflush(stdout);
    fprintf(stderr, "%s:%i: %s : Failure \n  ", file, line, g_testName);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, " \n");
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
/* computed in double : clock_t may be a 32-bit integer counting microseconds */
static unsigned long UT_elapsedMs(clock_t start)
{
    return (unsigned long)((double)(clock() - start) * 1000 / CLOCKS_PER_SEC);
}

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
    g_testName = name;
    g_randState = (g_seed * 2654435761U) | 1;   /* xorshift32 requires a non-zero state */
    failed = UT_invoke(fn);
    while (g_nbResources) {
        g_nbResources--;
        g_resources[g_nbResources].release(g_resources[g_nbResources].ptr);
    }
    printf("[ %s ] %s (%lu ms) \n", failed ? " FAILED " : "      OK", name, UT_elapsedMs(start));
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
    printf("Usage : %s [-l] [-s#] [-r] [filter...] \n", exeName);
    printf("Runs the tests whose name (suite.test) contains one of the filters, or all tests \n");
    printf(" -l  : list tests, without running them \n");
    printf(" -s# : seed of UT_rand() (default : %u) \n", g_seed);
    printf(" -r  : random seed, reported at the end \n");
    return 1;
}

int main(int argc, const char** argv)
{
    clock_t const start = clock();
    unsigned nbTests = 0, nbFailed = 0;
    int listOnly = 0, i;
    const UT_test* test;

    for (i = 1; i < argc; i++) {
        const char* const arg = argv[i];
        if (arg[0] != '-') continue;   /* filter */
        if (!strcmp(arg, "-l")) listOnly = 1;
        else if (arg[1] == 's' && arg[2] != 0) g_seed = (unsigned)strtoul(arg + 2, NULL, 10);
        else if (!strcmp(arg, "-r")) {   /* hashed : successive runs get unrelated seeds */
            time_t const t = time(NULL);
            g_seed = XXH32(&t, sizeof(t), 1) % 100000;
        }
        else return UT_usage(argv[0]);
    }

    for (test = g_tests; test != NULL; test = test->next) {
        char name[256];
        UT_fullName(name, sizeof(name), test);
        if (!UT_isSelected(name, argc, argv)) continue;
        nbTests++;
        if (listOnly) printf("%s \n", name);
        else nbFailed += (unsigned)UT_run(name, test->fn);
    }

    if (nbTests == 0) { printf("no test selected \n"); return 1; }
    if (listOnly) return 0;
    printf("[==========] %u tests, %u failed (%lu ms), seed %u \n", nbTests, nbFailed, UT_elapsedMs(start), g_seed);
    if (nbFailed) fprintf(stderr, "failures are reproduced with -s%u \n", g_seed);
    return nbFailed != 0;
}
