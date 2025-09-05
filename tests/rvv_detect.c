/*
 * rvv_detect.c - RISC-V Vector Extension detection and analysis tool
 * Copyright (C) 2024 LZ4 contributors
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__riscv_vector)
#include <riscv_vector.h>
#endif

static void print_rvv_info(void) {
    printf("RISC-V Vector Extension Information\n");
    printf("==================================\n");
    
#if defined(__riscv_vector)
    printf("RVV Support: ENABLED (compile-time)\n");
    
    #ifdef __riscv_v_min_vlen
    printf("Minimum VLEN: %d bits\n", __riscv_v_min_vlen);
    #endif
    
    #ifdef __riscv_v_elen
    printf("Maximum ELEN: %d bits\n", __riscv_v_elen);
    #endif
    
    #ifdef __riscv_v_elen_fp
    printf("Maximum ELEN (FP): %d bits\n", __riscv_v_elen_fp);
    #endif
    
    /* Runtime VLEN detection */
    size_t vl_max_e8 = vsetvlmax_e8m1();
    size_t vl_max_e32 = vsetvlmax_e32m1();
    
    printf("Runtime VLEN: %zu bits (detected via vsetvlmax)\n", vl_max_e8 * 8);
    printf("Max vector length (e8m1): %zu elements\n", vl_max_e8);
    printf("Max vector length (e32m1): %zu elements\n", vl_max_e32);
    
#else
    printf("RVV Support: DISABLED\n");
    printf("Reason: __riscv_vector not defined\n");
#endif
    
    printf("\n");
}

static void print_optimization_status(void) {
    printf("LZ4 RVV Optimization Status\n");
    printf("===========================\n");
    
#if defined(__riscv_vector)
    printf("[ENABLED] LZ4_count - String matching\n");
    printf("[ENABLED] LZ4_wildCopy8/32 - Memory copying\n");
    printf("[ENABLED] LZ4HC_countBack - Backward counting\n");
    printf("[ENABLED] LZ4HC_reverseCountPattern - Pattern matching\n");
    printf("[ENABLED] XXH32/XXH64 - Hash computation\n");
    printf("[ENABLED] LZ4HC_Insert - Hash table updates\n");
    printf("[ENABLED] LZ4_renormDictT - Hash table renormalization\n");
    printf("[ENABLED] LZ4_loadDict_internal - Dictionary loading\n");
#else
    printf("[DISABLED] All optimizations disabled (RVV not available)\n");
#endif
    
    printf("\n");
}

static void run_simple_performance_test(void) {
    printf("Simple RVV Performance Test\n");
    printf("===========================\n");
    
#if defined(__riscv_vector)
    const size_t test_size = 1024;
    unsigned char *src = malloc(test_size);
    unsigned char *dst = malloc(test_size);
    
    if (!src || !dst) {
        printf("Memory allocation failed\n");
        goto cleanup;
    }
    
    /* Fill source with test pattern */
    for (size_t i = 0; i < test_size; i++) {
        src[i] = (unsigned char)(i % 256);
    }
    
    /* Test vector memory copy */
    printf("Testing vector memory copy...\n");
    
    size_t pos = 0;
    while (pos < test_size) {
        size_t vl = vsetvl_e8m1(test_size - pos);
        vuint8m1_t vec = vle8_v_u8m1(&src[pos], vl);
        vse8_v_u8m1(&dst[pos], vec, vl);
        pos += vl;
    }
    
    /* Verify results */
    int errors = 0;
    for (size_t i = 0; i < test_size; i++) {
        if (src[i] != dst[i]) {
            errors++;
        }
    }
    
    printf("Vector copy test: %s (%d errors)\n", 
           errors == 0 ? "PASSED" : "FAILED", errors);
    
cleanup:
    free(src);
    free(dst);
#else
    printf("RVV not available - test skipped\n");
#endif
    
    printf("\n");
}

static void print_compiler_info(void) {
    printf("Compiler Information\n");
    printf("===================\n");
    
#ifdef __GNUC__
    printf("Compiler: GCC %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(__clang__)
    printf("Compiler: Clang %d.%d.%d\n", __clang_major__, __clang_minor__, __clang_patchlevel__);
#else
    printf("Compiler: Unknown\n");
#endif

#ifdef __riscv
    printf("Architecture: RISC-V\n");
    #ifdef __riscv_xlen
    printf("XLEN: %d bits\n", __riscv_xlen);
    #endif
#else
    printf("Architecture: Not RISC-V\n");
#endif

    printf("Build date: %s %s\n", __DATE__, __TIME__);
    printf("\n");
}

int main(int argc, char* argv[]) {
    printf("LZ4 RISC-V Vector Extension Detection Tool\n");
    printf("==========================================\n\n");
    
    print_compiler_info();
    print_rvv_info();
    print_optimization_status();
    
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        run_simple_performance_test();
    } else {
        printf("Use --test to run performance tests\n");
    }
    
    return 0;
}