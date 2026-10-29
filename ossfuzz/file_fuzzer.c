/**
 * This fuzz target exercises the lz4 file API (lib/lz4file.c) end to end.
 *
 * Phase 1 feeds arbitrary, possibly corrupt or truncated input to the read
 * side: LZ4F_readOpen() / LZ4F_read() / LZ4F_seek() / LZ4F_tell() driven by an
 * operation script taken from the fuzzed data.  Errors are expected on
 * malformed input; crashes are not.
 *
 * Phase 2 round trips the fuzzed data through the write side and back: the
 * bytes written with LZ4F_writeOpen() / LZ4F_write() / LZ4F_writeClose() must
 * decode back through the read side to exactly the original data.
 */

#define _POSIX_C_SOURCE 200809L  /* fmemopen, open_memstream */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz_helpers.h"
#include "fuzz_data_producer.h"
#define LZ4F_STATIC_LINKING_ONLY
#include "lz4frame.h"
#include "lz4file.h"

#define FILE_FUZZ_MAX_OPS 16
#define FILE_FUZZ_BUF_CAP (64 * 1024)

typedef struct {
    unsigned kind;      /* 0 : read, 1 : seek, 2 : tell */
    size_t want;        /* read size */
    long long offset;   /* seek offset */
    int origin;         /* seek origin */
} file_op_t;

/* runs one scripted operation; returns 0 if the file state became unusable
 * (read error on malformed input), meaning the remaining ops are moot */
static int runFileOp(LZ4_readFile_t* rf, uint8_t* buf, const file_op_t* op)
{
    if (op->kind == 0) {
        size_t const got = LZ4F_read(rf, buf, op->want);
        return !LZ4F_isError(got);
    }
    if (op->kind == 1) {
        /* errors are expected: seeking past the end or before the start */
        (void)LZ4F_seek(rf, op->offset, op->origin);
        return 1;
    }
    (void)LZ4F_tell(rf);
    return 1;
}

/* drives the read side over @fp, which holds arbitrary attacker-controlled
 * bytes; every LZ4F_* error return is acceptable here, crashes are not */
static void readFileOps(FILE* fp, const file_op_t* ops, unsigned nbOps)
{
    LZ4_readFile_t* rf = NULL;
    uint8_t* buf;
    unsigned op;

    if (LZ4F_isError(LZ4F_readOpen(&rf, fp)))
        return;

    buf = (uint8_t*)malloc(FILE_FUZZ_BUF_CAP);
    FUZZ_ASSERT(buf);

    for (op = 0; op < nbOps; op++) {
        if (!runFileOp(rf, buf, &ops[op]))
            break;
    }

    free(buf);
    FUZZ_ASSERT(!LZ4F_isError(LZ4F_readClose(rf)));
}

/* runs the op script on a valid frame of known content @data :
 * seeks land within it, and every read must return the original bytes */
static void checkFileOps(LZ4_readFile_t* rf, const uint8_t* data, size_t size,
                         const file_op_t* ops, unsigned nbOps)
{
    uint8_t* const buf = (uint8_t*)malloc(FILE_FUZZ_BUF_CAP);
    unsigned long long pos = LZ4F_tell(rf);
    unsigned op;

    FUZZ_ASSERT(buf);
    for (op = 0; op < nbOps; op++) {
        if (ops[op].kind == 0) {
            size_t const expected = MIN(ops[op].want, (size_t)(size - pos));
            FUZZ_ASSERT(LZ4F_read(rf, buf, ops[op].want) == expected);
            FUZZ_ASSERT(memcmp(buf, data + pos, expected) == 0);
            pos += expected;
        } else if (ops[op].kind == 1) {
            long long const magnitude = (ops[op].offset < 0) ? -ops[op].offset : ops[op].offset;
            unsigned long long const target = (unsigned long long)magnitude % (size + 1);
            long long const offset = (ops[op].origin == SEEK_SET) ? (long long)target
                                                                  : (long long)target - (long long)pos;
            FUZZ_ASSERT(!LZ4F_isError(LZ4F_seek(rf, offset, ops[op].origin)));
            pos = target;
        }
        FUZZ_ASSERT(LZ4F_tell(rf) == pos);
    }
    free(buf);
}

/* writes @data through the lz4file write API and reads it back through the
 * read API; the round trip must reproduce the original bytes exactly */
static void writeReadRoundTrip(const LZ4F_preferences_t* prefs,
                               const uint8_t* data, size_t size,
                               const file_op_t* ops, unsigned nbOps)
{
    char* compressed = NULL;
    size_t compressedSize = 0;
    FILE* const wfp = open_memstream(&compressed, &compressedSize);
    FILE* rfp;
    LZ4_writeFile_t* wf = NULL;
    LZ4_readFile_t* rf;
    uint8_t* back;
    size_t first;
    size_t total;

    FUZZ_ASSERT(wfp);

    if (!LZ4F_isError(LZ4F_writeOpen(&wf, wfp, prefs))) {
        /* split in two writes to exercise the internal buffering */
        first = size / 2;
        if (!LZ4F_isError(LZ4F_write(wf, data, first)))
            (void)LZ4F_write(wf, data + first, size - first);
        FUZZ_ASSERT(!LZ4F_isError(LZ4F_writeClose(wf)));
    }

    fclose(wfp);   /* flushes and finalizes compressed/compressedSize */
    FUZZ_ASSERT(compressed != NULL);
    FUZZ_ASSERT(compressedSize > 0);

    rfp = fmemopen(compressed, compressedSize, "rb");
    FUZZ_ASSERT(rfp);

    FUZZ_ASSERT(!LZ4F_isError(LZ4F_readOpen(&rf, rfp)));
    back = (uint8_t*)malloc(size);
    FUZZ_ASSERT(back);
    total = 0;
    while (total < size) {
        size_t const got = LZ4F_read(rf, back + total, size - total);
        /* the frame was just written by this process: it must decode */
        FUZZ_ASSERT(!LZ4F_isError(got));
        if (got == 0)
            break;
        total += got;
    }
    FUZZ_ASSERT_MSG(total == size, "lz4file round trip truncated the data");
    FUZZ_ASSERT_MSG(memcmp(back, data, size) == 0,
                    "lz4file round trip corrupted the data");
    checkFileOps(rf, data, size, ops, nbOps);
    free(back);
    FUZZ_ASSERT(!LZ4F_isError(LZ4F_readClose(rf)));

    fclose(rfp);
    free(compressed);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_dataProducer_t *producer = FUZZ_dataProducer_create(data, size);

    /* consume the op script and preferences first; the front remainder of the
     * input is the file contents both phases operate on */
    LZ4F_preferences_t const prefs = FUZZ_dataProducer_preferences(producer);
    file_op_t ops[FILE_FUZZ_MAX_OPS];
    unsigned const nbOps = FUZZ_dataProducer_range32(producer, 1, FILE_FUZZ_MAX_OPS);
    unsigned op;

    memset(ops, 0, sizeof(ops));
    for (op = 0; op < nbOps; op++) {
        ops[op].kind = FUZZ_dataProducer_range32(producer, 0, 2);
        if (ops[op].kind == 0) {
            ops[op].want = FUZZ_dataProducer_range32(producer, 0, FILE_FUZZ_BUF_CAP);
        } else if (ops[op].kind == 1) {
            long long const forward =
                (long long)FUZZ_dataProducer_range32(producer, 0, 1u << 20);
            long long const backward =
                (long long)FUZZ_dataProducer_range32(producer, 0, 1u << 20);
            ops[op].offset = forward - backward;
            ops[op].origin = (FUZZ_dataProducer_range32(producer, 0, 1) == 0)
                           ? SEEK_SET : SEEK_CUR;
        }
    }

    size = FUZZ_dataProducer_remainingBytes(producer);

    if (size > 0) {
        FILE* const fp = fmemopen((void*)data, size, "rb");
        FUZZ_ASSERT(fp);
        readFileOps(fp, ops, nbOps);
        fclose(fp);

        writeReadRoundTrip(&prefs, data, size, ops, nbOps);
    }

    FUZZ_dataProducer_free(producer);
    return 0;
}
