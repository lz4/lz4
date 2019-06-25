#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "lz4.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  size_t const compressed_dest_size = LZ4_compressBound(size);
  char *const dest_buffer = (char *)malloc(compressed_dest_size);

  int result = LZ4_compress_default((const char*)data, dest_buffer,
                                    size, compressed_dest_size);

  if (result == 0)
  {
    abort();
  }

  free(dest_buffer);

  return 0;
}
