/**
 * This fuzz target compresses the fuzzed data with the LZ4F streaming API,
 * using a random sequence of operations, into random output capacities.
 * Capacities may be smaller than LZ4F_compressBound() : each operation must
 * succeed if and only if its output fits, and then produce the same output
 * as a reference context which always receives a bound-sized buffer.
 * A failed operation invalidates the frame.
 * Completed frames must round trip correctly.
 * Note : output of linked blocks at fast levels may depend on context history.
 * Hence, after a failure, both contexts restart from a fresh state.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz_helpers.h"
#include "lz4_helpers.h"
#include "lz4frame.h"
#include "lz4frame_static.h"

#define MAX_OPS 64

typedef enum { op_begin, op_update, op_uncompressedUpdate, op_flush, op_end } opType_e;

typedef struct {
    LZ4F_cctx* refCtx;   /* always receives a bound-sized buffer */
    LZ4F_cctx* cctx;     /* receives random capacities */
    const LZ4F_preferences_t* prefs;
    const LZ4F_compressOptions_t* cOpts;
    char* refOut;
    size_t refOutCapacity;
    char* frame;
    size_t frameCapacity;
    size_t frameSize;
} state_t;

static size_t runOp(const state_t* s, LZ4F_cctx* cctx, opType_e type,
                    void* dst, size_t dstCapacity,
                    const uint8_t* src, size_t srcSize)
{
    switch (type) {
    case op_begin:
        return LZ4F_compressBegin(cctx, dst, dstCapacity, s->prefs);
    case op_update:
        return LZ4F_compressUpdate(cctx, dst, dstCapacity, src, srcSize, s->cOpts);
    case op_uncompressedUpdate:
        return LZ4F_uncompressedUpdate(cctx, dst, dstCapacity, src, srcSize, s->cOpts);
    case op_flush:
        return LZ4F_flush(cctx, dst, dstCapacity, s->cOpts);
    case op_end:
        return LZ4F_compressEnd(cctx, dst, dstCapacity, s->cOpts);
    }
    return (size_t)-1;
}

static size_t opBound(const state_t* s, opType_e type, size_t srcSize)
{
    switch (type) {
    case op_begin:
        return LZ4F_HEADER_SIZE_MAX;
    case op_update:
    case op_uncompressedUpdate:
        return LZ4F_compressBound(srcSize, s->prefs);
    case op_flush:
    case op_end:
        return LZ4F_compressBound(0, s->prefs);
    }
    return 0;
}

static void freshContexts(state_t* s)
{
    LZ4F_freeCompressionContext(s->refCtx);
    LZ4F_freeCompressionContext(s->cctx);
    FUZZ_ASSERT(!LZ4F_isError(LZ4F_createCompressionContext(&s->refCtx, LZ4F_VERSION)));
    FUZZ_ASSERT(!LZ4F_isError(LZ4F_createCompressionContext(&s->cctx, LZ4F_VERSION)));
}

static void checkInvalidated(LZ4F_cctx* cctx)
{
    char dst[LZ4F_HEADER_SIZE_MAX];
    size_t const r = LZ4F_compressEnd(cctx, dst, sizeof(dst), NULL);
    FUZZ_ASSERT_MSG(LZ4F_getErrorCode(r) == LZ4F_ERROR_compressionState_uninitialized,
                    "A failed operation must invalidate the frame");
}

/* Runs @type on both contexts, and appends its output to @s->frame.
 * @return : 1 on success, 0 if the operation failed because capacity was too small */
static int runBoth(state_t* s, opType_e type, const uint8_t* src, size_t srcSize, uint32_t* seed)
{
    size_t const bound = opBound(s, type, srcSize);
    size_t capacity;
    FUZZ_ASSERT(bound <= s->refOutCapacity);

    size_t const refSize = runOp(s, s->refCtx, type, s->refOut, bound, src, srcSize);
    FUZZ_ASSERT_MSG(!LZ4F_isError(refSize), "Operation must succeed with a bound-sized buffer");
    FUZZ_ASSERT(refSize <= bound);

    switch (FUZZ_rand32(seed, 0, 7)) {
    case 0:
    case 1: capacity = refSize; break;
    case 2:
    case 3: capacity = FUZZ_rand32(seed, (uint32_t)refSize, (uint32_t)bound); break;
    case 4: capacity = refSize ? refSize - 1 : 0; break;
    case 5: capacity = FUZZ_rand32(seed, 0, (uint32_t)refSize); break;
    default: capacity = FUZZ_rand32(seed, 0, (uint32_t)bound); break;
    }

    /* exact size, to detect overflows */
    char* const dst = (char*)malloc(capacity ? capacity : 1);
    FUZZ_ASSERT(dst != NULL);
    size_t const r = runOp(s, s->cctx, type, dst, capacity, src, srcSize);
    if (capacity < refSize) {
        FUZZ_ASSERT_MSG(LZ4F_getErrorCode(r) == LZ4F_ERROR_dstCapacity_tooSmall,
                        "Operation must fail when its output doesn't fit");
        free(dst);
        checkInvalidated(s->cctx);
        freshContexts(s);
        return 0;
    }
    FUZZ_ASSERT_MSG(r == refSize, "Operation must succeed when its output fits");
    FUZZ_ASSERT_MSG(!memcmp(dst, s->refOut, refSize), "Output must not depend on capacity");
    FUZZ_ASSERT(s->frameSize + refSize <= s->frameCapacity);
    memcpy(s->frame + s->frameSize, dst, refSize);
    s->frameSize += refSize;
    free(dst);
    return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint32_t seed = FUZZ_seed(&data, &size);
    LZ4F_preferences_t const prefs = FUZZ_randomPreferences(&seed);
    int const allowUncompressed = prefs.frameInfo.blockMode == LZ4F_blockIndependent;
    size_t const blockSize = LZ4F_getBlockSize(prefs.frameInfo.blockSizeID);
    LZ4F_compressOptions_t cOpts;
    state_t s;
    size_t pos = 0, frameStart = 0;
    int inFrame = 0;
    unsigned nbOps;

    memset(&cOpts, 0, sizeof(cOpts));
    cOpts.stableSrc = FUZZ_rand32(&seed, 0, 1);

    memset(&s, 0, sizeof(s));
    s.prefs = &prefs;
    s.cOpts = &cOpts;
    s.refOutCapacity = MAX(LZ4F_compressBound(size, &prefs), LZ4F_HEADER_SIZE_MAX);
    s.refOut = (char*)malloc(s.refOutCapacity);
    /* each operation can add up to 2 partial blocks */
    s.frameCapacity = LZ4F_compressFrameBound(size, &prefs)
                    + 2 * MAX_OPS * (LZ4F_BLOCK_HEADER_SIZE + LZ4F_BLOCK_CHECKSUM_SIZE);
    s.frame = (char*)malloc(s.frameCapacity);
    char* const rt = (char*)malloc(size ? size : 1);
    FUZZ_ASSERT(s.refOut != NULL);
    FUZZ_ASSERT(s.frame != NULL);
    FUZZ_ASSERT(rt != NULL);
    freshContexts(&s);

    for (nbOps = 0; nbOps < MAX_OPS; nbOps++) {
        if (!inFrame) {
            frameStart = pos;
            s.frameSize = 0;
            inFrame = runBoth(&s, op_begin, NULL, 0, &seed);
            continue;
        }
        if (pos == size || FUZZ_rand32(&seed, 0, 15) == 0) {
            if (runBoth(&s, op_end, NULL, 0, &seed)) {
                size_t const rtSize = FUZZ_decompressFrame(rt, size, s.frame, s.frameSize);
                FUZZ_ASSERT_MSG(rtSize == pos - frameStart, "Incorrect regenerated size");
                FUZZ_ASSERT_MSG(!memcmp(data + frameStart, rt, rtSize), "Corruption!");
            }
            inFrame = 0;
            if (pos == size) break;
            continue;
        }
        {   uint32_t const t = FUZZ_rand32(&seed, 0, 7);
            opType_e const type = (t == 0) ? op_flush
                                : ((t == 1) && allowUncompressed) ? op_uncompressedUpdate
                                : op_update;
            size_t const remaining = size - pos;
            size_t srcSize = 0;
            if (type != op_flush) {
                switch (FUZZ_rand32(&seed, 0, 3)) {
                case 0: srcSize = FUZZ_rand32(&seed, 0, 16); break;
                case 1: srcSize = blockSize - 1 + FUZZ_rand32(&seed, 0, 2); break;
                default: srcSize = FUZZ_rand32(&seed, 0, (uint32_t)MIN(remaining, 2 * blockSize)); break;
                }
                srcSize = MIN(srcSize, remaining);
            }
            inFrame = runBoth(&s, type, data + pos, srcSize, &seed);
            pos += srcSize;
        }
    }

    free(s.refOut);
    free(s.frame);
    free(rt);
    LZ4F_freeCompressionContext(s.refCtx);
    LZ4F_freeCompressionContext(s.cctx);

    return 0;
}
