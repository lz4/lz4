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
#define FUZ_COMPRESSIBILITY_DEFAULT 50
#define PRIME1   2654435761U
#define PRIME2   2246822519U
#define PRIME3   3266489917U


//**************************************
// Macros
//**************************************
#define DISPLAY(...)         fprintf(stderr, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...) if (displayLevel>=l) { DISPLAY(__VA_ARGS__); }


/*****************************************
  Local Parameters
*****************************************/
static int no_prompt = 0;
static char* programName;
static int displayLevel = 2;


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


#  define FUZ_rotl32(x,r) ((x << r) | (x >> (32 - r)))
unsigned int FUZ_rand(unsigned int* src)
{
    U32 rand32 = *src;
    rand32 *= PRIME1;
    rand32 += PRIME2;
    rand32  = FUZ_rotl32(rand32, 13);
    *src = rand32;
    return rand32;
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

#define FUZ_MAX(a,b) (a>b?a:b)

int FUZ_test(U32 seed, int nbCycles, int startCycle, double compressibility) {
        unsigned long long bytes = 0;
        unsigned long long cbytes = 0;
        unsigned long long hcbytes = 0;
        unsigned long long ccbytes = 0;
        void* CNBuffer;
        char* compressedBuffer;
        char* decodedBuffer;
#       define FUZ_max   LZ4_COMPRESSBOUND(LEN)
        unsigned int randState=seed;
        int ret, cycleNb;
#       define FUZ_CHECKTEST(cond, ...) if (cond) { printf("Test %i : ", testNb); printf(__VA_ARGS__); \
                                        printf(" (seed %u, cycle %i) \n", seed, cycleNb); goto _output_error; }
#       define FUZ_DISPLAYTEST          { testNb++; ((displayLevel<3) || no_prompt) ? 0 : printf("%2i\b\b", testNb); if (displayLevel==4) fflush(stdout); }
        void* stateLZ4   = malloc(LZ4_sizeofState());
        void* stateLZ4HC = malloc(LZ4_sizeofStateHC());
        void* LZ4continue;
        LZ4_stream_t LZ4dict;
        U32 crcOrig, crcCheck;
        int displayRefresh;


        // init
        memset(&LZ4dict, 0, sizeof(LZ4dict));

        // Create compressible test buffer
        CNBuffer = malloc(COMPRESSIBLE_NOISE_LENGTH);
        FUZ_fillCompressibleNoiseBuffer(CNBuffer, COMPRESSIBLE_NOISE_LENGTH, compressibility, &randState);
        compressedBuffer = malloc(LZ4_compressBound(FUZ_MAX_BLOCK_SIZE));
        decodedBuffer = malloc(FUZ_MAX_DICT_SIZE + FUZ_MAX_BLOCK_SIZE);

        // display refresh rate
        switch(displayLevel)
        {
        case 0: displayRefresh = nbCycles+1; break;
        case 1: displayRefresh=FUZ_MAX(1, nbCycles / 100); break;
        case 2: displayRefresh=89; break;
        default : displayRefresh=1;
        }

        // move to startCycle
        for (cycleNb = 0; cycleNb < startCycle; cycleNb++)
        {
            // synd rand & dict
            int dictSize, blockSize, blockStart;
            char* dict;
            char* block;

            blockSize  = FUZ_rand(&randState) % FUZ_MAX_BLOCK_SIZE;
            blockStart = FUZ_rand(&randState) % (COMPRESSIBLE_NOISE_LENGTH - blockSize);
            dictSize   = FUZ_rand(&randState) % FUZ_MAX_DICT_SIZE;
            if (dictSize > blockStart) dictSize = blockStart;
            block = ((char*)CNBuffer) + blockStart;
            dict = block - dictSize;
            LZ4_loadDict(&LZ4dict, dict, dictSize);
            LZ4_compress_continue(&LZ4dict, block, compressedBuffer, blockSize);
            LZ4_loadDict(&LZ4dict, dict, dictSize);
            LZ4_compress_continue(&LZ4dict, block, compressedBuffer, blockSize);
            LZ4_loadDict(&LZ4dict, dict, dictSize);
            LZ4_compress_continue(&LZ4dict, block, compressedBuffer, blockSize);
        }

        // Test loop
        for (cycleNb = startCycle; cycleNb < nbCycles; cycleNb++)
        {
            int testNb = 0;
            char* dict;
            char* block;
            int dictSize, blockSize, blockStart, compressedSize, HCcompressedSize;
            int blockContinueCompressedSize;

            if ((cycleNb%displayRefresh) == 0)
            {
                printf("\r%7i /%7i   - ", cycleNb, nbCycles);
                fflush(stdout);
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
            //FUZ_CHECKTEST(decodedBuffer[blockSize], "LZ4_decompress_safe wrote more than (unknown) target size");   // well, is that an issue ?
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
            if (crcCheck!=crcOrig)
            {
                int i=0;
                while (block[i]==decodedBuffer[i]) i++;
                printf("Wrong Byte at position %i/%i\n", i, blockSize);

            }
            FUZ_CHECKTEST(crcCheck!=crcOrig, "LZ4_decompress_fast_withPrefix64k corrupted decoded data (dict %i)", dictSize);

            FUZ_DISPLAYTEST;
            ret = LZ4_decompress_safe_withPrefix64k(compressedBuffer, decodedBuffer+dictSize, blockContinueCompressedSize, blockSize);
            FUZ_CHECKTEST(ret!=blockSize, "LZ4_decompress_safe_withPrefix64k did not regenerate original data");
            crcCheck = XXH32(decodedBuffer+dictSize, blockSize, 0);
            FUZ_CHECKTEST(crcCheck!=crcOrig, "LZ4_decompress_safe_withPrefix64k corrupted decoded data");

            // Compress using External dictionary
            FUZ_DISPLAYTEST;
            dict -= 9;   // Separation, so it is an ExtDict
            if (dict < (char*)CNBuffer) dict = (char*)CNBuffer;
            LZ4_loadDict(&LZ4dict, dict, dictSize);
            blockContinueCompressedSize = LZ4_compress_continue(&LZ4dict, block, compressedBuffer, blockSize);
            FUZ_CHECKTEST(blockContinueCompressedSize==0, "LZ4_compress_continue failed");

            FUZ_DISPLAYTEST;
            LZ4_loadDict(&LZ4dict, dict, dictSize);
            ret = LZ4_compress_limitedOutput_continue(&LZ4dict, block, compressedBuffer, blockSize, blockContinueCompressedSize-1);
            FUZ_CHECKTEST(ret>0, "LZ4_compress_limitedOutput_continue using ExtDict should fail : one missing byte for output buffer");

            FUZ_DISPLAYTEST;
            LZ4_loadDict(&LZ4dict, dict, dictSize);
            ret = LZ4_compress_limitedOutput_continue(&LZ4dict, block, compressedBuffer, blockSize, blockContinueCompressedSize);
            FUZ_CHECKTEST(ret!=blockContinueCompressedSize, "LZ4_compress_limitedOutput_compressed size is different (%i != %i)", ret, blockContinueCompressedSize);
            FUZ_CHECKTEST(ret<=0, "LZ4_compress_limitedOutput_continue should work : enough size available within output buffer");

            // Decompress with dictionary as external
            FUZ_DISPLAYTEST;
            decodedBuffer[blockSize] = 0;
            ret = LZ4_decompress_fast_usingDict(compressedBuffer, decodedBuffer, blockSize, dict, dictSize);
            FUZ_CHECKTEST(ret!=blockContinueCompressedSize, "LZ4_decompress_fast_usingDict did not read all compressed block input");
            FUZ_CHECKTEST(decodedBuffer[blockSize], "LZ4_decompress_fast_usingDict overrun specified output buffer size")
            crcCheck = XXH32(decodedBuffer, blockSize, 0);
            if (crcCheck!=crcOrig)
            {
                int i=0;
                while (block[i]==decodedBuffer[i]) i++;
                printf("Wrong Byte at position %i/%i\n", i, blockSize);

            }
            FUZ_CHECKTEST(crcCheck!=crcOrig, "LZ4_decompress_fast_usingDict corrupted decoded data (dict %i)", dictSize);

            FUZ_DISPLAYTEST;
            decodedBuffer[blockSize] = 0;
            ret = LZ4_decompress_safe_usingDict(compressedBuffer, decodedBuffer, blockContinueCompressedSize, blockSize, dict, dictSize);
            FUZ_CHECKTEST(ret!=blockSize, "LZ4_decompress_safe_usingDict did not regenerate original data");
            FUZ_CHECKTEST(decodedBuffer[blockSize], "LZ4_decompress_safe_usingDict overrun specified output buffer size")
            crcCheck = XXH32(decodedBuffer, blockSize, 0);
            FUZ_CHECKTEST(crcCheck!=crcOrig, "LZ4_decompress_safe_usingDict corrupted decoded data");

            FUZ_DISPLAYTEST;
            decodedBuffer[blockSize-1] = 0;
            ret = LZ4_decompress_fast_usingDict(compressedBuffer, decodedBuffer, blockSize-1, dict, dictSize);
            FUZ_CHECKTEST(ret>=0, "LZ4_decompress_fast_withDict should have failed : wrong original size (-1 byte)");
            FUZ_CHECKTEST(decodedBuffer[blockSize-1], "LZ4_decompress_fast_usingDict overrun specified output buffer size");

            FUZ_DISPLAYTEST;
            decodedBuffer[blockSize-1] = 0;
            ret = LZ4_decompress_safe_usingDict(compressedBuffer, decodedBuffer, blockContinueCompressedSize, blockSize-1, dict, dictSize);
            FUZ_CHECKTEST(ret>=0, "LZ4_decompress_safe_usingDict should have failed : not enough output size (-1 byte)");
            FUZ_CHECKTEST(decodedBuffer[blockSize-1], "LZ4_decompress_safe_usingDict overrun specified output buffer size");

            FUZ_DISPLAYTEST;
            if (blockSize > 10)
            {
                decodedBuffer[blockSize-10] = 0;
                ret = LZ4_decompress_safe_usingDict(compressedBuffer, decodedBuffer, blockContinueCompressedSize, blockSize-10, dict, dictSize);
                FUZ_CHECKTEST(ret>=0, "LZ4_decompress_safe_usingDict should have failed : output buffer too small (-10 byte)");
                FUZ_CHECKTEST(decodedBuffer[blockSize-10], "LZ4_decompress_safe_usingDict overrun specified output buffer size (-10 byte) (blockSize=%i)", blockSize);
            }


            // Fill stats
            bytes += blockSize;
            cbytes += compressedSize;
            hcbytes += HCcompressedSize;
            ccbytes += blockContinueCompressedSize;
        }

        printf("\r%7i /%7i   - ", cycleNb, nbCycles);
        printf("all tests completed successfully \n");
        printf("compression ratio: %0.3f%%\n", (double)cbytes/bytes*100);
        printf("HC compression ratio: %0.3f%%\n", (double)hcbytes/bytes*100);
        printf("ratio with dict: %0.3f%%\n", (double)ccbytes/bytes*100);

        // unalloc
        if(!no_prompt) getchar();
        free(CNBuffer);
        free(compressedBuffer);
        free(decodedBuffer);
        free(stateLZ4);
        free(stateLZ4HC);
        return 0;

_output_error:
        if(!no_prompt) getchar();
        free(CNBuffer);
        free(compressedBuffer);
        free(decodedBuffer);
        free(stateLZ4);
        free(stateLZ4HC);
        return 1;
}


int FUZ_usage()
{
    DISPLAY( "Usage :\n");
    DISPLAY( "      %s [args]\n", programName);
    DISPLAY( "\n");
    DISPLAY( "Arguments :\n");
    DISPLAY( " -i#    : Nb of tests (default:%i) \n", NB_ATTEMPTS);
    DISPLAY( " -s#    : Select seed (default:prompt user)\n");
    DISPLAY( " -t#    : Select starting test number (default:0)\n");
    DISPLAY( " -p#    : Select compressibility in %% (default:%i%%)\n", FUZ_COMPRESSIBILITY_DEFAULT);
    DISPLAY( " -v     : verbose\n");
    DISPLAY( " -h     : display help and exit\n");
    return 0;
}


int main(int argc, char** argv) {
    char userInput[50] = {0};
    U32 timestamp = FUZ_GetMilliStart();
    U32 seed=0;
    int seedset=0;
    int argNb;
    int nbTests = NB_ATTEMPTS;
    int testNb = 0;
    int proba = FUZ_COMPRESSIBILITY_DEFAULT;

    // Check command line
    programName = argv[0];
    for(argNb=1; argNb<argc; argNb++)
    {
        char* argument = argv[argNb];

        if(!argument) continue;   // Protection if argument empty

        // Decode command (note : aggregated commands are allowed)
        if (argument[0]=='-')
        {
            if (!strcmp(argument, "--no-prompt")) { no_prompt=1; seedset=1; displayLevel=1; continue; }

            while (argument[1]!=0)
            {
                argument++;
                switch(*argument)
                {
                case 'h':
                    return FUZ_usage();
                case 'v':
                    argument++;
                    displayLevel=4;
                    break;
                case 'i':
                    argument++;
                    nbTests=0;
                    while ((*argument>='0') && (*argument<='9'))
                    {
                        nbTests *= 10;
                        nbTests += *argument - '0';
                        argument++;
                    }
                    break;
                case 's':
                    argument++;
                    seed=0; seedset=1;
                    while ((*argument>='0') && (*argument<='9'))
                    {
                        seed *= 10;
                        seed += *argument - '0';
                        argument++;
                    }
                    break;
                case 't':
                    argument++;
                    testNb=0;
                    while ((*argument>='0') && (*argument<='9'))
                    {
                        testNb *= 10;
                        testNb += *argument - '0';
                        argument++;
                    }
                    break;
                case 'p':
                    argument++;
                    proba=0;
                    while ((*argument>='0') && (*argument<='9'))
                    {
                        proba *= 10;
                        proba += *argument - '0';
                        argument++;
                    }
                    if (proba<0) proba=0;
                    if (proba>100) proba=100;
                    break;
                default: ;
                }
            }

        }
    }

    // Get Seed
    printf("Starting LZ4 fuzzer (%i-bits, %s)\n", (int)(sizeof(size_t)*8), LZ4_VERSION);

    if (!seedset)
    {
        printf("Select an Initialisation number (default : random) : ");
        fflush(stdout);
        if ( no_prompt || fgets(userInput, sizeof userInput, stdin) )
        {
            if ( sscanf(userInput, "%u", &seed) == 1 ) {}
            else seed = FUZ_GetMilliSpan(timestamp);
        }
    }
    printf("Seed = %u\n", seed);
    if (proba!=FUZ_COMPRESSIBILITY_DEFAULT) printf("Compressibility : %i%%\n", proba);

    //FUZ_SecurityTest();

    if (nbTests<=0) nbTests=1;

    return FUZ_test(seed, nbTests, testNb, ((double)proba) / 100);
}
