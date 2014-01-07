/*
    fuzzer.c - Fuzzer test tool for LZ4
    Copyright (C) Yann Collet - Andrew Mahone 2012-2013
    Code started by Andrew Mahone, modified by Yann Collet
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

//**************************************
// Remove Visual warning messages
//**************************************
#define _CRT_SECURE_NO_WARNINGS  // fgets


//**************************************
// Includes
//**************************************
#include <stdlib.h>
#include <stdio.h>      // fgets, sscanf
#include <sys/timeb.h>  // timeb
#include "lz4.h"
#include "lz4hc.h"


//**************************************
// Constants
//**************************************
#ifndef LZ4_VERSION
#  define LZ4_VERSION ""
#endif

#define NB_ATTEMPTS (1<<17)
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


//*********************************************************
//  Functions
//*********************************************************
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
    *src =  ((*src) * PRIME1) + PRIME2;
    return *src;
}


int test_canary(unsigned char *buf)
{
    int i;
    for (i = 0; i < 2048; i++)
        if (buf[i] != buf[i + 2048])
            return 0;
    return 1;
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


//int main(int argc, char *argv[]) {
int main() {
        unsigned long long bytes = 0;
        unsigned long long cbytes = 0;
        unsigned long long hcbytes = 0;
        unsigned char buf[LEN];
        unsigned char testOut[LEN+1];
#       define FUZ_max   LZ4_COMPRESSBOUND(LEN)
#       define FUZ_avail ROUND_PAGE(FUZ_max)
        const int off_full = FUZ_avail - FUZ_max;
        unsigned char cbuf[FUZ_avail + PAGE_SIZE];
        unsigned int seed, randState, cur_seq=PRIME3, seeds[NUM_SEQ], timestamp=FUZ_GetMilliStart();
        int i, j, k, ret, len, lenHC, attemptNb;
        char userInput[30] = {0};
#       define FUZ_CHECKTEST(cond, message) if (cond) { printf("Test %i : %s : seed %u, cycle %i \n", testNb, message, seed, attemptNb); goto _output_error; }
#       define FUZ_DISPLAYTEST              testNb++; printf("%2i\b\b", testNb);
        void* stateLZ4   = malloc(LZ4_sizeofState());
        void* stateLZ4HC = malloc(LZ4_sizeofStateHC());

        printf("starting LZ4 fuzzer (%s)\n", LZ4_VERSION);
        printf("Select an Initialisation number (default : random) : ");
        fflush(stdout);
        if ( fgets(userInput, sizeof userInput, stdin) )
        {
            if ( sscanf(userInput, "%d", &seed) == 1 ) {}
            else seed = FUZ_GetMilliSpan(timestamp);
        }
        printf("Seed = %u\n", seed);
        randState = seed;

        //FUZ_SecurityTest();

        for (i = 0; i < 2048; i++)
                cbuf[FUZ_avail + i] = cbuf[FUZ_avail + 2048 + i] = FUZ_rand(&randState) >> 16;

        for (attemptNb = 0; attemptNb < NB_ATTEMPTS; attemptNb++)
        {
            int testNb = 0;

            printf("\r%7i /%7i   - ", attemptNb, NB_ATTEMPTS);

            for (j = 0; j < NUM_SEQ; j++) {
                    seeds[j] = FUZ_rand(&randState) << 8;
                    seeds[j] ^= (FUZ_rand(&randState) >> 8) & 65535;
            }
            for (j = 0; j < LEN; j++) {
                    k = FUZ_rand(&randState);
                    if (j == 0 || NEW_SEQ(k))
                            cur_seq = seeds[(FUZ_rand(&randState) >> 16) & SEQ_MSK];
                    if (MOD_SEQ(k)) {
                            k = (FUZ_rand(&randState) >> 16) & SEQ_MSK;
                            seeds[k] = FUZ_rand(&randState) << 8;
                            seeds[k] ^= (FUZ_rand(&randState) >> 8) & 65535;
                    }
                    buf[j] = FUZ_rand(&cur_seq) >> 16;
            }

            // Test compression HC
            FUZ_DISPLAYTEST;   // 1
            ret = LZ4_compressHC_limitedOutput((const char*)buf, (char*)&cbuf[off_full], LEN, FUZ_max);
            FUZ_CHECKTEST(ret==0, "LZ4_compressHC_limitedOutput() failed despite sufficient space");
            lenHC = ret;

            // Test compression HC using external state
            FUZ_DISPLAYTEST;   // 1
            ret = LZ4_compressHC_withStateHC(stateLZ4HC, (const char*)buf, (char*)&cbuf[off_full], LEN);
            FUZ_CHECKTEST(ret==0, "LZ4_compressHC_withStateHC() failed");

            // Test compression using external state
            FUZ_DISPLAYTEST;   // 2
            ret = LZ4_compress_withState(stateLZ4, (const char*)buf, (char*)&cbuf[off_full], LEN);
            FUZ_CHECKTEST(ret==0, "LZ4_compress_withState() failed");

            // Test compression
            FUZ_DISPLAYTEST;   // 2
            ret = LZ4_compress_limitedOutput((const char*)buf, (char*)&cbuf[off_full], LEN, FUZ_max);
            FUZ_CHECKTEST(ret==0, "LZ4_compress_limitedOutput() failed despite sufficient space");
            len = ret;

            // Test decoding with output size being exactly what's necessary => must work
            FUZ_DISPLAYTEST;   // 3
            ret = LZ4_decompress_fast((char*)&cbuf[off_full], (char*)testOut, LEN);
            FUZ_CHECKTEST(ret<0, "LZ4_decompress_fast failed despite correct space");

            // Test decoding with one byte missing => must fail
            FUZ_DISPLAYTEST;   // 4
            ret = LZ4_decompress_fast((char*)&cbuf[off_full], (char*)testOut, LEN-1);
            FUZ_CHECKTEST(ret>=0, "LZ4_decompress_fast should have failed, due to Output Size being too small");

            // Test decoding with one byte too much => must fail
            FUZ_DISPLAYTEST;
            ret = LZ4_decompress_fast((char*)&cbuf[off_full], (char*)testOut, LEN+1);
            FUZ_CHECKTEST(ret>=0, "LZ4_decompress_fast should have failed, due to Output Size being too large");

            // Test decoding with enough output size => must work
            FUZ_DISPLAYTEST;
            ret = LZ4_decompress_safe((char*)&cbuf[off_full], (char*)testOut, len, LEN+1);
            FUZ_CHECKTEST(ret<0, "LZ4_decompress_safe failed despite sufficient space");

            // Test decoding with output size being exactly what's necessary => must work
            FUZ_DISPLAYTEST;
            ret = LZ4_decompress_safe((char*)&cbuf[off_full], (char*)testOut, len, LEN);
            FUZ_CHECKTEST(ret<0, "LZ4_decompress_safe failed despite sufficient space");

            // Test decoding with output size being one byte too short => must fail
            FUZ_DISPLAYTEST;
            ret = LZ4_decompress_safe((char*)&cbuf[off_full], (char*)testOut, len, LEN-1);
            FUZ_CHECKTEST(ret>=0, "LZ4_decompress_safe should have failed, due to Output Size being one byte too short");

            // Test decoding with input size being one byte too short => must fail
            FUZ_DISPLAYTEST;
            ret = LZ4_decompress_safe((char*)&cbuf[off_full], (char*)testOut, len-1, LEN);
            FUZ_CHECKTEST(ret>=0, "LZ4_decompress_safe should have failed, due to input size being one byte too short");

            // Test decoding with input size being one byte too large => must fail
            FUZ_DISPLAYTEST;
            ret = LZ4_decompress_safe((char*)&cbuf[off_full], (char*)testOut, len+1, LEN);
            FUZ_CHECKTEST(ret>=0, "LZ4_decompress_safe should have failed, due to input size being too large");
            //if (ret>=0) { printf("Test 10 : decompression should have failed, due to input size being too large : seed %u, len %d\n", seed, LEN); goto _output_error; }

            // Test partial decoding with target output size being max/2 => must work
            FUZ_DISPLAYTEST;
            ret = LZ4_decompress_safe_partial((char*)&cbuf[off_full], (char*)testOut, len, LEN/2, LEN);
            FUZ_CHECKTEST(ret<0, "LZ4_decompress_safe_partial failed despite sufficient space");

            // Test partial decoding with target output size being just below max => must work
            FUZ_DISPLAYTEST;
            ret = LZ4_decompress_safe_partial((char*)&cbuf[off_full], (char*)testOut, len, LEN-3, LEN);
            FUZ_CHECKTEST(ret<0, "LZ4_decompress_safe_partial failed despite sufficient space");

            // Test compression with output size being exactly what's necessary (should work)
            FUZ_DISPLAYTEST;
            ret = LZ4_compress_limitedOutput((const char*)buf, (char*)&cbuf[FUZ_avail-len], LEN, len);
            FUZ_CHECKTEST(ret==0, "LZ4_compress_limitedOutput() failed despite sufficient space");
            FUZ_CHECKTEST(!test_canary(&cbuf[FUZ_avail]), "compression overran output buffer");

            // Test compression with output size being exactly what's necessary and external state (should work)
            FUZ_DISPLAYTEST;   // 2
            ret = LZ4_compress_limitedOutput_withState(stateLZ4, (const char*)buf, (char*)&cbuf[off_full], LEN, len);
            FUZ_CHECKTEST(ret==0, "LZ4_compress_limitedOutput_withState() failed despite sufficient space");
            FUZ_CHECKTEST(!test_canary(&cbuf[FUZ_avail]), "compression overran output buffer");

            // Test HC compression with output size being exactly what's necessary (should work)
            FUZ_DISPLAYTEST;
            ret = LZ4_compressHC_limitedOutput((const char*)buf, (char*)&cbuf[FUZ_avail-len], LEN, lenHC);
            FUZ_CHECKTEST(ret==0, "LZ4_compressHC_limitedOutput() failed despite sufficient space");

            // Test HC compression with output size being exactly what's necessary (should work)
            FUZ_DISPLAYTEST;
            ret = LZ4_compressHC_limitedOutput_withStateHC(stateLZ4HC, (const char*)buf, (char*)&cbuf[FUZ_avail-len], LEN, lenHC);
            FUZ_CHECKTEST(ret==0, "LZ4_compressHC_limitedOutput_withStateHC() failed despite sufficient space");

            // Test compression with just one missing byte into output buffer => must fail
            FUZ_DISPLAYTEST;
            ret = LZ4_compress_limitedOutput((const char*)buf, (char*)&cbuf[FUZ_avail-(len-1)], LEN, len-1);
            FUZ_CHECKTEST(ret, "compression overran output buffer");
            FUZ_CHECKTEST(!test_canary(&cbuf[FUZ_avail]), "compression overran output buffer");

            // Test HC compression with just one missing byte into output buffer => must fail
            FUZ_DISPLAYTEST;
            ret = LZ4_compressHC_limitedOutput((const char*)buf, (char*)&cbuf[FUZ_avail-(len-1)], LEN, lenHC-1);
            FUZ_CHECKTEST(ret, "HC compression overran output buffer");

            bytes += LEN;
            cbytes += len;
            hcbytes += lenHC;
            FUZ_rand(&randState);
        }

        printf("all tests completed successfully \n");
        printf("compression ratio: %0.3f%%\n", (double)cbytes/bytes*100);
        printf("HC compression ratio: %0.3f%%\n", (double)hcbytes/bytes*100);
        getchar();
        return 0;

_output_error:
        getchar();
        return 1;
}
