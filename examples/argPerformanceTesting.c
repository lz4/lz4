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
#include <inttypes.h> /* for PRIu64 */
#include <time.h>     /* for clock_gettime() */

/* We need to know what one billion is for clock timing. */
#define BILLION 1000000000L

/* Create a crude set of test IDs so we can switch on them later. */
#define ID__LZ4_COMPRESS_DEFAULT        1
#define ID__LZ4_COMPRESS_FAST           2
#define ID__LZ4_COMPRESS_FAST_EXTSTATE  3
#define ID__LZ4_COMPRESS_GENERIC        4



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
 * Runs the benchmark for LZ4_compress_* based on function_id.
 */
uint64_t bench(char *known_good_dst, const int function_id, const int iterations, const char *src, char *dst, const int src_size, const int max_dst_size) {
  uint64_t time_taken = 0;
  int rv = 0;
  struct timespec start, end;
  const int acceleration = 1;
  LZ4_stream_t state;

  // Select the right function to perform the benchmark on.  For simplicity, start the timer here.  We perform 2 initial loops to
  // ensure that dst remains matching to known_good_dst between successive calls.
  clock_gettime(CLOCK_MONOTONIC, &start);
  switch(function_id) {
    case ID__LZ4_COMPRESS_DEFAULT:
      printf("Starting benchmark for function: LZ4_compress_default()\n");
      for(int junk=0; junk<2; junk++) {
        rv = LZ4_compress_default(src, dst, src_size, max_dst_size);
        if (rv < 1)
          run_screaming("Couldn't run LZ4_compress_default()... error code received is in exit code.", rv);
        if (memcmp(known_good_dst, dst, max_dst_size) != 0)
          run_screaming("According to memcmp(), the compressed dst we got doesn't match the known_good_dst... ruh roh.", 1);
      }
      for (int i=1; i<=iterations; i++)
        LZ4_compress_default(src, dst, src_size, max_dst_size);
      break;

    case ID__LZ4_COMPRESS_FAST:
      printf("Starting benchmark for function: LZ4_compress_fast()\n");
      for(int junk=0; junk<2; junk++) {
        rv = LZ4_compress_fast(src, dst, src_size, max_dst_size, acceleration);
        if (rv < 1)
          run_screaming("Couldn't run LZ4_compress_fast()... error code received is in exit code.", rv);
        if (memcmp(known_good_dst, dst, max_dst_size) != 0)
          run_screaming("According to memcmp(), the compressed dst we got doesn't match the known_good_dst... ruh roh.", 1);
      }
      for (int i=1; i<=iterations; i++)
        LZ4_compress_fast(src, dst, src_size, max_dst_size, acceleration);
      break;

    case ID__LZ4_COMPRESS_FAST_EXTSTATE:
      printf("Starting benchmark for function: LZ4_compress_fast_extState()\n");
      for(int junk=0; junk<2; junk++) {
        rv = LZ4_compress_fast_extState(&state, src, dst, src_size, max_dst_size, acceleration);
        if (rv < 1)
          run_screaming("Couldn't run LZ4_compress_fast_extState()... error code received is in exit code.", rv);
        if (memcmp(known_good_dst, dst, max_dst_size) != 0)
          run_screaming("According to memcmp(), the compressed dst we got doesn't match the known_good_dst... ruh roh.", 1);
      }
      for (int i=1; i<=iterations; i++)
        LZ4_compress_fast_extState(&state, src, dst, src_size, max_dst_size, acceleration);
      break;

    case ID__LZ4_COMPRESS_GENERIC:
      printf("Starting benchmark for function: LZ4_compress_generic()\n");
      LZ4_resetStream((LZ4_stream_t*)&state);
      for(int junk=0; junk<2; junk++) {
        LZ4_resetStream((LZ4_stream_t*)&state);
        rv = LZ4_compress_generic_wrapper(&state, src, dst, src_size, acceleration);
        if (rv < 1)
          run_screaming("Couldn't run LZ4_compress_generic()... error code received is in exit code.", rv);
        if (memcmp(known_good_dst, dst, max_dst_size) != 0)
          run_screaming("According to memcmp(), the compressed dst we got doesn't match the known_good_dst... ruh roh.", 1);
      }
      for (int i=1; i<=iterations; i++) {
        LZ4_resetStream((LZ4_stream_t*)&state);
        LZ4_compress_generic_wrapper(&state, src, dst, src_size, acceleration);
      }
      break;

    default:
      run_screaming("The test specified isn't valid.  Please check your code.", 1);
      break;
  }

  // Stop timer and return time taken.
  clock_gettime(CLOCK_MONOTONIC, &end);
  time_taken = BILLION *(end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;

  return time_taken;
}



/*
 * Run tests.  Call stack (before theoretical compiler optimizations) for LZ4_compress_default is as follows:
 * LZ4_compress_default
 *   LZ4_compress_fast
 *     LZ4_compress_fast_extState
 *       LZ4_compress_generic
 *
 * Test suite A)  Uses generic Lorem Ipsum text which should be generally compressible insomuch as basic human text is
 *                compressible for such a small src_size
 * Test Suite B)  For the sake of testing, see what results we get if the data is drastically easier to compress.  IF there are
 *                indeed losses and IF more compressible data is faster to process, this will exacerbate the findings.
 *
 * Test 1)  LZ4_compress_default.
 *   A.  No expectations here obviously.
 * Test 2)  LZ4_compress_fast.
 *   A.  Compiler should have optimized the 'acceleration' constant expression (1), so no gains are expected here.  That
 *       said, if nothing else ever calls this with a different acceleration, it could be eliminated.
 * Test 3)  LZ4_compress_fast_extState.
 *   A.  This requires an LZ4_stream_t struct to track the compression stream, however it is initialized via a call to
 *       LZ4_resetStream() which ultimately just memset()s the memeory to 0.  Avoiding creating this struct repeatedly
 *       might yield a minor improvement, but I doubt it.
 *   B.  There is then an integer check on acceleration that has to run each iteration.  (safety check)
 *   C.  A call to LZ4_compressBound() is required to determine if output should be limited .  (safety check)
 * Test 4)  LZ4_compress_generic.
 *   !.  This is a STATIC INLINE function, which means it's probably slurped into the caller: LZ4_compress_fast_extState.
 *       There's likely nothing to test here but I'll create a wrapper to test it out.
 *   A.  Calling this directly does NOT allow us to avoid calling resetStream() each time, but we do get to call it with
 *       our own local variable which might help... or not.
 *   B.  We can avoid checking acceleration each time, which isn't very helpful.
 *   C.  Since we can guarantee a few things we will avoid a few if..else checks.  But this isn't fair as the function is
 *       serving a purpose and we're avoiding that safety check.
 */
int main(int argc, char **argv) {
  // Get and verify options.  This isn't user friendly but I don't care for a test.
  int iterations = atoi(argv[1]);
  if (argc < 2)
    usage("Must specify at least 1 argument.");
  if (iterations < 1)
    usage("Argument 1 (iterations) must be > 0.");

  // Setup source data to work with.  500 bytes.  Let LZ4 tell us the safest size for dst.
  const int src_size = 2000;
  const int max_dst_size = LZ4_compressBound(src_size);
  const char *src    = "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed luctus purus et risus vulputate, et mollis orci ullamcorper. Nulla facilisi. Fusce in ligula sed purus varius aliquet interdum vitae justo. Proin quis diam velit. Nulla varius iaculis auctor. Cras volutpat, justo eu dictum pulvinar, elit sem porttitor metus, et imperdiet metus sapien et ante. Nullam nisi nulla, ornare eu tristique eu, dignissim vitae diam. Nulla sagittis porta libero, a accumsan felis sagittis scelerisque.  Integer laoreet eleifend congue. Etiam rhoncus leo vel dolor fermentum, quis luctus nisl iaculis. Praesent a erat sapien. Aliquam semper mi in lorem ultrices ultricies. Lorem ipsum dolor sit amet, consectetur adipiscing elit. In feugiat risus sed enim ultrices, at sodales nulla tristique. Maecenas eget pellentesque justo, sed pellentesque lectus. Fusce sagittis sit amet elit vel varius. Donec sed ligula nec ligula vulputate rutrum sed ut lectus. Etiam congue pharetra leo vitae cursus. Morbi enim ante, porttitor ut varius vel, tincidunt quis justo. Nunc iaculis, risus id ultrices semper, metus est efficitur ligula, vel posuere risus nunc eget purus. Ut lorem turpis, condimentum at sem sed, porta aliquam turpis. In ut sapien a nulla dictum tincidunt quis sit amet lorem. Fusce at est egestas, luctus neque eu, consectetur tortor. Phasellus eleifend ultricies nulla ac lobortis.  Morbi maximus quam cursus vehicula iaculis. Maecenas cursus vel justo ut rutrum. Curabitur magna orci, dignissim eget dapibus vitae, finibus id lacus. Praesent rhoncus mattis augue vitae bibendum. Praesent porta mauris non ultrices fermentum. Quisque vulputate ipsum in sodales pulvinar. Aliquam nec mollis felis. Donec vitae augue pulvinar, congue nisl sed, pretium purus. Fusce lobortis mi ac neque scelerisque semper. Pellentesque vel est vitae magna aliquet aliquet. Nam non dolor. Nulla facilisi. Class aptent taciti sociosqu ad litora torquent per conubia nostra, per inceptos himenaeos. Morbi ac lacinia felis metus.";
  const char *hc_src = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  char *dst               = calloc(1, max_dst_size);
  char *known_good_dst    = calloc(1, max_dst_size);
  char *known_good_hc_dst = calloc(1, max_dst_size);

  // Pre-load dst with the compressed data so we can memcmp() it in our bench() function to ensure we're getting matching responses outputs.
  if (LZ4_compress_default(src, known_good_dst, src_size, max_dst_size) < 0)
    run_screaming("Couldn't create a known-good destination buffer for comparison... this is bad.", 1);
  if (LZ4_compress_default(hc_src, known_good_hc_dst, src_size, max_dst_size) < 0)
    run_screaming("Couldn't create a known-good (highly compressible) destination buffer for comparison... this is bad.", 1);

  // Suite A - Normal Compressibility
  printf("\nStarting suite A:  Normal compressible text.\n");
  uint64_t time_taken__default       = bench(known_good_dst, ID__LZ4_COMPRESS_DEFAULT,       iterations, src, dst, src_size, max_dst_size);
  uint64_t time_taken__fast          = bench(known_good_dst, ID__LZ4_COMPRESS_FAST,          iterations, src, dst, src_size, max_dst_size);
  uint64_t time_taken__fast_extstate = bench(known_good_dst, ID__LZ4_COMPRESS_FAST_EXTSTATE, iterations, src, dst, src_size, max_dst_size);
  uint64_t time_taken__generic       = bench(known_good_dst, ID__LZ4_COMPRESS_GENERIC,       iterations, src, dst, src_size, max_dst_size);
  memset(dst, 0, max_dst_size);
  // Suite B - Highly Compressible
  printf("\nStarting suite B:  Highly compressible text.\n");
  uint64_t time_taken_hc__default       = bench(known_good_hc_dst, ID__LZ4_COMPRESS_DEFAULT,       iterations, hc_src, dst, src_size, max_dst_size);
  uint64_t time_taken_hc__fast          = bench(known_good_hc_dst, ID__LZ4_COMPRESS_FAST,          iterations, hc_src, dst, src_size, max_dst_size);
  uint64_t time_taken_hc__fast_extstate = bench(known_good_hc_dst, ID__LZ4_COMPRESS_FAST_EXTSTATE, iterations, hc_src, dst, src_size, max_dst_size);
  uint64_t time_taken_hc__generic       = bench(known_good_hc_dst, ID__LZ4_COMPRESS_GENERIC,       iterations, hc_src, dst, src_size, max_dst_size);

  // Report and leave.
  const char *format        = "|%-16s|%-32s|%16.9f|%16d|%16d|%13.2f%%|\n";
  const char *header_format = "|%-16s|%-32s|%16s|%16s|%16s|%14s|\n";
  const char *separator     = "+----------------+--------------------------------+----------------+----------------+----------------+--------------+\n";
  printf("\n");
  printf("%s", separator);
  printf(header_format, "Source", "Function Benchmarked", "Total Seconds", "Iterations/sec", "ns/Iteration", "% of default");
  printf("%s", separator);
  printf(format, "Normal Text", "LZ4_compress_default()",       (double)time_taken__default       / BILLION, (int)(iterations / ((double)time_taken__default       /BILLION)), time_taken__default       / iterations, (double)time_taken__default       * 100 / time_taken__default);
  printf(format, "Normal Text", "LZ4_compress_fast()",          (double)time_taken__fast          / BILLION, (int)(iterations / ((double)time_taken__fast          /BILLION)), time_taken__fast          / iterations, (double)time_taken__fast          * 100 / time_taken__default);
  printf(format, "Normal Text", "LZ4_compress_fast_extState()", (double)time_taken__fast_extstate / BILLION, (int)(iterations / ((double)time_taken__fast_extstate /BILLION)), time_taken__fast_extstate / iterations, (double)time_taken__fast_extstate * 100 / time_taken__default);
  printf(format, "Normal Text", "LZ4_compress_generic()",       (double)time_taken__generic       / BILLION, (int)(iterations / ((double)time_taken__generic       /BILLION)), time_taken__generic       / iterations, (double)time_taken__generic       * 100 / time_taken__default);
  printf(header_format, "", "", "", "", "", "");
  printf(format, "Compressible", "LZ4_compress_default()",       (double)time_taken_hc__default       / BILLION, (int)(iterations / ((double)time_taken_hc__default       /BILLION)), time_taken_hc__default       / iterations, (double)time_taken_hc__default       * 100 / time_taken_hc__default);
  printf(format, "Compressible", "LZ4_compress_fast()",          (double)time_taken_hc__fast          / BILLION, (int)(iterations / ((double)time_taken_hc__fast          /BILLION)), time_taken_hc__fast          / iterations, (double)time_taken_hc__fast          * 100 / time_taken_hc__default);
  printf(format, "Compressible", "LZ4_compress_fast_extState()", (double)time_taken_hc__fast_extstate / BILLION, (int)(iterations / ((double)time_taken_hc__fast_extstate /BILLION)), time_taken_hc__fast_extstate / iterations, (double)time_taken_hc__fast_extstate * 100 / time_taken_hc__default);
  printf(format, "Compressible", "LZ4_compress_generic()",       (double)time_taken_hc__generic       / BILLION, (int)(iterations / ((double)time_taken_hc__generic       /BILLION)), time_taken_hc__generic       / iterations, (double)time_taken_hc__generic       * 100 / time_taken_hc__default);
  printf("%s", separator);
  printf("\n");
  printf("All done, ran %d iterations per test.\n", iterations);
  return 0;
}
