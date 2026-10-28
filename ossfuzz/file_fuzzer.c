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

typedef struct {
    unsigned kind;      /* 0 : read, 1 : seek, 2 : tell */
    size_t want;        /* read size */
    long long offset;   /* seek offset */
    int origin;         /* seek origin */
} file_op_t;

/* runs one scripted operation; returns 0 if the file state became unusable
 * (read error on malformed input), meaning the remaining ops are moot */
static int runFileOp(LZ4_readFile_t* rf, const uint8_t* buf, size_t bufCap,
                     const file_op_t* op)
{
    if (op->kind == 0) {
        size_t const got = LZ4F_read(rf, (void*)buf, op->want);
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
    if (LZ4F_isError(LZ4F_readOpen(&rf, fp)))
        return;

    size_t const bufCap = 64 * 1024;
    uint8_t* const buf = (uint8_t*)malloc(bufCap);
    FUZZ_ASSERT(buf);

    for (unsigned op = 0; op < nbOps; op++) {
        if (!runFileOp(rf, buf, bufCap, &ops[op]))
            break;
    }

    free(buf);
    FUZZ_ASSERT(!LZ4F_isError(LZ4F_readClose(rf)));
}

/* writes @data through the lz4file write API and reads it back through the
 * read API; the round trip must reproduce the original bytes exactly */
static void writeReadRoundTrip(const LZ4F_preferences_t* prefs,
                               const uint8_t* data, size_t size)
{
    char* compressed = NULL;
    size_t compressedSize = 0;
    FILE* const wfp = open_memstream(&compressed, &compressedSize);
    FUZZ_ASSERT(wfp);

    { LZ4_writeFile_t* wf = NULL;
      if (!LZ4F_isError(LZ4F_writeOpen(&wf, wfp, prefs))) {
        /* split in two writes to exercise the internal buffering */
        size_t const first = size / 2;
        if (!LZ4F_isError(LZ4F_write(wf, data, first)))
            (void)LZ4F_write(wf, data + first, size - first);
        FUZZ_ASSERT(!LZ4F_isError(LZ4F_writeClose(wf)));
    } }

    fclose(wfp);   /* flushes and finalizes compressed/compressedSize */
    FUZZ_ASSERT(compressed != NULL);
    FUZZ_ASSERT(compressedSize > 0);

    FILE* const rfp = fmemopen(compressed, compressedSize, "rb");
    FUZZ_ASSERT(rfp);

    { LZ4_readFile_t* rf = NULL;
      FUZZ_ASSERT(!LZ4F_isError(LZ4F_readOpen(&rf, rfp)));
      uint8_t* const back = (uint8_t*)malloc(size);
      FUZZ_ASSERT(back);
      size_t total = 0;
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
      free(back);
      FUZZ_ASSERT(!LZ4F_isError(LZ4F_readClose(rf)));
    }

    fclose(rfp);
    free(compressed);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_dataProducer_t *producer = FUZZ_dataProducer_create(data, size);

    /* consume the op script and preferences first; the front remainder of the
     * input is the file contents both phases operate on */
    LZ4F_preferences_t const prefs = FUZZ_dataProducer_preferences(producer);

    unsigned const nbOps = FUZZ_dataProducer_range32(producer, 1, FILE_FUZZ_MAX_OPS);
    file_op_t ops[FILE_FUZZ_MAX_OPS];
    memset(ops, 0, sizeof(ops));
    for (unsigned op = 0; op < nbOps; op++) {
        ops[op].kind = FUZZ_dataProducer_range32(producer, 0, 2);
        if (ops[op].kind == 0) {
            ops[op].want = FUZZ_dataProducer_range32(producer, 0, 64 * 1024);
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

        writeReadRoundTrip(&prefs, data, size);
    }

    FUZZ_dataProducer_free(producer);
    return 0;
}
