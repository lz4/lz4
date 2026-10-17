/*
 * xxHash RVV Performance Benchmark
 *
 * This benchmark measures the performance improvement of RVV-optimized
 * xxHash implementation compared to the scalar version.
 *
 * Copyright (C) 2024 LZ4 contributors
 */

/* Undefine namespace to use standard xxHash function names */
#ifdef XXH_NAMESPACE
#undef XXH_NAMESPACE
#endif

#define XXH_STATIC_LINKING_ONLY
#include "../lib/xxhash.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>

/* Use standard types for compatibility */
typedef uint32_t U32;
typedef uint64_t U64;

#define BENCH_SIZE (10 * 1024 * 1024)  /* 10 MB */
#define BENCH_ITERATIONS 100

static double get_time_sec(void)
{
#ifdef _POSIX_C_SOURCE
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#else
    return (double)clock() / CLOCKS_PER_SEC;
#endif
}

static void benchmark_xxh32(const void* data, size_t size, U32 seed)
{
    printf("XXH32 Benchmark (%.2f MB, %d iterations):\n",
           size / (1024.0 * 1024.0), BENCH_ITERATIONS);

    double start = get_time_sec();
    volatile U32 result;  /* Prevent optimization */

    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        result = XXH32(data, size, seed);
    }

    double elapsed = get_time_sec() - start;
    double throughput = (size * BENCH_ITERATIONS) / (elapsed * 1024 * 1024);

    printf("  Time:    %.3f seconds\n", elapsed);
    printf("  Throughput: %.2f MB/s\n", throughput);
    printf("  Result:  %08x (verification)\n\n", result);
}

static void benchmark_xxh64(const void* data, size_t size, U64 seed)
{
    printf("XXH64 Benchmark (%.2f MB, %d iterations):\n",
           size / (1024.0 * 1024.0), BENCH_ITERATIONS);

    double start = get_time_sec();
    volatile U64 result;

    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        result = XXH64(data, size, seed);
    }

    double elapsed = get_time_sec() - start;
    double throughput = (size * BENCH_ITERATIONS) / (elapsed * 1024 * 1024);

    printf("  Time:    %.3f seconds\n", elapsed);
    printf("  Throughput: %.2f MB/s\n", throughput);
    printf("  Result:  %016llx (verification)\n\n", (unsigned long long)result);
}

static void prepare_test_data(unsigned char* buffer, size_t size)
{
    /* Fill with various patterns to simulate real-world data */
    size_t i;

    /* 25% highly compressible (repeated) */
    for (i = 0; i < size / 4; i++) {
        buffer[i] = 'A';
    }

    /* 25% sequential pattern */
    for (; i < size / 2; i++) {
        buffer[i] = (unsigned char)(i & 0xff);
    }

    /* 50% pseudo-random */
    unsigned int seed = 12345;
    for (; i < size; i++) {
        seed = seed * 1103515245 + 12345;
        buffer[i] = (unsigned char)(seed >> 16);
    }
}

int main(void)
{
    printf("========================================\n");
    printf("xxHash RVV Performance Benchmark\n");
    printf("========================================\n\n");

#if XXH_ENABLE_RVV
    printf("RVV: ENABLED (vectorized implementation)\n");
#else
    printf("RVV: DISABLED (scalar implementation)\n");
#endif

    printf("Data size: %.2f MB\n", BENCH_SIZE / (1024.0 * 1024.0));
    printf("Iterations: %d\n\n", BENCH_ITERATIONS);

    /* Allocate and prepare test buffer */
    unsigned char* buffer;
#ifdef __STDC_VERSION__
#if __STDC_VERSION__ >= 199901L
    buffer = malloc(BENCH_SIZE);
#else
    buffer = (unsigned char*)malloc(BENCH_SIZE);
#endif
#else
    buffer = (unsigned char*)malloc(BENCH_SIZE);
#endif

    if (!buffer) {
        fprintf(stderr, "Error: Failed to allocate test buffer\n");
        return 1;
    }

    prepare_test_data(buffer, BENCH_SIZE);

    /* Run benchmarks */
    benchmark_xxh32(buffer, BENCH_SIZE, 0);
    benchmark_xxh64(buffer, BENCH_SIZE, 0);

    free(buffer);

    printf("========================================\n");
    printf("Benchmark completed\n");
    printf("========================================\n");

    return 0;
}
