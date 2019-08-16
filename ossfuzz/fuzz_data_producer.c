#include "fuzz_data_producer.h"

FUZZ_dataProducer_t *FUZZ_dataProducer_create(const uint8_t *data, size_t size) {
  FUZZ_dataProducer_t *producer = malloc(sizeof(FUZZ_dataProducer_t));
  producer->data = data;
  producer->size = size;
  return producer;
}

void FUZZ_dataProducer_free(FUZZ_dataProducer_t *producer) { free(producer); }

uint32_t FUZZ_dataProducer_uint32(FUZZ_dataProducer_t *producer, uint32_t min,
                                  uint32_t max) {
  FUZZ_ASSERT(min <= max);

  uint32_t range = max - min;
  uint32_t rolling = range;
  uint32_t result = 0;

  while (rolling > 0 && producer->size > 0) {
    uint8_t next = *(producer->data + producer->size - 1);
    producer->size -= 1;
    result = (result << 8) | next;
    rolling >>= 8;
  }

  if (range == 0xffffffff) {
    return result;
  }

  return min + result % (range + 1);
}
