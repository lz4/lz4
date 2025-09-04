/*
 * rvv_benchmark.c - RVV specific performance benchmarks
 * Copyright (C) 2024 LZ4 contributors
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "../lib/lz4.h"
#include "../lib/lz4hc.h"

#define BENCHMARK_SIZE (1024 * 1024)  /* 1MB */
#define ITERATIONS 100

static double get_time_diff_ms(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000.0 + 
           (end.tv_nsec - start.tv_nsec) / 1000000.0;
}

static void benchmark_compression_speed() {
    char* input = malloc(BENCHMARK_SIZE);
    char* compressed = malloc(LZ4_compressBound(BENCHMARK_SIZE));
    
    printf("RVV Compression Speed Benchmark\n");
    printf("================================\n");
    
    /* Generate test data - mix of patterns and random data */
    for (int i = 0; i < BENCHMARK_SIZE; i++) {
        if (i < BENCHMARK_SIZE / 4) {
            input[i] = 'A';  /* Highly compressible */
        } else if (i < BENCHMARK_SIZE / 2) {
            input[i] = (char)(i % 256);  /* Repetitive pattern */
        } else {
            input[i] = (char)(rand() % 256);  /* Random data */
        }
    }
    
    struct timespec start, end;
    
    /* Benchmark LZ4_compress_default */
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        LZ4_compress_default(input, compressed, BENCHMARK_SIZE, LZ4_compressBound(BENCHMARK_SIZE));
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double time_ms = get_time_diff_ms(start, end);
    double speed_mbs = (BENCHMARK_SIZE * ITERATIONS) / (time_ms / 1000.0) / (1024 * 1024);
    
    printf("LZ4_compress_default: %.2f MB/s (%.2f ms for %d iterations)\n", 
           speed_mbs, time_ms, ITERATIONS);
    
    /* Benchmark LZ4_compress_HC */
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        LZ4_compress_HC(input, compressed, BENCHMARK_SIZE, LZ4_compressBound(BENCHMARK_SIZE), 9);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    time_ms = get_time_diff_ms(start, end);
    speed_mbs = (BENCHMARK_SIZE * ITERATIONS) / (time_ms / 1000.0) / (1024 * 1024);
    
    printf("LZ4_compress_HC:      %.2f MB/s (%.2f ms for %d iterations)\n", 
           speed_mbs, time_ms, ITERATIONS);
    
    free(input);
    free(compressed);
}

static void benchmark_decompression_speed() {
    char* input = malloc(BENCHMARK_SIZE);
    char* compressed = malloc(LZ4_compressBound(BENCHMARK_SIZE));
    char* decompressed = malloc(BENCHMARK_SIZE);
    
    printf("\nRVV Decompression Speed Benchmark\n");
    printf("==================================\n");
    
    /* Generate test data */
    for (int i = 0; i < BENCHMARK_SIZE; i++) {
        input[i] = (char)(rand() % 256);
    }
    
    int compressed_size = LZ4_compress_default(input, compressed, BENCHMARK_SIZE, 
                                               LZ4_compressBound(BENCHMARK_SIZE));
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        LZ4_decompress_safe(compressed, decompressed, compressed_size, BENCHMARK_SIZE);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double time_ms = get_time_diff_ms(start, end);
    double speed_mbs = (BENCHMARK_SIZE * ITERATIONS) / (time_ms / 1000.0) / (1024 * 1024);
    
    printf("LZ4_decompress_safe:  %.2f MB/s (%.2f ms for %d iterations)\n", 
           speed_mbs, time_ms, ITERATIONS);
    printf("Compression ratio:    %.2f:1\n", 
           (double)BENCHMARK_SIZE / compressed_size);
    
    free(input);
    free(compressed);  
    free(decompressed);
}

int main() {
    printf("LZ4 RVV Performance Benchmark\n");
    printf("Build date: %s %s\n", __DATE__, __TIME__);
    
#ifdef __riscv_vector
    printf("RVV support: ENABLED\n");
#else
    printf("RVV support: DISABLED\n");
#endif
    
    printf("Data size: %d KB\n", BENCHMARK_SIZE / 1024);
    printf("Iterations: %d\n\n", ITERATIONS);
    
    srand((unsigned int)time(NULL));
    
    benchmark_compression_speed();
    benchmark_decompression_speed();
    
    return 0;
}