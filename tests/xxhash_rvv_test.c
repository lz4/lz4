/*
 * xxHash RVV Correctness Test
 *
 * This test validates that the RVV-optimized xxHash implementation
 * produces identical results to the scalar reference implementation.
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
#include <string.h>
#include <stdint.h>

/* Use standard types for compatibility */
typedef uint32_t U32;
typedef uint64_t U64;

/* Standard xxHash test vectors from xxHash specification */
static const struct {
    const char* data;
    size_t len;
    U32 expected32;
    U64 expected64;
} test_vectors[] = {
    /* Basic test cases */
    {"", 0, 0x02cc5d05, 0xef46db3751d8e999ULL},
    {"a", 1, 0x550d7456, 0xd24ec4f1a98c6e5bULL},
    {"abc", 3, 0x32d153ff, 0x44bc2cf5ad770999ULL},
    {"hello world", 11, 0xcebb6622, 0x45ab6734b21e6968ULL},

    /* Additional test cases */
    {"hello", 5, 0xfb0077f9, 0x26c7827d889f6da3ULL},
    {"abcdefghijklmnopqrstuvwxyz", 26, 0x63a14d5f, 0xcfe1f278fa89835cULL},
    {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 62, 0x9c285e64, 0x89e6e0c4de39bca6ULL},
};

static int run_tests(void)
{
    int failed = 0;

    printf("Running xxHash RVV correctness tests...\n\n");

    for (size_t i = 0; i < sizeof(test_vectors) / sizeof(test_vectors[0]); i++) {
        const char* data = test_vectors[i].data;
        size_t len = test_vectors[i].len;
        U32 expected32 = test_vectors[i].expected32;
        U64 expected64 = test_vectors[i].expected64;

        U32 h32 = XXH32(data, len, 0);
        U64 h64 = XXH64(data, len, 0);

        int test_failed = 0;

        if (h32 != expected32) {
            printf("FAIL: Test %zu - XXH32 mismatch\n", i);
            printf("  Input: \"%s\" (len=%zu)\n", data, len);
            printf("  Expected: 0x%08x\n", expected32);
            printf("  Got:      0x%08x\n", h32);
            test_failed = 1;
        }

        if (h64 != expected64) {
            printf("FAIL: Test %zu - XXH64 mismatch\n", i);
            printf("  Input: \"%s\" (len=%zu)\n", data, len);
            printf("  Expected: 0x%016llx\n", (unsigned long long)expected64);
            printf("  Got:      0x%016llx\n", (unsigned long long)h64);
            test_failed = 1;
        }

        if (!test_failed) {
            printf("PASS: Test %zu (\"%.20s%s\", len=%zu)\n",
                   i, data, len > 20 ? "..." : "", len);
        } else {
            failed++;
        }
    }

    printf("\n");

    /* Test with different seeds */
    printf("Testing with different seeds...\n");
    const char* test_data = "test data";
    size_t test_len = strlen(test_data);

    for (U32 seed = 0; seed < 5; seed++) {
        U32 h32 = XXH32(test_data, test_len, seed);
        U64 h64 = XXH64(test_data, test_len, seed);
        printf("  seed=%u: XXH32=0x%08x XXH64=0x%016llx\n",
               seed, h32, (unsigned long long)h64);
    }

    printf("\n");

    return failed;
}

int main(void)
{
    printf("============================================\n");
    printf("xxHash RVV Correctness Validation Test\n");
    printf("============================================\n\n");

#if XXH_ENABLE_RVV
    printf("RVV: ENABLED\n");
#else
    printf("RVV: DISABLED (scalar implementation)\n");
#endif

    printf("\n");

    int failed = run_tests();

    printf("============================================\n");
    if (failed == 0) {
        printf("All tests PASSED\n");
        return 0;
    } else {
        printf("%d test(s) FAILED\n", failed);
        return 1;
    }
}
