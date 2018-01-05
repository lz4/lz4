/*
    emscripten-tests.c - Demo program to test lz4 in emscripten
    Copyright (C) Yann Collet 2012-2016

    GPL v2 License

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

    You can contact the author at :
    - LZ4 source repository : https://github.com/lz4/lz4
    - LZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
*/

/**************************************
*  Includes
**************************************/
#include "platform.h"    /* _CRT_SECURE_NO_WARNINGS, Large Files support */
#include <stdio.h>       /* fprintf, fopen, ftello */
#include <stdlib.h>      /* rand */
#include <string.h>      /* memcmp */

#include "lz4.h"


/**************************************
*  Constants
**************************************/
#define PROGRAM_DESCRIPTION "LZ4 emscripten testbed"
#define AUTHOR "Yann Collet"
#define WELCOME_MESSAGE "*** %s v%s %i-bits, by %s ***\n", PROGRAM_DESCRIPTION, LZ4_VERSION_STRING, (int)(sizeof(void*)*8), AUTHOR


/**************************************
*  Macros
**************************************/
#define DISPLAY(...) fprintf(stderr, __VA_ARGS__)
#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))


enum {
    MESSAGE_MAX_BYTES   = 1024,
    RING_BUFFER_BYTES   = 1024 * 8 + MESSAGE_MAX_BYTES,
    DECODE_RING_BUFFER  = RING_BUFFER_BYTES + MESSAGE_MAX_BYTES,   /* Intentionally larger, to test unsynchronized ring buffers */
    ITERATIONS          = 500000,
};

static int test(char* decBuf, char* inpBuf) {
    int decOffset = 0;
    int inpOffset = 0;

    LZ4_stream_t lz4Stream_body = { { 0 } };
    LZ4_stream_t* lz4Stream = &lz4Stream_body;

    LZ4_streamDecode_t lz4StreamDecode_body = { { 0 } };
    LZ4_streamDecode_t* lz4StreamDecode = &lz4StreamDecode_body;

    for(int i=0; i<ITERATIONS; ++i) {
        // Read random length ([1,MESSAGE_MAX_BYTES]) data to the ring buffer.
        char* const inpPtr = &inpBuf[inpOffset];
        const int inpBytes = (rand() % MESSAGE_MAX_BYTES) + 1;

        for(int j=0; j<inpBytes; ++j) {
          inpPtr[j] = (rand() % 10);
        }

        {
#define CMPBUFSIZE (LZ4_COMPRESSBOUND(MESSAGE_MAX_BYTES))
            char cmpBuf[CMPBUFSIZE];
            const int cmpBytes = LZ4_compress_fast_continue(lz4Stream, inpPtr, cmpBuf, inpBytes, CMPBUFSIZE, 0);
            if(cmpBytes <= 0) { return 1; }

            inpOffset += inpBytes;

            // Wraparound the ringbuffer offset
            if(inpOffset >= RING_BUFFER_BYTES - MESSAGE_MAX_BYTES) inpOffset = 0;

            {
                char* const decPtr = &decBuf[decOffset];
                const int decBytes = LZ4_decompress_safe_continue(
                    lz4StreamDecode, cmpBuf, decPtr, cmpBytes, MESSAGE_MAX_BYTES);
                if(decBytes <= 0) { return 2; }
                decOffset += decBytes;

                // Wraparound the ringbuffer offset
                if(decOffset >= DECODE_RING_BUFFER - MESSAGE_MAX_BYTES) decOffset = 0;

                {
                  if (decBytes != inpBytes) {
                    return 3;
                  }
                  if (memcmp(inpPtr, decPtr, decBytes) != 0) {
                    return 4;
                  }
                }
            }
        }
    }
    return 0;
}

// This struct here to force ordering of buffers in memory.
// emscripten tends to allocate this at low addresses, which can
// expose bugs.
//
// Be careful that adding other test cases linked into this
// executable might move this, making tests pointless.
// Probably good to keep this as the only test in this executable
// and create separate executables for other emscripten tests.
//
static struct {
  char lowBuf[MAX(DECODE_RING_BUFFER, RING_BUFFER_BYTES)];
  char highBuf[MAX(DECODE_RING_BUFFER, RING_BUFFER_BYTES)];
} buffers;


int test_blockStreaming_ringbuffer_low_dec() {
    return test(buffers.lowBuf, buffers.highBuf);
}

int test_blockStreaming_ringbuffer_low_comp() {
    return test(buffers.highBuf, buffers.lowBuf);
}

int main() {
    // Welcome message
    DISPLAY(WELCOME_MESSAGE);

    DISPLAY("Low decompression buffer address... ");
    int result_low_dec = test_blockStreaming_ringbuffer_low_dec();
    if (result_low_dec == 0) {
        DISPLAY("PASS\n");
    } else {
        DISPLAY("FAIL: %d\n", result_low_dec);
    }

    DISPLAY("Low compression buffer address... ");
    int result_low_comp = test_blockStreaming_ringbuffer_low_comp();
    if (result_low_comp == 0) {
        DISPLAY("PASS\n");
    } else {
        DISPLAY("FAIL: %d\n", result_low_comp);
    }

    return result_low_dec + result_low_comp;
}
