#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "fuzz_helpers.h"

typedef struct {
  const uint8_t *data;
  size_t size;
} FUZZ_dataProducer_t;

FUZZ_dataProducer_t *FUZZ_dataProducer_create(const uint8_t *data, size_t size);

void FUZZ_dataProducer_free(FUZZ_dataProducer_t *producer);

uint32_t FUZZ_dataProducer_uint32(FUZZ_dataProducer_t *producer, uint32_t min,
                                  uint32_t max);
