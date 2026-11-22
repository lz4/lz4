/*
 * freestanding_frame.c : lz4frame decoder in freestanding mode (LZ4_FREESTANDING, see lz4.h),
 * built the way the Linux kernel imports LZ4 (#2105) : a single translation unit, every function static,
 * so that only the decoder remains. Its only headers are <limits.h>, <stddef.h> and <stdint.h>,
 * those of any freestanding environment (compat/freestanding_include/), and its memory comes from LZ4F_CustomMem.
 *
 * Built twice (see test-freestanding-frame in tests/Makefile) :
 * - with -DDECODER, freestanding : its undefined symbols must be memcpy(), memmove() and memset(),
 *   which compilers may invoke anyway, and XXH32 (lib/xxhash.h),
 * - without : the test, which compresses with the regular library,
 *   and decodes with the freestanding decoder, one page at a time.
 *
 * Copyright (C) Yann Collet 2026
 * GPL v2 License, see tests/README.md
 */

#ifdef DECODER

#define LZ4_FREESTANDING 1
#define LZ4_memcpy(d, s, n)  __builtin_memcpy((d), (s), (n))
#define LZ4_memmove(d, s, n) __builtin_memmove((d), (s), (n))
#define LZ4_memset(p, v, n)  __builtin_memset((p), (v), (n))
#define LZ4LIB_VISIBILITY  static __attribute__((unused))
#define LZ4FLIB_VISIBILITY static __attribute__((unused))
#define LZ4_PUBLISH_STATIC_FUNCTIONS
#define LZ4F_PUBLISH_STATIC_FUNCTIONS
#include "../../lib/lz4.c"
#include "../../lib/lz4hc.c"
#include "../../lib/lz4frame.c"

#else   /* test */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LZ4F_STATIC_LINKING_ONLY   /* LZ4F_CustomMem */
#include "lz4frame.h"

#endif

/* entry point of the freestanding decoder :
 * decodes the frame at src into pages of pageSize bytes, allocating through cmem,
 * @return : decoded size, or -1 if src is not exactly one valid frame, or if it doesn't fit */
long FS_decodeFrame(unsigned char* const* pages, size_t nbPages, size_t pageSize,
                    const void* src, size_t srcSize, LZ4F_CustomMem cmem);

#ifdef DECODER

long FS_decodeFrame(unsigned char* const* pages, size_t nbPages, size_t pageSize,
                    const void* src, size_t srcSize, LZ4F_CustomMem cmem)
{
    const char* in = (const char*)src;
    size_t page = 0, pagePos = 0, total = 0;
    size_t hint = 1;
    long result = -1;
    LZ4F_dctx* const dctx = LZ4F_createDecompressionContext_advanced(cmem, LZ4F_VERSION);
    if (dctx == NULL) return -1;
    while (srcSize > 0 && hint != 0 && page < nbPages) {
        size_t inSize = srcSize;
        size_t outSize = pageSize - pagePos;
        hint = LZ4F_decompress(dctx, pages[page] + pagePos, &outSize, in, &inSize, NULL);
        if (LZ4F_isError(hint)) goto _end;
        in += inSize; srcSize -= inSize;
        pagePos += outSize; total += outSize;
        if (pagePos == pageSize) { page++; pagePos = 0; }
    }
    if (hint == 0 && srcSize == 0) result = (long)total;   /* frame complete, nothing after it */
_end:
    LZ4F_freeDecompressionContext(dctx);
    return result;
}

#else   /* test */

/* allocator given to the freestanding decoder */
static void* FS_alloc(void* opaque, size_t size) { (void)opaque; return malloc(size); }
static void  FS_free(void* opaque, void* p) { (void)opaque; free(p); }

#define PAGE_SIZE   4096
#define SAMPLE_SIZE (600 << 10)

/* compressible text, with an incompressible segment (stored as uncompressed blocks) */
static void fillSample(unsigned char* buf, size_t size)
{
    static const char* const words[] = { "frame ", "block ", "page ", "kernel ", "module ", "linked ", "lz4 ", "\n" };
    unsigned state = 1;
    size_t pos = 0;
    while (pos < size) {
        const char* w;
        size_t len;
        state = state * 1103515245u + 12345u;
        w = words[(state >> 16) % (sizeof(words) / sizeof(words[0]))];
        len = strlen(w);
        if (len > size - pos) len = size - pos;
        memcpy(buf + pos, w, len);
        pos += len;
    }
    for (pos = size / 3; pos < size / 3 + (100 << 10); pos++) {
        state = state * 1103515245u + 12345u;
        buf[pos] = (unsigned char)(state >> 24);
    }
}

/* decodes frame into separate pages, then compares with expected (NULL : decoding must fail) */
static int check(const char* name, const void* frame, size_t frameSize, const unsigned char* expected, size_t expectedSize)
{
    LZ4F_CustomMem const cmem = { FS_alloc, NULL, FS_free, NULL };
    size_t const nbPages = SAMPLE_SIZE / PAGE_SIZE + 1;
    unsigned char* pages[SAMPLE_SIZE / PAGE_SIZE + 1];
    long r;
    size_t n;
    int ok;
    for (n = 0; n < nbPages; n++) pages[n] = (unsigned char*)malloc(PAGE_SIZE);
    r = FS_decodeFrame(pages, nbPages, PAGE_SIZE, frame, frameSize, cmem);
    if (expected == NULL) {
        ok = (r == -1);
    } else {
        ok = (r == (long)expectedSize);
        for (n = 0; ok && n < expectedSize; n += PAGE_SIZE) {
            size_t const len = (expectedSize - n < PAGE_SIZE) ? expectedSize - n : PAGE_SIZE;
            ok = !memcmp(pages[n / PAGE_SIZE], expected + n, len);
        }
    }
    for (n = 0; n < nbPages; n++) free(pages[n]);
    printf("%-40s : %s (%ld)\n", name, ok ? "ok" : "FAILED", r);
    return ok;
}

int main(void)
{
    unsigned char* const sample = (unsigned char*)malloc(SAMPLE_SIZE);
    size_t const frameCapacity = LZ4F_compressFrameBound(SAMPLE_SIZE, NULL) + 64;   /* + header options */
    unsigned char* const frame = (unsigned char*)malloc(frameCapacity);
    size_t frameSize;
    LZ4F_preferences_t const defaultPrefs = LZ4F_INIT_PREFERENCES;
    LZ4F_preferences_t prefs = defaultPrefs;
    int ok = 1;
    if (sample == NULL || frame == NULL) return 1;
    fillSample(sample, SAMPLE_SIZE);

    /* kernel modules : lz4 -12 -B4 -BD */
    prefs.compressionLevel = 12;
    prefs.frameInfo.blockSizeID = LZ4F_max64KB;
    prefs.frameInfo.blockMode = LZ4F_blockLinked;
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    frameSize = LZ4F_compressFrame(frame, frameCapacity, sample, SAMPLE_SIZE, &prefs);
    if (LZ4F_isError(frameSize)) return 1;
    ok &= check("64 KB linked blocks, content checksum", frame, frameSize, sample, SAMPLE_SIZE);
    ok &= check("truncated", frame, frameSize - 1, NULL, 0);
    frame[frameSize - 1] ^= 1;
    ok &= check("wrong content checksum", frame, frameSize, NULL, 0);

    prefs.frameInfo.blockChecksumFlag = LZ4F_blockChecksumEnabled;
    prefs.frameInfo.contentSize = SAMPLE_SIZE;
    frameSize = LZ4F_compressFrame(frame, frameCapacity, sample, SAMPLE_SIZE, &prefs);
    if (LZ4F_isError(frameSize)) return 1;
    ok &= check("+ block checksums, content size", frame, frameSize, sample, SAMPLE_SIZE);

    prefs = defaultPrefs;
    prefs.frameInfo.blockSizeID = LZ4F_max1MB;
    prefs.frameInfo.blockMode = LZ4F_blockIndependent;
    frameSize = LZ4F_compressFrame(frame, frameCapacity, sample, SAMPLE_SIZE, &prefs);
    if (LZ4F_isError(frameSize)) return 1;
    ok &= check("1 MB independent block", frame, frameSize, sample, SAMPLE_SIZE);

    frameSize = LZ4F_compressFrame(frame, frameCapacity, sample, 0, &prefs);
    if (LZ4F_isError(frameSize)) return 1;
    ok &= check("empty frame", frame, frameSize, sample, 0);

    free(frame);
    free(sample);
    return !ok;
}

#endif
