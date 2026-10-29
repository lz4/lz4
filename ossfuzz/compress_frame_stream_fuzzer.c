/**
 * This fuzz target compresses the fuzzed data with the LZ4F streaming API,
 * using a random sequence of operations, into random output capacities.
 * Capacities may be smaller than LZ4F_compressBound() : begin and updates must
 * succeed if and only if their output fits, and then produce the same output
 * as a reference context which always receives a bound-sized buffer.
 * A failed operation invalidates the frame.
 * Flush and end fill dstBuffer, or complete : they are invoked again while they fill it,
 * producing the same output in pieces. Capacity 0 is rejected when output remains.
 * Completed frames must round trip correctly.
 * Note : output of linked blocks at fast levels may depend on context history.
 * Hence, after a failure, both contexts restart from a fresh state.
 * Multi-threading (nbWorkers >= 1) : both contexts employ different nb of workers.
 * Without autoFlush, output lags behind input, depending on scheduling :
 * then only whole frames are compared, and each operation must succeed with a bound-sized buffer.
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
    const LZ4F_preferences_t* refPrefs;
    const LZ4F_preferences_t* prefs;
    const LZ4F_compressOptions_t* cOpts;
    int lagging;         /* output lags behind input : only whole frames are comparable */
    char* refOut;
    size_t refOutCapacity;
    char* refFrame;
    size_t refFrameSize;
    char* frame;
    size_t frameCapacity;
    size_t frameSize;
} state_t;

static size_t runOp(const state_t* s, LZ4F_cctx* cctx, const LZ4F_preferences_t* prefs, opType_e type,
                    void* dst, size_t dstCapacity,
                    const uint8_t* src, size_t srcSize)
{
    switch (type) {
    case op_begin:
        return LZ4F_compressBegin(cctx, dst, dstCapacity, prefs);
    case op_update:
    case op_uncompressedUpdate: {
        LZ4F_compressOptions_t cOpts = *s->cOpts;
        if (type == op_uncompressedUpdate) cOpts.uncompressed = 1;
        return LZ4F_compressUpdate(cctx, dst, dstCapacity, src, srcSize, &cOpts);
    }
    case op_flush:
        return LZ4F_flush(cctx, dst, dstCapacity, s->cOpts);
    case op_end:
        return LZ4F_compressEnd(cctx, dst, dstCapacity, s->cOpts);
    }
    return (size_t)-1;
}

static size_t opBound(const LZ4F_preferences_t* prefs, opType_e type, size_t srcSize)
{
    switch (type) {
    case op_begin:
        return LZ4F_HEADER_SIZE_MAX;
    case op_update:
    case op_uncompressedUpdate:
        return LZ4F_compressBound(srcSize, prefs);
    case op_flush:
    case op_end:
        return LZ4F_compressBound(0, prefs);
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

/* Flush and end fill dstBuffer, or complete : invoked again, with other capacities, while they fill it.
 * Each invocation receives exactly its capacity, to detect overflows.
 * @return : total output, written into @out */
static size_t runPieces(const state_t* s, opType_e type, char* out, size_t outCapacity, size_t capacity, uint32_t* seed)
{
    size_t pos = 0;
    for (;;) {
        char* const dst = (char*)malloc(capacity ? capacity : 1);
        size_t r;
        FUZZ_ASSERT(dst != NULL);
        r = runOp(s, s->cctx, s->prefs, type, dst, capacity, NULL, 0);
        if (capacity == 0) {
            free(dst);
            if (!LZ4F_isError(r)) {
                FUZZ_ASSERT_MSG((type == op_flush) && (r == 0), "Only an empty flush succeeds into 0 byte");
                return pos;
            }
            FUZZ_ASSERT_MSG(LZ4F_getErrorCode(r) == LZ4F_ERROR_dstCapacity_tooSmall,
                            "Capacity 0 must be rejected when output remains");
            capacity = FUZZ_rand32(seed, 1, 256);
            continue;
        }
        FUZZ_ASSERT_MSG(!LZ4F_isError(r), "Flush and end must not fail for lack of capacity");
        FUZZ_ASSERT(r <= capacity);
        FUZZ_ASSERT(pos + r <= outCapacity);
        memcpy(out + pos, dst, r);
        pos += r;
        free(dst);
        if (r < capacity) return pos;
        capacity = FUZZ_rand32(seed, 1, 4096);
    }
}

/* Runs @type on both contexts, and appends its output to @s->frame.
 * @return : 1 on success, 0 if the operation failed because capacity was too small */
static int runBoth(state_t* s, opType_e type, const uint8_t* src, size_t srcSize, uint32_t* seed)
{
    size_t const refBound = opBound(s->refPrefs, type, srcSize);
    size_t const bound = opBound(s->prefs, type, srcSize);
    size_t capacity;
    FUZZ_ASSERT(refBound <= s->refOutCapacity);

    size_t refSize;
    if (s->lagging && ((type == op_flush) || (type == op_end))) {
        /* multi-threaded : jobs in flight are emitted in several invocations (at least one job each) */
        size_t r;
        refSize = 0;
        do {
            FUZZ_ASSERT(refSize + refBound <= s->refOutCapacity);
            r = runOp(s, s->refCtx, s->refPrefs, type, s->refOut + refSize, refBound, src, srcSize);
            FUZZ_ASSERT_MSG(!LZ4F_isError(r), "Flush and end must succeed with a bound-sized buffer");
            refSize += r;
        } while (r == refBound);
    } else {
        refSize = runOp(s, s->refCtx, s->refPrefs, type, s->refOut, refBound, src, srcSize);
        FUZZ_ASSERT_MSG(!LZ4F_isError(refSize), "Operation must succeed with a bound-sized buffer");
        FUZZ_ASSERT(refSize <= refBound);
        if ((type == op_flush) || (type == op_end)) {
            FUZZ_ASSERT_MSG(refSize < refBound, "Single-threaded : output must remain strictly below LZ4F_compressBound(0)");
        }
    }

    if (s->lagging) {
        FUZZ_ASSERT(s->refFrameSize + refSize <= s->frameCapacity);
        memcpy(s->refFrame + s->refFrameSize, s->refOut, refSize);
        s->refFrameSize += refSize;
        switch (FUZZ_rand32(seed, 0, 3)) {
        case 0:
        case 1: capacity = bound; break;
        case 2: capacity = FUZZ_rand32(seed, 0, (uint32_t)bound); break;
        default: capacity = FUZZ_rand32(seed, 0, (uint32_t)MIN(bound, 256)); break;
        }
        if ((type == op_flush) || (type == op_end)) {
            s->frameSize += runPieces(s, type, s->frame + s->frameSize, s->frameCapacity - s->frameSize, capacity, seed);
            return 1;
        }
        char* const dst = (char*)malloc(capacity ? capacity : 1);
        FUZZ_ASSERT(dst != NULL);
        size_t const r = runOp(s, s->cctx, s->prefs, type, dst, capacity, src, srcSize);
        if (LZ4F_isError(r)) {
            FUZZ_ASSERT_MSG(capacity < bound, "Operation must succeed with a bound-sized buffer");
            FUZZ_ASSERT_MSG(LZ4F_getErrorCode(r) == LZ4F_ERROR_dstCapacity_tooSmall,
                            "Operation may only fail when its output doesn't fit");
            free(dst);
            checkInvalidated(s->cctx);
            freshContexts(s);
            return 0;
        }
        FUZZ_ASSERT(r <= capacity);
        FUZZ_ASSERT(s->frameSize + r <= s->frameCapacity);
        memcpy(s->frame + s->frameSize, dst, r);
        s->frameSize += r;
        free(dst);
        return 1;
    }

    switch (FUZZ_rand32(seed, 0, 7)) {
    case 0:
    case 1: capacity = refSize; break;
    case 2:
    case 3: capacity = FUZZ_rand32(seed, (uint32_t)refSize, (uint32_t)bound); break;
    case 4: capacity = refSize ? refSize - 1 : 0; break;
    case 5: capacity = FUZZ_rand32(seed, 0, (uint32_t)refSize); break;
    default: capacity = FUZZ_rand32(seed, 0, (uint32_t)bound); break;
    }

    if ((type == op_flush) || (type == op_end)) {
        size_t const total = runPieces(s, type, s->frame + s->frameSize, s->frameCapacity - s->frameSize, capacity, seed);
        FUZZ_ASSERT_MSG(total == refSize, "Output in pieces must have the same size");
        FUZZ_ASSERT_MSG(!memcmp(s->frame + s->frameSize, s->refOut, refSize), "Output must not depend on capacity");
        s->frameSize += total;
        return 1;
    }

    /* exact size, to detect overflows */
    char* const dst = (char*)malloc(capacity ? capacity : 1);
    FUZZ_ASSERT(dst != NULL);
    size_t const r = runOp(s, s->cctx, s->prefs, type, dst, capacity, src, srcSize);
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
    LZ4F_preferences_t prefs = FUZZ_randomPreferences(&seed);
    LZ4F_preferences_t refPrefs;
    if (FUZZ_rand32(&seed, 0, 1)) prefs.nbWorkers = FUZZ_rand32(&seed, 1, 3);
    refPrefs = prefs;
    if (prefs.nbWorkers) refPrefs.nbWorkers = FUZZ_rand32(&seed, 1, 3);   /* output must not depend on nbWorkers */
    size_t const blockSize = LZ4F_getBlockSize(prefs.frameInfo.blockSizeID);
    LZ4F_compressOptions_t cOpts;
    state_t s;
    size_t pos = 0, frameStart = 0;
    int inFrame = 0;
    unsigned nbOps;

    memset(&cOpts, 0, sizeof(cOpts));
    cOpts.stableSrc = FUZZ_rand32(&seed, LZ4F_srcUnstable, LZ4F_srcStableUntilFlush);   /* input remains until the end */

    memset(&s, 0, sizeof(s));
    s.refPrefs = &refPrefs;
    s.prefs = &prefs;
    s.cOpts = &cOpts;
    s.lagging = (prefs.nbWorkers != 0) && !prefs.autoFlush;
    /* multi-threaded flush and end : all jobs in flight, in several invocations of LZ4F_compressBound(0) */
    s.refOutCapacity = LZ4F_compressBound(size, &refPrefs) + LZ4F_compressFrameBound(size, &refPrefs)
                     + 2 * MAX_OPS * (LZ4F_BLOCK_HEADER_SIZE + LZ4F_BLOCK_CHECKSUM_SIZE) + LZ4F_compressBound(0, &refPrefs);
    s.refOut = (char*)malloc(s.refOutCapacity);
    /* each operation can add up to 2 partial blocks */
    s.frameCapacity = LZ4F_compressFrameBound(size, &prefs)
                    + 2 * MAX_OPS * (LZ4F_BLOCK_HEADER_SIZE + LZ4F_BLOCK_CHECKSUM_SIZE);
    s.frame = (char*)malloc(s.frameCapacity);
    s.refFrame = (char*)malloc(s.frameCapacity);
    char* const rt = (char*)malloc(size ? size : 1);
    FUZZ_ASSERT(s.refOut != NULL);
    FUZZ_ASSERT(s.frame != NULL);
    FUZZ_ASSERT(s.refFrame != NULL);
    FUZZ_ASSERT(rt != NULL);
    freshContexts(&s);

    for (nbOps = 0; nbOps < MAX_OPS; nbOps++) {
        if (!inFrame) {
            frameStart = pos;
            s.frameSize = 0;
            s.refFrameSize = 0;
            inFrame = runBoth(&s, op_begin, NULL, 0, &seed);
            continue;
        }
        if (pos == size || FUZZ_rand32(&seed, 0, 15) == 0) {
            if (runBoth(&s, op_end, NULL, 0, &seed)) {
                if (s.lagging) {
                    FUZZ_ASSERT_MSG(s.frameSize == s.refFrameSize && !memcmp(s.frame, s.refFrame, s.frameSize),
                                    "Frame must not depend on nbWorkers, capacities, nor scheduling");
                }
                size_t const rtSize = FUZZ_decompressFrame(rt, size, s.frame, s.frameSize,
                                                           FUZZ_rand32(&seed, 0, 7) == 0 /* occasionally skip checksums */);
                FUZZ_ASSERT_MSG(rtSize == pos - frameStart, "Incorrect regenerated size");
                FUZZ_ASSERT_MSG(!memcmp(data + frameStart, rt, rtSize), "Corruption!");
            }
            inFrame = 0;
            if (pos == size) break;
            continue;
        }
        {   uint32_t const t = FUZZ_rand32(&seed, 0, 7);
            opType_e const type = (t == 0) ? op_flush
                                : (t == 1) ? op_uncompressedUpdate
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
    free(s.refFrame);
    free(rt);
    LZ4F_freeCompressionContext(s.refCtx);
    LZ4F_freeCompressionContext(s.cctx);

    return 0;
}
