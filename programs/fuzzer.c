/*
    fuzzer.c - Fuzzer test tool for LZ4
    Copyright (C) Yann Collet 2012-2014
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
    - LZ4 homepage : http://fastcompression.blogspot.com/p/lz4.html
    - LZ4 source repository : http://code.google.com/p/lz4/
*/

/**************************************
 Remove Visual warning messages
**************************************/
#define _CRT_SECURE_NO_WARNINGS   // fgets


/**************************************
 Includes
**************************************/
#include <stdlib.h>
#include <stdio.h>      // fgets, sscanf
#include <sys/timeb.h>  // timeb
#include <string.h>     // strcmp
#include "lz4.h"
#include "lz4hc.h"
#include "xxhash.h"


/**************************************
   Basic Types
**************************************/
#if defined (__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)   /* C99 */
# include <stdint.h>
  typedef  uint8_t BYTE;
  typedef uint16_t U16;
  typedef uint32_t U32;
  typedef  int32_t S32;
  typedef uint64_t U64;
#else
  typedef unsigned char       BYTE;
  typedef unsigned short      U16;
  typedef unsigned int        U32;
  typedef   signed int        S32;
  typedef unsigned long long  U64;
#endif


/**************************************
 Constants
**************************************/
#ifndef LZ4_VERSION
#  define LZ4_VERSION "rc118"
#endif

#define NB_ATTEMPTS (1<<16)
#define COMPRESSIBLE_NOISE_LENGTH (1 << 21)
#define FUZ_MAX_BLOCK_SIZE (1 << 17)
#define FUZ_MAX_DICT_SIZE  (1 << 15)
#define LEN ((1<<15))
#define SEQ_POW 2
#define NUM_SEQ (1 << SEQ_POW)
#define SEQ_MSK ((NUM_SEQ) - 1)
#define MOD_SEQ(x) ((((x) >> 8) & 255) == 0)
#define NEW_SEQ(x) ((((x) >> 10) %10) == 0)
#define PAGE_SIZE 4096
#define ROUND_PAGE(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PRIME1   2654435761U
#define PRIME2   2246822519U
#define PRIME3   3266489917U


/*********************************************************
  Fuzzer functions
*********************************************************/
static int FUZ_GetMilliStart()
{
   struct timeb tb;
   int nCount;
   ftime( &tb );
   nCount = (int) (tb.millitm + (tb.time & 0xfffff) * 1000);
   return nCount;
}


static int FUZ_GetMilliSpan( int nTimeStart )
{
   int nSpan = FUZ_GetMilliStart() - nTimeStart;
   if ( nSpan < 0 )
      nSpan += 0x100000 * 1000;
   return nSpan;
}


unsigned int FUZ_rand(unsigned int* src)
{
    *src = XXH32(&src, sizeof(src), 0);
    return *src;
}


#define FUZ_RAND15BITS  ((FUZ_rand(seed) >> 3) & 32767)
#define FUZ_RANDLENGTH  ( ((FUZ_rand(seed) >> 7) & 3) ? (FUZ_rand(seed) % 14) : (FUZ_rand(seed) & 511) + 15)
void FUZ_fillCompressibleNoiseBuffer(void* buffer, int bufferSize, double proba, U32* seed)
{
    BYTE* BBuffer = (BYTE*)buffer;
    int pos = 0;
    U32 P32 = (U32)(32768 * proba);

    // First Byte
    BBuffer[pos++] = (BYTE)(FUZ_rand(seed));

    while (pos < bufferSize)
    {
        // Select : Literal (noise) or copy (within 64K)
        if (FUZ_RAND15BITS < P32)
        {
            // Copy (within 64K)
            int ref, d;
            int length = FUZ_RANDLENGTH + 4;
            int offset = FUZ_RAND15BITS + 1;
            if (offset > pos) offset = pos;
            if (pos + length > bufferSize) length = bufferSize - pos;
            ref = pos - offset;
            d = pos + length;
            while (pos < d) BBuffer[pos++] = BBuffer[ref++];
        }
        else
        {
            // Literal (noise)
            int d;
            int length = FUZ_RANDLENGTH;
            if (pos + length > bufferSize) length = bufferSize - pos;
            d = pos + length;
            while (pos < d) BBuffer[pos++] = (BYTE)(FUZ_rand(seed) >> 5);
        }
    }
}


int FUZ_SecurityTest()
{
  char* output;
  char* input;
  int i, r;

  printf("Overflow test (issue 52)...\n");
  input = (char*) malloc (20<<20);
  output = (char*) malloc (20<<20);
  input[0] = 0x0F;
  input[1] = 0x00;
  input[2] = 0x00;
  for(i = 3; i < 16840000; i++)
    input[i] = 0xff;
  r = LZ4_decompress_fast(input, output, 20<<20);

  free(input);
  free(output);
  printf(" Passed (return = %i < 0)\n",r);
  return 0;
}


int main(int argc, char** argv) {
        const int no_prompt = (argc > 1) && (!strcmp(argv[1], "--no-prompt"));
        unsigned long long bytes = 0;
        unsigned long long cbytes = 0;
        unsigned long long hcbytes = 0;
        unsigned long long ccbytes = 0;
        void* CNBuffer;
        char* compressedBuffer;
        char* decodedBuffer;
#       define FUZ_max   LZ4_COMPRESSBOUND(LEN)
#       define FUZ_avail ROUND_PAGE(FUZ_max)
        unsigned int seed=0, randState=0, timestamp=FUZ_GetMilliStart();
        int ret, attemptNb;
        char userInput[30] = {0};
#       define FUZ_CHECKTEST(cond, ...) if (cond) { printf("Test %i : ", testNb); printf(__VA_ARGS__); \
                                        printf(" (seed %u, cycle %i) \n", seed, attemptNb); goto _output_error; }
#       define FUZ_DISPLAYTEST          testNb++; no_prompt ? 0 : printf("%2i\b\b", testNb);
        void* stateLZ4   = malloc(LZ4_sizeofState());
        void* stateLZ4HC = malloc(LZ4_sizeofStateHC());
        void* LZ4continue;
        U32 crcOrig, crcCheck;

        // Get Seed
        printf("Starting LZ4 fuzzer (%i-bits, %s)\n", (int)(sizeof(size_t)*8), LZ4_VERSION);
        printf("Select an Initialisation number (default : random) : ");
        fflush(stdout);
        if ( no_prompt || fgets(userInput, sizeof userInput, stdin) )
        {
            if ( sscanf(userInput, "%u", &seed) == 1 ) {}
            else seed = FUZ_GetMilliSpan(timestamp);
        }
        printf("Seed = %u\n", seed);
        randState = seed;

        //FUZ_SecurityTest();

        // Create compressible test buffer
        CNBuffer = malloc(COMPRESSIBLE_NOISE_LENGTH);
        FUZ_fillCompressibleNoiseBuffer(CNBuffer, COMPRESSIBLE_NOISE_LENGTH, 0.5, &randState);
        compressedBuffer = malloc(LZ4_compressBound(FUZ_MAX_BLOCK_SIZE));
        decodedBuffer = malloc(LZ4_compressBound(FUZ_MAX_BLOCK_SIZE));

        // Test loop
        for (attemptNb = 0; attemptNb < NB_ATTEMPTS; attemptNb++)
        {
            int testNb = 0;
            char* dict;
            char* block;
            int dictSize, blockSize, blockStart, compressedSize, HCcompressedSize;
            int blockContinueCompressedSize;

            // note : promptThrottle is throtting stdout to prevent
            //        Travis-CI's output limit (10MB) and false hangup detection.
            const int promptThrottle = ((attemptNb % (NB_ATTEMPTS / 100)) == 0);
            if (!no_prompt || attemptNb == 0 || promptThrottle)
            {
                printf("\r%7i /%7i   - ", attemptNb, NB_ATTEMPTS);
                if (no_prompt) fflush(stdout);
            }

            // Select block to test
            blockSize  = FUZ_rand(&randState) % FUZ_MAX_BLOCK_SIZE;
            blockStart = FUZ_rand(&randState) % (COMPRESSIBLE_NOISE_LENGTH - blockSize);
            dictSize   = FUZ_rand(&randState) % FUZ_MAX_DICT_SIZE;
            if (dictSize > blockStart) dictSize = blockStart;
            block = ((char*)CNBuffer) + blockStart;
            dict = block - dictSize;

            /* Compression tests */

            // Test compression HC
            FUZ_DISPLAYTEST;
            ret = LZ4_compressHC(block, compressedBuffer, blockSize);
            FUZ_CHECKTEST(ret==0, "LZ4_compressHC() failed");
            HCcompressedSize = ret;

            // Test compression HC using external state
            FUZ_DISPLAYTEST;
            ret = LZ4_compressHC_withStateHC(stateLZ4HC, block, compressedBuffer, blockSize);
            FUZ_CHECKTEST(ret==0, "LZ4_compressHC_withStateHC() failed");

            // Test compression using external state
            FUZ_DISPLAYTEST;
            ret = LZ4_compress_withState(stateLZ4, block, compressedBuffer, blockSize);
            FUZ_CHECKTEST(ret==0, "LZ4_compress_withState() failed");

            // Test compression
            FUZ_DISPLAYTEST;
            ret = LZ4_compress(block, compressedBuffer, blockSize);
            FUZ_CHECKTEST(ret==0, "LZ4_compress() failed");
            compressedSize = ret;

            /* Decompression tests */

            crcOrig = XXH32(block, blockSize, 0);

            // Test decoding with output size being exactly what's necessary => must work
            FUZ_DISPLAYTEST;
            ret = LZ4_decompress_fast(compressedBuffer, decodedBuffer, blockSize);
            FUZ_CHECKTEST(ret<0, "LZ4_decompress_fast failed despite correct space");
            FUZ_CHECKTEST(ret!=compressedSize, "LZ4_decompress_fast failed : did not fully read compressed data");
            crcCheck = XXH32(decodedBuffer, blockSize, 0);
            FUZ_CHECKTEST(crcCheck!=crcOrig, "LZ4_decompress_fast corrupted decoded data");

            // Test decoding with one byte missing => must fail
            FUZ_DISPLAYTEST;
            decodedBuffer[blockSize-1] = 0;
            ret = LZ4_decompress_fast(compressedBuffer, decodedBuffer, blockSize-1);
            FUZ_CHECKTEST(ret>=0, "LZ4_decompress_fast should have failed, due to Output Size being too small");
            FUZ_CHECKTEST(decodedBuffer[blockSize-1], "LZ4_decompress_fast overrun specified output buffer");

            // Test decoding with one byte too much => must fail
            FUZ_DISPLAYTEST;
            ret = LZ4_decompress_fast(compressedBuffer, decodedBuffer, blockSize+1);
            FUZ_CHECKTEST(ret>=0, "LZ4_decompress_fast should have failed, due to Output Size being too large");

            // Test decoding with output size exactly what's necessary => must work
            FUZ_DISPLAYTEST;
            decodedBuffer[blockSize] = 0;
            ret = LZ4_decompress_safe(compressedBuffer, decodedBuffer, compressedSize, blockSize);
            FUZ_CHECKTEST(ret<0, "LZ4_decompress_safe failed despite sufficient space");
            FUZ_CHECKTEST(ret!=blockSize, "LZ4_decompress_safe did not regenerate original data");
            FUZ_CHECKTEST(decodedBuffer[blockSize], "LZ4_decompress_safe overrun specified output buffer size");
            crcCheck = XXH32(decodedBuffer, blockSize, 0);
            FUZ_CHECKTEST(crcCheck!=crcOrig, "LZ4_decompress_safe corrupted decoded data");

            // Test decoding with more than enough output size => must work
            FUZ_DISPLAYTEST;
            decodedBuffer[blockSize] = 0;
            decodedBuffer[blockSize+1] = 0;
            ret = LZ4_decompress_safe(compressedBuffer, decodedBuffer, compressedSize, blockSize+1);
            FUZ_CHECKTEST(ret<0, "LZ4_decompress_safe failed despite amply sufficient space");
            FUZ_CHECKTEST(ret!=blockSize, "LZ4_decompress_safe did not regenerate original data");
            //FUZ_CHECKTEST(decodedBuffer[blockSize], "LZ4_decompress_safe wrote more than target size");   // well, is that an issue ?
            FUZ_CHECKTEST(decodedBuffer[blockSize+1], "LZ4_decompress_safe overrun specified output buffer size");
            crcCheck = XXH32(decodedBuffer, blockSize, 0);
            FUZ_CHECKTEST(crcCheck!=crcOrig, "LZ4_decompress_safe corrupted decoded data");

            // Test decoding with output size being one byte too short => must fail
            FUZ_DISPLAYTEST;
            decodedBuffer[blockSize-1] = 0;
            ret = LZ4_decompress_safe(compressedBuffer, decodedBuffer, compressedSize, blockSize-1);
            FUZ_CHECKTEST(ret>=0, "LZ4_decompress_safe should have failed, due to Output Size being one byte too short");
            FUZ_CHECKTEST(decodedBuffer[blockSize-1], "LZ4_decompress_safe overrun specified output buffer size");

            // Test decoding with output size being 10 bytes too short => must fail
            FUZ_DISPLAYTEST;
            if (blockSize>10)
            {
                decodedBuffer[blockSize-10] = 0;
                ret = LZ4_decompress_safe(compressedBuffer, decodedBuffer, compressedSize, blockSize-10);
                FUZ_CHECKTEST(ret>=0, "LZ4_decompress_safe should have failed, due to Output Size being 10 bytes too short");
                FUZ_CHECKTEST(decodedBuffer[blockSize-10], "LZ4_decompress_safe overrun specified output buffer size");
            }

            // Test decoding with input size being one byte too short => must fail
            FUZ_DISPLAYTEST;
            ret = LZ4_decompress_safe(compressedBuffer, decodedBuffer, compressedSize-1, blockSize);
            FUZ_CHECKTEST(ret>=0, "LZ4_decompress_safe should have failed, due to input size being one byte too short (blockSize=%i, ret=%i, compressedSize=%i)", blockSize, ret, compressedSize);

            // Test decoding with input size being one byte too large => must fail
            FUZ_DISPLAYTEST;
            decodedBuffer[blockSize] = 0;
            ret = LZ4_decompress_safe(compressedBuffer, decodedBuffer, compressedSize+1, blockSize);
            FUZ_CHECKTEST(ret>=0, "LZ4_decompress_safe should have failed, due to input size being too large");
            FUZ_CHECKTEST(decodedBuffer[blockSize], "LZ4_decompress_safe overrun specified output buffer size");

            // Test partial decoding with target output size being max/2 => must work
            FUZ_DISPLAYTEST;
            ret = LZ4_decompress_safe_partial(compressedBuffer, decodedBuffer, compressedSize, blockSize/2, blockSize);
            FUZ_CHECKTEST(ret<0, "LZ4_decompress_safe_partial failed despite sufficient space");

            // Test partial decoding with target output size being just below max => must work
            FUZ_DISPLAYTEST;
            ret = LZ4_decompress_safe_partial(compressedBuffer, decodedBuffer, compressedSize, blockSize-3, blockSize);
            FUZ_CHECKTEST(ret<0, "LZ4_decompress_safe_partial failed despite sufficient space");

            /* Test Compression with limited output size */

            // Test compression with output size being exactly what's necessary (should work)
            FUZ_DISPLAYTEST;
            ret = LZ4_compress_limitedOutput(block, compressedBuffer, blockSize, compressedSize);
            FUZ_CHECKTEST(ret==0, "LZ4_compress_limitedOutput() failed despite sufficient space");

            // Test compression with output size being exactly what's necessary and external state (should work)
            FUZ_DISPLAYTEST;
            ret = LZ4_compress_limitedOutput_withState(stateLZ4, block, compressedBuffer, blockSize, compressedSize);
            FUZ_CHECKTEST(ret==0, "LZ4_compress_limitedOutput_withState() failed despite sufficient space");

            // Test HC compression with output size being exactly what's necessary (should work)
            FUZ_DISPLAYTEST;
            ret = LZ4_compressHC_limitedOutput(block, compressedBuffer, blockSize, HCcompressedSize);
            FUZ_CHECKTEST(ret==0, "LZ4_compressHC_limitedOutput() failed despite sufficient space");

            // Test HC compression with output size being exactly what's necessary (should work)
            FUZ_DISPLAYTEST;
            ret = LZ4_compressHC_limitedOutput_withStateHC(stateLZ4HC, block, compressedBuffer, blockSize, HCcompressedSize);
            FUZ_CHECKTEST(ret==0, "LZ4_compressHC_limitedOutput_withStateHC() failed despite sufficient space");

            // Test compression with just one missing byte into output buffer => must fail
            FUZ_DISPLAYTEST;
            compressedBuffer[compressedSize-1] = 0;
            ret = LZ4_compress_limitedOutput(block, compressedBuffer, blockSize, compressedSize-1);
            FUZ_CHECKTEST(ret, "LZ4_compress_limitedOutput should have failed (output buffer too small by 1 byte)");
            FUZ_CHECKTEST(compressedBuffer[compressedSize-1], "LZ4_compress_limitedOutput overran output buffer")

            // Test HC compression with just one missing byte into output buffer => must fail
            FUZ_DISPLAYTEST;
            compressedBuffer[compressedSize-1] = 0;
            ret = LZ4_compressHC_limitedOutput(block, compressedBuffer, blockSize, HCcompressedSize-1);
            FUZ_CHECKTEST(ret, "LZ4_compressHC_limitedOutput should have failed (output buffer too small by 1 byte)");
            FUZ_CHECKTEST(compressedBuffer[compressedSize-1], "LZ4_compressHC_limitedOutput overran output buffer")

            /* Dictionary tests */

            // Compress using dictionary
            FUZ_DISPLAYTEST;
            LZ4continue = LZ4_create (dict);
            LZ4_compress_continue (LZ4continue, dict, compressedBuffer, dictSize);   // Just to fill hash tables
            blockContinueCompressedSize = LZ4_compress_continue (LZ4continue, block, compressedBuffer, blockSize);
            FUZ_CHECKTEST(blockContinueCompressedSize==0, "LZ4_compress_continue failed");
            LZ4_free (LZ4continue);

            // Decompress with dictionary as prefix
            FUZ_DISPLAYTEST;
            memcpy(decodedBuffer, dict, dictSize);
            ret = LZ4_decompress_fast_withPrefix64k(compressedBuffer, decodedBuffer+dictSize, blockSize);
            FUZ_CHECKTEST(ret!=blockContinueCompressedSize, "LZ4_decompress_fast_withPrefix64k did not read all compressed block input");
            crcCheck = XXH32(decodedBuffer+dictSize, blockSize, 0);
            FUZ_CHECKTEST(crcCheck!=crcOrig, "LZ4_decompress_fast_withPrefix64k corrupted decoded data");

            FUZ_DISPLAYTEST;
            ret = LZ4_decompress_safe_withPrefix64k(compressedBuffer, decodedBuffer+dictSize, blockContinueCompressedSize, blockSize);
            FUZ_CHECKTEST(ret!=blockSize, "LZ4_decompress_safe_withPrefix64k did not regenerate original data");
            crcCheck = XXH32(decodedBuffer+dictSize, blockSize, 0);
            FUZ_CHECKTEST(crcCheck!=crcOrig, "LZ4_decompress_safe_withPrefix64k corrupted decoded data");

            // Decompress with dictionary as external
            FUZ_DISPLAYTEST;
            decodedBuffer[blockSize] = 0;
            ret = LZ4_decompress_fast_withDict(compressedBuffer, decodedBuffer, blockSize, dict, dictSize);
            FUZ_CHECKTEST(ret!=blockContinueCompressedSize, "LZ4_decompress_fast_withDict did not read all compressed block input");
            FUZ_CHECKTEST(decodedBuffer[blockSize], "LZ4_decompress_fast_withDict overrun specified output buffer size")
            crcCheck = XXH32(decodedBuffer, blockSize, 0);
            FUZ_CHECKTEST(crcCheck!=crcOrig, "LZ4_decompress_fast_withDict corrupted decoded data");

            FUZ_DISPLAYTEST;
            decodedBuffer[blockSize] = 0;
            ret = LZ4_decompress_safe_withDict(compressedBuffer, decodedBuffer, blockContinueCompressedSize, blockSize, dict, dictSize);
            FUZ_CHECKTEST(ret!=blockSize, "LZ4_decompress_safe_withDict did not regenerate original data");
            FUZ_CHECKTEST(decodedBuffer[blockSize], "LZ4_decompress_safe_withDict overrun specified output buffer size")
            crcCheck = XXH32(decodedBuffer, blockSize, 0);
            FUZ_CHECKTEST(crcCheck!=crcOrig, "LZ4_decompress_safe_withDict corrupted decoded data");

            FUZ_DISPLAYTEST;
            decodedBuffer[blockSize-1] = 0;
            ret = LZ4_decompress_fast_withDict(compressedBuffer, decodedBuffer, blockSize-1, dict, dictSize);
            FUZ_CHECKTEST(ret>=0, "LZ4_decompress_fast_withDict should have failed : wrong original size (-1 byte)");
            FUZ_CHECKTEST(decodedBuffer[blockSize-1], "LZ4_decompress_fast_withDict overrun specified output buffer size");

            FUZ_DISPLAYTEST;
            decodedBuffer[blockSize-1] = 0;
            ret = LZ4_decompress_safe_withDict(compressedBuffer, decodedBuffer, blockContinueCompressedSize, blockSize-1, dict, dictSize);
            FUZ_CHECKTEST(ret>=0, "LZ4_decompress_safe_withDict should have failed : not enough output size (-1 byte)");
            FUZ_CHECKTEST(decodedBuffer[blockSize-1], "LZ4_decompress_safe_withDict overrun specified output buffer size");

            FUZ_DISPLAYTEST;
            if (blockSize > 10)
            {
                decodedBuffer[blockSize-10] = 0;
                ret = LZ4_decompress_safe_withDict(compressedBuffer, decodedBuffer, blockContinueCompressedSize, blockSize-10, dict, dictSize);
                FUZ_CHECKTEST(ret>=0, "LZ4_decompress_safe_withDict should have failed : not enough output size (-10 byte)");
                FUZ_CHECKTEST(decodedBuffer[blockSize-10], "LZ4_decompress_safe_withDict overrun specified output buffer size (-10 byte) (blockSize=%i)", blockSize);
            }


            // Fill stats
            bytes += blockSize;
            cbytes += compressedSize;
            hcbytes += HCcompressedSize;
            ccbytes += blockContinueCompressedSize;
        }

        printf("\r%7i /%7i   - ", attemptNb, NB_ATTEMPTS);
        printf("all tests completed successfully \n");
        printf("compression ratio: %0.3f%%\n", (double)cbytes/bytes*100);
        printf("HC compression ratio: %0.3f%%\n", (double)hcbytes/bytes*100);
        printf("ratio with dict: %0.3f%%\n", (double)ccbytes/bytes*100);

        // unalloc
        if(!no_prompt) getchar();
        free(CNBuffer);
        free(compressedBuffer);
        free(decodedBuffer);
        return 0;

_output_error:
        if(!no_prompt) getchar();
        free(CNBuffer);
        free(compressedBuffer);
        free(decodedBuffer);
        return 1;
}
