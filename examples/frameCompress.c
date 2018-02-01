/* LZ4frame API example : compress a file
 * Based on sample code from Zbigniew Jędrzejewski-Szmek
 *
 * This example streams an input file into an output file
 * using a bounded memory budget.
 * Input is read in chunks of IN_CHUNK_SIZE */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <lz4frame.h>


#define IN_CHUNK_SIZE  (16*1024)

static const LZ4F_preferences_t kPrefs = {
    { LZ4F_max256KB, LZ4F_blockLinked, LZ4F_noContentChecksum, LZ4F_frame,
      0 /* unknown content size */, 0 /* no dictID */ , LZ4F_noBlockChecksum },
    0,   /* compression level; 0 == default */
    0,   /* autoflush */
    { 0, 0, 0, 0 },  /* reserved, must be set to 0 */
};


/* safe_fwrite() :
 * performs fwrite(), ensure operation success, or immediately exit() */
static void safe_fwrite(void* buf, size_t eltSize, size_t nbElt, FILE* f)
{
    size_t const writtenSize = fwrite(buf, eltSize, nbElt, f);
    size_t const expectedSize = eltSize * nbElt;   /* note : should check for overflow */
    if (writtenSize < expectedSize) {
        if (ferror(f))  /* note : ferror() must follow fwrite */
            printf("Write failed\n");
        else
            printf("Short write\n");
        exit(1);
    }
}


/* ================================================= */
/*     Streaming Compression example               */
/* ================================================= */

typedef struct {
    int error;
    unsigned long long size_in;
    unsigned long long size_out;
} compressResult_t;

static compressResult_t
compress_file_internal(FILE* in, FILE* out,
                    LZ4F_compressionContext_t ctx,
                    void* inBuff, size_t inChunkSize,
                    void* outBuff, size_t outCapacity)
{
    compressResult_t result = { 1, 0, 0 };  /* result for an error */
    unsigned long long count_in = 0, count_out;

    assert(ctx != NULL);
    assert(outCapacity >= LZ4F_HEADER_SIZE_MAX);
    assert(outCapacity >= LZ4F_compressBound(inChunkSize, &kPrefs));

    /* write frame header */
    {   size_t const headerSize = LZ4F_compressBegin(ctx, outBuff, outCapacity, &kPrefs);
        if (LZ4F_isError(headerSize)) {
            printf("Failed to start compression: error %zu\n", headerSize);
            return result;
        }
        count_out = headerSize;
        printf("Buffer size is %zu bytes, header size %zu bytes\n", outCapacity, headerSize);
        safe_fwrite(outBuff, 1, headerSize, out);
    }

    /* stream file */
    for (;;) {
        size_t const readSize = fread(inBuff, 1, IN_CHUNK_SIZE, in);
        if (readSize == 0) break;
        count_in += readSize;

        size_t const compressedSize = LZ4F_compressUpdate(ctx,
                                                outBuff, outCapacity,
                                                inBuff, readSize,
                                                NULL);
        if (LZ4F_isError(compressedSize)) {
            printf("Compression failed: error %zu\n", compressedSize);
            return result;
        }

        printf("Writing %zu bytes\n", compressedSize);
        safe_fwrite(outBuff, 1, compressedSize, out);
        count_out += compressedSize;
    }

    /* flush whatever remains within internal buffers */
    {   size_t const compressedSize = LZ4F_compressEnd(ctx,
                                            outBuff, outCapacity,
                                            NULL);
        if (LZ4F_isError(compressedSize)) {
            printf("Failed to end compression: error %zu\n", compressedSize);
            return result;
        }

        printf("Writing %zu bytes\n", compressedSize);
        safe_fwrite(outBuff, 1, compressedSize, out);
        count_out += compressedSize;
    }

    result.size_in = count_in;
    result.size_out = count_out;
    result.error = 0;
    return result;
}

static compressResult_t
compress_file(FILE* in, FILE* out)
{
    compressResult_t result = { 1, 0, 0 };  /* == error, default (early exit) */

    assert(in != NULL);
    assert(out != NULL);

    /* allocate ressources */
    LZ4F_compressionContext_t ctx;
    if (LZ4F_isError( LZ4F_createCompressionContext(&ctx, LZ4F_VERSION) )) {
        printf("error: failed to create context \n");
        return result;
    }

    char* outbuff = NULL;
    void* const src = malloc(IN_CHUNK_SIZE);
    if (!src) {
        printf("Not enough memory\n");
        goto cleanup;
    }

    size_t const outbufCapacity = LZ4F_compressBound(IN_CHUNK_SIZE, &kPrefs);   /* large enough for any input <= IN_CHUNK_SIZE */
    outbuff = malloc(outbufCapacity);
    if (!outbuff) {
        printf("Not enough memory\n");
        goto cleanup;
    }

    result = compress_file_internal(in, out,
                                        ctx,
                                        src, IN_CHUNK_SIZE,
                                        outbuff, outbufCapacity);

 cleanup:
    LZ4F_freeCompressionContext(ctx);   /* supports free on NULL */
    free(src);
    free(outbuff);
    return result;
}


/* ================================================= */
/*     Streaming decompression example               */
/* ================================================= */

static size_t get_block_size(const LZ4F_frameInfo_t* info) {
    switch (info->blockSizeID) {
        case LZ4F_default:
        case LZ4F_max64KB:  return 1 << 16;
        case LZ4F_max256KB: return 1 << 18;
        case LZ4F_max1MB:   return 1 << 20;
        case LZ4F_max4MB:   return 1 << 22;
        default:
            printf("Impossible unless more block sizes are allowed\n");
            exit(1);
    }
}

static size_t decompress_file(FILE* in, FILE* out) {
    void* const src = malloc(IN_CHUNK_SIZE);
    void* dst = NULL;
    size_t dstCapacity = 0;
    LZ4F_dctx* dctx = NULL;
    size_t ret = 1;

    /* Initialization */
    if (!src) { perror("decompress_file(src)"); goto cleanup; }
    {   size_t const dctxStatus = LZ4F_createDecompressionContext(&dctx, 100);
        if (LZ4F_isError(dctxStatus)) {
            printf("LZ4F_dctx creation error: %s\n", LZ4F_getErrorName(dctxStatus));
            goto cleanup;
    }   }

    /* Decompression */
    while (ret != 0) {
        /* Load more input */
        size_t srcSize = fread(src, 1, IN_CHUNK_SIZE, in);
        const void* srcPtr = src;
        const void* const srcEnd = srcPtr + srcSize;
        if (srcSize == 0 || ferror(in)) {
            printf("Decompress: not enough input or error reading file\n");
            goto cleanup;
        }
        /* Allocate destination buffer if it isn't already */
        if (!dst) {
            LZ4F_frameInfo_t info;
            ret = LZ4F_getFrameInfo(dctx, &info, src, &srcSize);
            if (LZ4F_isError(ret)) {
                printf("LZ4F_getFrameInfo error: %s\n", LZ4F_getErrorName(ret));
                goto cleanup;
            }
            /* Allocating enough space for an entire block isn't necessary for
             * correctness, but it allows some memcpy's to be elided.
             */
            dstCapacity = get_block_size(&info);
            dst = malloc(dstCapacity);
            if (!dst) { perror("decompress_file(dst)"); goto cleanup; }
            srcPtr += srcSize;
            srcSize = srcEnd - srcPtr;
        }
        /* Decompress:
         * Continue while there is more input to read and the frame isn't over.
         * If srcPtr == srcEnd then we know that there is no more output left in the
         * internal buffer left to flush.
         */
        while (srcPtr != srcEnd && ret != 0) {
            /* INVARIANT: Any data left in dst has already been written */
            size_t dstSize = dstCapacity;
            ret = LZ4F_decompress(dctx, dst, &dstSize, srcPtr, &srcSize, /* LZ4F_decompressOptions_t */ NULL);
            if (LZ4F_isError(ret)) {
                printf("Decompression error: %s\n", LZ4F_getErrorName(ret));
                goto cleanup;
            }
            /* Flush output */
            if (dstSize != 0){
                size_t written = fwrite(dst, 1, dstSize, out);
                printf("Writing %zu bytes\n", dstSize);
                if (written != dstSize) {
                    printf("Decompress: Failed to write to file\n");
                    goto cleanup;
                }
            }
            /* Update input */
            srcPtr += srcSize;
            srcSize = srcEnd - srcPtr;
        }
    }
    /* Check that there isn't trailing input data after the frame.
     * It is valid to have multiple frames in the same file, but this example
     * doesn't support it.
     */
    ret = fread(src, 1, 1, in);
    if (ret != 0 || !feof(in)) {
        printf("Decompress: Trailing data left in file after frame\n");
        goto cleanup;
    }

cleanup:
    free(src);
    free(dst);
    return LZ4F_freeDecompressionContext(dctx);   /* note : free works on NULL */
}


int compare(FILE* fp0, FILE* fp1)
{
    int result = 0;

    while(0 == result) {
        char b0[1024];
        char b1[1024];
        const size_t r0 = fread(b0, 1, sizeof(b0), fp0);
        const size_t r1 = fread(b1, 1, sizeof(b1), fp1);

        result = (int) r0 - (int) r1;

        if (0 == r0 || 0 == r1) {
            break;
        }
        if (0 == result) {
            result = memcmp(b0, b1, r0);
        }
    }

    return result;
}


int main(int argc, const char **argv) {
    char inpFilename[256] = { 0 };
    char lz4Filename[256] = { 0 };
    char decFilename[256] = { 0 };

    if(argc < 2) {
        printf("Please specify input filename\n");
        return 0;
    }

    snprintf(inpFilename, 256, "%s", argv[1]);
    snprintf(lz4Filename, 256, "%s.lz4", argv[1]);
    snprintf(decFilename, 256, "%s.lz4.dec", argv[1]);

    printf("inp = [%s]\n", inpFilename);
    printf("lz4 = [%s]\n", lz4Filename);
    printf("dec = [%s]\n", decFilename);

    /* compress */
    {   FILE* const inpFp = fopen(inpFilename, "rb");
        FILE* const outFp = fopen(lz4Filename, "wb");

        printf("compress : %s -> %s\n", inpFilename, lz4Filename);
        compressResult_t const ret = compress_file(inpFp, outFp);
        if (ret.error) {
            printf("compress : failed with code %i\n", ret.error);
            return ret.error;
        }
        printf("%s: %zu → %zu bytes, %.1f%%\n",
            inpFilename,
            (size_t)ret.size_in, (size_t)ret.size_out,  /* might overflow */
            (double)ret.size_out / ret.size_in * 100);
        printf("compress : done\n");

        fclose(outFp);
        fclose(inpFp);
    }

    /* decompress */
    {   FILE* const inpFp = fopen(lz4Filename, "rb");
        FILE* const outFp = fopen(decFilename, "wb");
        size_t ret;

        printf("decompress : %s -> %s\n", lz4Filename, decFilename);
        ret = decompress_file(inpFp, outFp);
        if (ret) {
            printf("decompress : failed with code %zu\n", ret);
            return (int)ret;
        }
        printf("decompress : done\n");

        fclose(outFp);
        fclose(inpFp);
    }

    /* verify */
    {   FILE* const inpFp = fopen(inpFilename, "rb");
        FILE* const decFp = fopen(decFilename, "rb");

        printf("verify : %s <-> %s\n", inpFilename, decFilename);
        const int cmp = compare(inpFp, decFp);
        if(0 == cmp) {
            printf("verify : OK\n");
        } else {
            printf("verify : NG\n");
        }

        fclose(decFp);
        fclose(inpFp);
    }

    return 0;
}
