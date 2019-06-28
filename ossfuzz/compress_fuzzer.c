#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "lz4.h"

#define CHECK(COND)   if (!(COND)) { abort(); }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  size_t const compressed_dest_size = LZ4_compressBound(size);
  char *const dest_buffer = (char *)malloc(compressed_dest_size);

  CHECK(dest_buffer != NULL);

  // Allocation succeeded, try compressing the incoming data.
  int result = LZ4_compress_default((const char*)data,
                                    dest_buffer,
                                    size,
                                    compressed_dest_size);
  CHECK(result != 0);

  free(dest_buffer);

  return 0;
}
