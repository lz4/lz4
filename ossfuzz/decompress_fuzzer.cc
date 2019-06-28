#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "lz4.h"

#define CHECK(COND)   if (!(COND)) { abort(); }

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  size_t const buffer_size = 10 * 1024 * 1024;
  char *const dest_buffer = (char *)malloc(buffer_size);

  if (dest_buffer != NULL)
  {
    // Allocation succeeded, try decompressing the incoming data.
    int result = LZ4_decompress_safe((const char*)data,
                                     dest_buffer,
                                     size,
                                     buffer_size);

    // Ignore the result of decompression.
    (void)result;

    free(dest_buffer);
  }

  return 0;
}
