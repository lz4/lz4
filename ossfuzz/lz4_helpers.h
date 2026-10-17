#ifndef LZ4_HELPERS
#define LZ4_HELPERS

#include "lz4frame.h"

LZ4F_frameInfo_t FUZZ_randomFrameInfo(uint32_t* seed);

LZ4F_preferences_t FUZZ_randomPreferences(uint32_t* seed);

/* @skipChecksums : sets the decompression option of the same name */
size_t FUZZ_decompressFrame(void* dst, const size_t dstCapacity,
                            const void* src, const size_t srcSize,
                            int skipChecksums);

#endif /* LZ4_HELPERS */
