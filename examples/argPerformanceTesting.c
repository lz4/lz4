/*
 * Temporary example to confirm/disprove a performance optimization I believe might exist by re-using arguments instead of making
 * LZ4_compress_generic re-evaluate them repeatedly between calls.
 */

/* Since lz4 compiles with c99 we need to enable posix linking for time.h structs and functions. */
#if __STDC_VERSION__ >= 199901L
#define _XOPEN_SOURCE 600
#else
#define _XOPEN_SOURCE 500
#endif
#define _POSIX_C_SOURCE 199309L

/* Includes, for Power! */
#include "lz4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>   /* for atoi() */
#include <stdint.h>   /* for uint_types */
#include <time.h>     /* for clock_gettime() */

/* We need to know what one billion is for clock timing. */
#define BILLION 1000000000L


/*
 * Easy show-error-and-bail function.
 */
void run_screaming(const char *message, const int code) {
  printf("%s\n", message);
  exit(code);
  return;
}


/*
 * Centralize the usage function to keep main cleaner.
 */
void usage(const char *message) {
  printf("Usage: ./argPerformanceTesting <iterations>\n");
  run_screaming(message, 1);
  return;
}


/*
 * Runs the benchmark for LZ4_compress_default.
 */
uint64_t bench__LZ4_compress_default(const int iterations, const char *src, char *dst, const int src_size, const int max_dst_size) {
  int time_taken = 0;
  int rv = 0;
  struct timespec start, end;

  // Banner
  printf("Starting test:  bench__LZ4_compress_default\n");

  // Do one test just to sanity check that our invocation will work in the loop.
  rv = LZ4_compress_default(src, dst, src_size, max_dst_size);
  if (rv < 1)
    run_screaming("Couldn't run LZ4_compress_default... error code received was %d\n", rv);

  // Start the timer and begin the loop, no magic or setups here.
  clock_gettime(CLOCK_MONOTONIC, &start);
  for(int i=1; i<=iterations; i++) {
    LZ4_compress_default(src, dst, src_size, max_dst_size);
  }

  // Stop timer and return time taken.
  clock_gettime(CLOCK_MONOTONIC, &end);
  time_taken = BILLION *(end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;

  return time_taken;
}


/*
 * Runs the benchmark for LZ4_compress_fast.
 */
uint64_t bench__LZ4_compress_fast(const int iterations, const char *src, char *dst, const int src_size, const int max_dst_size) {
  int time_taken = 0;
  int rv = 0;
  struct timespec start, end;

  // Banner
  printf("Starting test:  bench__LZ4_compress_fast\n");

  // Do one test just to sanity check that our invocation will work in the loop.
  int acceleration = 1;
  rv = LZ4_compress_fast(src, dst, src_size, max_dst_size, acceleration);
  if (rv < 1)
    run_screaming("Couldn't run LZ4_compress_fast... error code received was %d\n", rv);

  // Start the timer and begin the loop, no magic or setups here.
  clock_gettime(CLOCK_MONOTONIC, &start);
  for(int i=1; i<=iterations; i++) {
    LZ4_compress_fast(src, dst, src_size, max_dst_size, acceleration);
  }

  // Stop timer and return time taken.
  clock_gettime(CLOCK_MONOTONIC, &end);
  time_taken = BILLION *(end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;

  return time_taken;
}



/*
 * Party time.  Let's run the tests now.
 */
int main(int argc, char **argv) {
  // Get and verify options.  This isn't user friendly but I don't care for a test.
  int iterations = atoi(argv[1]);
  if (argc < 2)
    usage("Must specify at least 1 argument.");
  if (iterations < 1)
    usage("Argument 1 (iterations) must be > 0.");

  // Setup source data to work with.  500 bytes.  Let LZ4 tell us the safest size for dst.
  const int src_size = 500;
  const int max_dst_size = LZ4_compressBound(src_size);
  const char *src                     = "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Etiam commodo maximus cursus. Suspendisse porta sapien quis euismod placerat. In pulvinar dolor mi, congue pharetra ex porttitor ut. Aliquam cursus iaculis dui quis pulvinar. Aliquam non diam et ex semper finibus ut vel metus. Integer at egestas sapien. Fusce in ultrices turpis, ac vulputate risus. Donec vel erat cursus, ullamcorper augue nec, consequat urna. Sed diam dolor, egestas vitae massa a, elementum malesuada neque. Aliquam id sed.";
  //const char *highly_compressible_src = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  char *dst = malloc(max_dst_size);

  /* Run tests.  Call stack (before theoretical compiler optimizations) is as follows:
   * LZ4_compress_default
   *   LZ4_compress_fast
   *     LZ4_compress_fast_extState
   *       LZ4_compress_generic
   */

  //  Test 1:  Call LZ4_compress_default.  No expectations here obviously.
  uint64_t time_taken__default = bench__LZ4_compress_default(iterations, src, dst, src_size, max_dst_size);

  // Test 2:  Call LZ4_compress_fast.  Compiler should have optimized the 'acceleration' constant expression (1), so no gains expected here.
  uint64_t time_taken__fast = bench__LZ4_compress_default(iterations, src, dst, src_size, max_dst_size);

  // Test 3:  Call LZ4_compress_fast_extState

  // Test 4:  Call LZ4_compress_generic

  // Test 5:  Call LZ4_compress_generic_TESTING

  // Report and leave.
  const char *format        = "%-24s%16.9f%16d%16d\n";
  const char *header_format = "%-24s%16s%16s%16s\n";
  printf("\n");
  printf(header_format, "Function Benchmarked", "Total Seconds", "Iterations/sec", "ns/Iteration");
  printf(format, "LZ4_compress_default()", (float)time_taken__default / BILLION, (int)(iterations / ((float)time_taken__default/BILLION)), time_taken__default / iterations);
  printf(format, "LZ4_compress_fast()",    (float)time_taken__fast    / BILLION, (int)(iterations / ((float)time_taken__fast   /BILLION)), time_taken__fast    / iterations);
  printf("\n");
  printf("All done, ran %d iterations.\n", iterations);
  return 0;
}
