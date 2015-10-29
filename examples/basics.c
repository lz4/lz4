/*
 * basics.c
 * Copyright  : Kyle Harper
 * License    : Follows same licensing as the lz4.c/lz4.h program at any given time.  Currently, BSD 2.
 * Description: Example program to demonstrate the basic usage of the compress/decompress functions within lz4.c/lz4.h.
 *              The functions you'll likely want are LZ4_compress_default and LZ4_decompress_fast.  Both of these are documented in
 *              the lz4.h header file; I recommend reading them.
 */

/* Includes, for Power! */
#include "lz4.h"    // This is all that is required to expose the prototypes for basic compression and decompression.
#include <stdio.h>  // For printf()
#include <string.h> // For memcmp()
#include <stdlib.h> // For exit()

/*
 * Easy show-error-and-bail function.
 */
void run_screaming(const char *message, const int code) {
  printf("%s\n", message);
  exit(code);
  return;
}


/*
 * main
 */
int main(void) {
  /* Introduction */
  // Below we will have a Compression and Decompression section to demonstrate.  There are a few important notes before we start:
  //   1) The return codes of LZ4_ functions are important.  Read lz4.h if you're unsure what a given code means.
  //   2) LZ4 uses char* pointers in all LZ4_ functions.  This is baked into the API and probably not going to change.  If your
  //      program uses pointers that are unsigned char*, void*, or otherwise different you may need to do some casting or set the
  //      right -W compiler flags to ignore those warnings (e.g.: -Wno-pointer-sign).

  /* Compression */
  // We'll store some text into a variable pointed to by *src to be compressed later.
  const char *src = "Lorem ipsum dolor sit amet, consectetur adipiscing elit.";
  // The compression function needs to know how many bytes of data we're sending.  The string above has 57 characters == 57 bytes.
  const int src_size = 57;
  // LZ4 provides a function that will tell you the maximum size of compressed output based on input data via LZ4_compressBound().
  const int max_dst_size = LZ4_compressBound(src_size);
  // We will use that size for our destination boundary when allocating space.
  char *compressed_data = malloc(max_dst_size);
  if (compressed_data == NULL)
    run_screaming("Failed to allocate memory for *compressed_data.", 1);
  // That's all the information and preparation LZ4 needs to compress *src into *compressed_data.  Invoke LZ4_compress_default now
  // with our size values and pointers to our memory locations.  Save the return value for error checking.
  int return_value = 0;
  return_value = LZ4_compress_default(src, compressed_data, src_size, max_dst_size);
  // Check return_value to determine what happened.
  if (return_value < 0)
    run_screaming("A negative result from LZ4_compress_default indicates a failure trying to compress the data.  See exit code (echo $?) for value returned.", return_value);
  if (return_value == 0)
    run_screaming("A result of 0 means compression worked, but was stopped because the destination buffer couldn't hold all the information.", 1);
  if (return_value > 0)
    printf("We successfully compressed some data!\n");
  // Not only does a positive return_value mean success, the value returned == the number of bytes required.  You can use this to
  // realloc() *compress_data to free up memory, if desired.  We'll do so just to demonstrate the concept.
  const int compressed_data_size = return_value;
  compressed_data = (char *)realloc(compressed_data, compressed_data_size);
  if (compressed_data == NULL)
    run_screaming("Failed to re-alloc memory for compressed_data.  Sad :(", 1);

  /* Decompression */
  // Now that we've successfully compressed the information from *src to *compressed_data, let's do the opposite!  We'll create a
  // *new_src location of size src_size since we know that value.
  char *new_src = malloc(src_size);
  if (new_src == NULL)
    run_screaming("Failed to allocate memory for *new_src.", 1);
  // The LZ4_decompress_fast function needs to know where the compressed data is, where the new_src memory location is, and how
  // large the new_src (uncompressed) output will be.  Again, save the return_value.
  return_value = LZ4_decompress_fast(compressed_data, new_src, src_size);
  if (return_value < 0)
    run_screaming("A negative result from LZ4_decompress_fast indicates a failure trying to decompress the data.  See exit code (echo $?) for value returned.", return_value);
  if (return_value == 0)
    run_screaming("I'm not sure this function can ever return 0.  Documentation in lz4.h doesn't indicate so.", 1);
  if (return_value > 0)
    printf("We successfully decompressed some data!\n");
  // Not only does a positive return value mean success, the value returned == the number of bytes read from the compressed_data
  // stream.  I'm not sure there's ever a time you'll need to know this in most cases...

  /* Validation */
  // We should be able to compare our original *src with our *new_src and be byte-for-byte identical.
  if (memcmp(src, new_src, src_size) != 0)
    run_screaming("Validation failed.  *src and *new_src are not identical.", 1);
  printf("Validation done.  The string we ended up with is:\n%s\n", new_src);
  return 0;
}
