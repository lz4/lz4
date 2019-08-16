#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

FUZZ_STATIC uint32_t FUZZ_produceUint32Range(uint8_t *data, size_t size,
                                             uint32_t min, uint32_t max) {
  if (min > max) {
    return 0;
  }

  uint32_t range = max - min;
  uint32_t rolling = range;
  uint32_t result = 0;

  while (rolling > 0 && size > 0) {
    uint8_t next = *(data + size - 1);
    size -= 1;
    result = (result << 8) | next;
  }

  if (range == 0xffffffff) {
    return result;
  }

  return min + result % (range + 1);
}
