#include <stdint.h>
#include <stdio.h>

#include "fuzz_data_producer.h"

static int expect32(uint32_t actual, uint32_t expected, const char* name)
{
    if (actual == expected) return 0;
    fprintf(stderr, "%s: expected 0x%08x, got 0x%08x\n", name, (unsigned)expected, (unsigned)actual);
    return 1;
}

int main(void)
{
    static const uint8_t data[]   = {0xAA, 0x01, 0x23, 0x45, 0x67};
    FUZZ_dataProducer_t* producer = FUZZ_dataProducer_create(data, sizeof(data));
    int                  result   = 0;

    result |= expect32(FUZZ_dataProducer_retrieve32(producer), 0x67452301U, "four-byte value");
    result |= expect32((uint32_t)FUZZ_dataProducer_remainingBytes(producer), 1U, "four-byte consumption");
    result |= expect32(FUZZ_dataProducer_retrieve32(producer), 0xAAU, "single-byte value");
    result |= expect32((uint32_t)FUZZ_dataProducer_remainingBytes(producer), 0U, "single-byte consumption");
    result |= expect32(FUZZ_dataProducer_retrieve32(producer), 0U, "empty value");

    FUZZ_dataProducer_free(producer);
    return result;
}
