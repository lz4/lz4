/**
 * Unit test of the fuzz data producer (this is not a fuzzer).
 * Values are consumed from the end of the input :
 * 32-bit values are read from 4 bytes, in little-endian order.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "fuzz_helpers.h"
#include "fuzz_data_producer.h"

static uint32_t range32_of(const uint8_t* data, size_t size, uint32_t min, uint32_t max)
{
    FUZZ_dataProducer_t* const producer = FUZZ_dataProducer_create(data, size);
    uint32_t const value = FUZZ_dataProducer_range32(producer, min, max);
    FUZZ_dataProducer_free(producer);
    return value;
}

int main(void)
{
    {   uint8_t const data[] = { 0xAA, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
        FUZZ_dataProducer_t* const producer = FUZZ_dataProducer_create(data, sizeof(data));
        FUZZ_ASSERT(FUZZ_dataProducer_retrieve32(producer) == 0x08070605);
        FUZZ_ASSERT(FUZZ_dataProducer_remainingBytes(producer) == 5);
        FUZZ_ASSERT(FUZZ_dataProducer_retrieve32(producer) == 0x04030201);
        FUZZ_ASSERT(FUZZ_dataProducer_remainingBytes(producer) == 1);
        /* less than 4 bytes left : a single byte is consumed */
        FUZZ_ASSERT(FUZZ_dataProducer_retrieve32(producer) == 0xAA);
        FUZZ_ASSERT(FUZZ_dataProducer_remainingBytes(producer) == 0);
        /* nothing left */
        FUZZ_ASSERT(FUZZ_dataProducer_retrieve32(producer) == 0);
        FUZZ_dataProducer_free(producer);
    }

    /* range32() can produce any value within its range, not just small ones */
    {   uint8_t const seed[] = { 0x45, 0x23, 0x01, 0x00 };   /* 0x00012345 == 74565 */
        FUZZ_ASSERT(range32_of(seed, sizeof(seed), 1000, 1999) == 1000 + 74565 % 1000);
    }
    {   uint8_t const seed[] = { 0xFF, 0xFF, 0xFF, 0x7F };
        FUZZ_ASSERT(range32_of(seed, sizeof(seed), 0, 0xFFFFFFFF) == 0x7FFFFFFF);
    }

    printf("fuzz_data_producer_test : OK \n");
    return 0;
}
