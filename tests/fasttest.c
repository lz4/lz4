/**************************************
 * Compiler Options
 **************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  define _CRT_SECURE_NO_WARNINGS // for MSVC
#  define snprintf sprintf_s
#endif
#ifdef __GNUC__
#  pragma GCC diagnostic ignored "-Wmissing-braces"   /* GCC bug 53119 : doesn't accept { 0 } as initializer (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53119) */
#endif


/**************************************
 * Includes
 **************************************/
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "lz4.h"


/* Returns non-zero on failure. */
int test_compress(const char *input, int inSize, char *output, int outSize)
{
    LZ4_stream_t lz4Stream_body = { 0 };
    LZ4_stream_t* lz4Stream = &lz4Stream_body;

    int inOffset = 0;
    int outOffset = 0;

    if (inSize & 3) return -1;

    while (inOffset < inSize) {
        const int length = inSize >> 2;
        if (inSize > 1024) return -2;
        if (outSize - (outOffset + 8) < LZ4_compressBound(length)) return -3;
        {
            const int outBytes = LZ4_compress_fast_continue(
                lz4Stream, input + inOffset, output + outOffset + 8, length, outSize-outOffset, 1);
            if(outBytes <= 0) return -4;
            memcpy(output + outOffset, &length, 4); /* input length */
            memcpy(output + outOffset + 4, &outBytes, 4); /* output length */
            inOffset += length;
            outOffset += outBytes + 8;
        }
    }
    if (outOffset + 8 > outSize) return -5;
    memset(output + outOffset, 0, 4);
    memset(output + outOffset + 4, 0, 4);
    return 0;
}

/* Returns non-zero on failure. Not a safe function. */
int test_decompress(const char *uncompressed, const char *compressed)
{
    char outBufferA[1024];
    char spacing; /* So prefixEnd != dest */
    char outBufferB[1024];
    char *output = outBufferA;
    char *lastOutput = outBufferB;
    LZ4_streamDecode_t lz4StreamDecode_body = { 0 };
    LZ4_streamDecode_t* lz4StreamDecode = &lz4StreamDecode_body;
    int offset = 0;
    int unOffset = 0;
    int lastBytes = 0;

    (void)spacing;

    for(;;) {
        int32_t bytes;
        int32_t unBytes;
        /* Read uncompressed size and compressed size */
        memcpy(&unBytes, compressed + offset, 4);
        memcpy(&bytes, compressed + offset + 4, 4);
        offset += 8;
        /* Check if we reached end of stream or error */
        if(bytes == 0 && unBytes == 0) return 0;
        if(bytes <= 0 || unBytes <= 0 || unBytes > 1024) return 1;

        /* Put the last output in the dictionary */
        LZ4_setStreamDecode(lz4StreamDecode, lastOutput, lastBytes);
        /* Decompress */
        bytes = LZ4_decompress_fast_continue(
            lz4StreamDecode, compressed + offset, output, unBytes);
        if(bytes <= 0) return 2;
        /* Check result */
        {   int const r = memcmp(uncompressed + unOffset, output, unBytes);
            if (r) return 3;
        }
        { char* const tmp = output; output = lastOutput; lastOutput = tmp; }
        offset += bytes;
        unOffset += unBytes;
        lastBytes = unBytes;
    }
}


int main(int argc, char **argv)
{
    char input[] =
        "Hello Hello Hello Hello Hello Hello Hello Hello!"
        "Hello Hello Hello Hello Hello Hello Hello Hello!"
        "Hello Hello Hello Hello Hello Hello Hello Hello!"
        "Hello Hello Hello Hello Hello Hello Hello Hello!"
        "Hello Hello Hello Hello Hello Hello Hello Hello!"
        "Hello Hello Hello Hello Hello Hello Hello Hello!"
        "Hello Hello Hello Hello Hello Hello Hello Hello!"
        "Hello Hello Hello Hello Hello Hello Hello Hello!"
        "Hello Hello Hello Hello Hello Hello Hello Hello!"
        "Hello Hello Hello Hello Hello Hello Hello Hello!"
        "Hello Hello Hello Hello Hello Hello Hello Hello!"
        "Hello Hello Hello Hello Hello Hello Hello Hello!"
        "Hello Hello Hello Hello Hello Hello Hello Hello!"
        "Hello Hello Hello Hello Hello Hello Hello Hello!"
        "Hello Hello Hello Hello Hello Hello Hello Hello!"
        "Hello Hello Hello Hello Hello Hello Hello Hello";
    char output[LZ4_COMPRESSBOUND(4096)];
    int r;

    (void)argc;
    (void)argv;

    if ((r = test_compress(input, sizeof(input), output, sizeof(output)))) {
        return r;
    }
    if ((r = test_decompress(input, output))) {
        return r;
    }
    return 0;
}
