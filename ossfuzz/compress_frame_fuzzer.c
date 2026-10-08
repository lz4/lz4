/**
 * This fuzz target attempts to compress the fuzzed data with the simple
 * compression function with an output buffer that may be too small to
 * ensure that the compressor never crashes.
 * Compression must succeed if and only if the frame fits,
 * and then produce the same frame as with a bound-sized buffer.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz_helpers.h"
#include "lz4.h"
#include "lz4frame.h"
#include "lz4frame_static.h"
#include "lz4_helpers.h"
#include "fuzz_data_producer.h"

static void checkCapacity(const uint8_t* src, size_t srcSize,
                          const LZ4F_preferences_t* prefs,
                          const char* ref, size_t refSize,
                          size_t dstCapacity)
{
    /* exact size, to detect overflows */
    char* const dst = (char*)malloc(dstCapacity ? dstCapacity : 1);
    FUZZ_ASSERT(dst != NULL);

    size_t const dstSize =
            LZ4F_compressFrame(dst, dstCapacity, src, srcSize, prefs);
    if (dstCapacity >= refSize) {
        FUZZ_ASSERT_MSG(dstSize == refSize, "Compression must succeed when the frame fits");
        FUZZ_ASSERT_MSG(!memcmp(dst, ref, refSize), "Frame must not depend on capacity");
    } else {
        FUZZ_ASSERT_MSG(LZ4F_getErrorCode(dstSize) == LZ4F_ERROR_dstCapacity_tooSmall,
                        "Compression must fail when the frame doesn't fit");
    }

    free(dst);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_dataProducer_t *producer = FUZZ_dataProducer_create(data, size);
    LZ4F_preferences_t const prefs = FUZZ_dataProducer_preferences(producer);
    size_t const dstCapacitySeed = FUZZ_dataProducer_retrieve32(producer);
    size_t const smallerSeed = FUZZ_dataProducer_retrieve32(producer);
    size = FUZZ_dataProducer_remainingBytes(producer);

    size_t const compressBound = LZ4F_compressFrameBound(size, &prefs);
    size_t const dstCapacity = FUZZ_getRange_from_uint32(dstCapacitySeed, 0, compressBound);

    char* const ref = (char*)malloc(compressBound);
    char* const rt = (char*)malloc(size ? size : 1);

    FUZZ_ASSERT(ref!=NULL);
    FUZZ_ASSERT(rt!=NULL);

    /* Compression into a bound-sized buffer must succeed and round trip correctly. */
    size_t const refSize =
            LZ4F_compressFrame(ref, compressBound, data, size, &prefs);
    FUZZ_ASSERT(!LZ4F_isError(refSize));
    {   size_t const rtSize = FUZZ_decompressFrame(rt, size, ref, refSize);
        FUZZ_ASSERT_MSG(rtSize == size, "Incorrect regenerated size");
        FUZZ_ASSERT_MSG(!memcmp(data, rt, size), "Corruption!");
    }

    checkCapacity(data, size, &prefs, ref, refSize, dstCapacity);
    checkCapacity(data, size, &prefs, ref, refSize, refSize);
    checkCapacity(data, size, &prefs, ref, refSize, refSize - 1);
    checkCapacity(data, size, &prefs, ref, refSize, refSize - 1 - (smallerSeed % refSize));

    free(ref);
    free(rt);
    FUZZ_dataProducer_free(producer);

    return 0;
}
