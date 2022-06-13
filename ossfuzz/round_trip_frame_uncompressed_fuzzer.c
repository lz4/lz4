/**
 * This fuzz target performs a lz4 round-trip test (compress & decompress),
 * compares the result with the original, and calls abort() on corruption.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz_data_producer.h"
#include "fuzz_helpers.h"
#include "lz4.h"
#include "lz4_helpers.h"
#include "lz4frame.h"
#include "lz4frame_static.h"

static void decompress(LZ4F_dctx *dctx, void *src, void *dst,
                           size_t dstCapacity, size_t readSize) {
  size_t ret = 1;
  const void *srcPtr = (const char *)src;
  void *dstPtr = (char *)dst;
  const void *const srcEnd = (const char *)srcPtr + readSize;

  while (ret != 0) {
    while (srcPtr < srcEnd && ret != 0) {
      /* Any data within dst has been flushed at this stage */
      size_t dstSize = dstCapacity;
      size_t srcSize = (const char *)srcEnd - (const char *)srcPtr;
      ret = LZ4F_decompress(dctx, dstPtr, &dstSize, srcPtr, &srcSize,
                            /* LZ4F_decompressOptions_t */ NULL);
      FUZZ_ASSERT(!LZ4F_isError(ret));

      /* Update input */
      srcPtr = (const char *)srcPtr + srcSize;
      dstPtr = (char *)dstPtr + dstSize;
    }

    FUZZ_ASSERT(srcPtr <= srcEnd);
  }
}

static void compress_round_trip(const uint8_t* data, size_t size,
                                FUZZ_dataProducer_t *producer, LZ4F_preferences_t const prefs) {
  size = FUZZ_dataProducer_remainingBytes(producer);

  uint8_t *uncompressedData = malloc(size);
  size_t uncompressedOffset = rand() % (size + 1);

  FUZZ_dataProducer_t *uncompressedProducer =
      FUZZ_dataProducer_create(uncompressedData, size);
  size_t uncompressedSize =
      FUZZ_dataProducer_remainingBytes(uncompressedProducer);

  size_t const dstCapacity =
      LZ4F_compressFrameBound(LZ4_compressBound(size), &prefs) +
      uncompressedSize;
  char *const dst = (char *)malloc(dstCapacity);
  size_t rtCapacity = dstCapacity;
  char *const rt = (char *)malloc(rtCapacity);

  FUZZ_ASSERT(dst);
  FUZZ_ASSERT(rt);

  /* Compression must succeed and round trip correctly. */
  LZ4F_compressionContext_t ctx;
  size_t const ctxCreation = LZ4F_createCompressionContext(&ctx, LZ4F_VERSION);
  FUZZ_ASSERT(!LZ4F_isError(ctxCreation));

  size_t const headerSize = LZ4F_compressBegin(ctx, dst, dstCapacity, &prefs);
  FUZZ_ASSERT(!LZ4F_isError(headerSize));
  size_t compressedSize = headerSize;

  /* Compress data before uncompressed offset */
  size_t lz4Return = LZ4F_compressUpdate(ctx, dst + compressedSize, dstCapacity,
                                         data, uncompressedOffset, NULL);
  FUZZ_ASSERT(!LZ4F_isError(lz4Return));
  compressedSize += lz4Return;

  /* Add uncompressed data */
  lz4Return = LZ4F_uncompressedUpdate(ctx, dst + compressedSize, dstCapacity,
                                      uncompressedData, uncompressedSize, NULL);
  FUZZ_ASSERT(!LZ4F_isError(lz4Return));
  compressedSize += lz4Return;

  /* Compress data after uncompressed offset */
  lz4Return = LZ4F_compressUpdate(ctx, dst + compressedSize, dstCapacity,
                                  data + uncompressedOffset,
                                  size - uncompressedOffset, NULL);
  FUZZ_ASSERT(!LZ4F_isError(lz4Return));
  compressedSize += lz4Return;

  /* Finish compression */
  lz4Return = LZ4F_compressEnd(ctx, dst + compressedSize, dstCapacity, NULL);
  FUZZ_ASSERT(!LZ4F_isError(lz4Return));
  compressedSize += lz4Return;

  LZ4F_decompressOptions_t opts;
  memset(&opts, 0, sizeof(opts));
  opts.stableDst = 1;
  LZ4F_dctx *dctx;
  LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
  FUZZ_ASSERT(dctx);

  decompress(dctx, dst, rt, rtCapacity, compressedSize);

  LZ4F_freeDecompressionContext(dctx);

  char *const expectedData = (char *)malloc(size + uncompressedSize);
  memcpy(expectedData, data, uncompressedOffset);
  memcpy(expectedData + uncompressedOffset, uncompressedData, uncompressedSize);
  memcpy(expectedData + uncompressedOffset + uncompressedSize,
         data + uncompressedOffset, size - uncompressedOffset);

  FUZZ_ASSERT_MSG(!memcmp(expectedData, rt, size), "Corruption!");
  free(expectedData);

  free(dst);
  free(rt);
  free(uncompressedData);

  FUZZ_dataProducer_free(producer);
  FUZZ_dataProducer_free(uncompressedProducer);
  LZ4F_freeCompressionContext(ctx);
}

static void compress_linked_block_mode(const uint8_t* data, size_t size) {
  FUZZ_dataProducer_t *producer = FUZZ_dataProducer_create(data, size);
  LZ4F_preferences_t prefs = FUZZ_dataProducer_preferences(producer);
  prefs.frameInfo.blockMode = LZ4F_blockLinked;
  compress_round_trip(data, size, producer, prefs);
}

static void compress_independent_block_mode(const uint8_t* data, size_t size) {
  FUZZ_dataProducer_t *producer = FUZZ_dataProducer_create(data, size);
  LZ4F_preferences_t prefs = FUZZ_dataProducer_preferences(producer);
  prefs.frameInfo.blockMode = LZ4F_blockIndependent;
  compress_round_trip(data, size, producer, prefs);
}


int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  compress_linked_block_mode(data, size);
  compress_independent_block_mode(data, size);
  return 0;
}
