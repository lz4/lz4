/*
    fuzzer.c - Fuzzer test tool for LZ4
    Copyright (C) Andrew Mahone - Yann Collet 2012
	Original code by Andrew Mahone / Modified by Yann Collet
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
#include <stdio.h>      // fgets
#include <sys/timeb.h>  // timeb
#include "lz4.h"


//**************************************
// Constants
//**************************************
#define NB_ATTEMPTS (1<<18)
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


int test_canary(unsigned char *buf) {
        int i;
        for (i = 0; i < 2048; i++)
                if (buf[i] != buf[i + 2048])
                        return 0;
        return 1;
}

//int main(int argc, char *argv[]) {
int main() {
        unsigned long long bytes = 0;
        unsigned long long cbytes = 0;
        unsigned char buf[LEN];
#       undef  max
#       define max   LZ4_compressBound(LEN)
#       define avail ROUND_PAGE(max)
        const int off_full = avail - max;
        unsigned char cbuf[avail + PAGE_SIZE];
		unsigned int seed, cur_seq, seeds[NUM_SEQ], timestamp=FUZ_GetMilliStart();
        int i, j, k, ret, len;
		char userInput[30] = {0};

		printf("starting LZ4 fuzzer\n");
		printf("Select an Initialisation number (default : random) : ");
		fflush(stdout);
		if ( fgets(userInput, sizeof userInput, stdin) )
		{
			if ( sscanf(userInput, "%d", &seed) == 1 ) {}
			else seed = FUZ_GetMilliSpan(timestamp);
		}
		printf("Seed = %u\n", seed);

        for (i = 0; i < 2048; i++)
                cbuf[avail + i] = cbuf[avail + 2048 + i] = FUZ_rand(&seed) >> 16;

        for (i = 0; i < NB_ATTEMPTS; i++) {
			printf("\r%7i /%7i\r", i, NB_ATTEMPTS);
			FUZ_rand(&seed);
            for (j = 0; j < NUM_SEQ; j++) {
                    seeds[j] = FUZ_rand(&seed) << 8;
                    seeds[j] ^= (FUZ_rand(&seed) >> 8) & 65535;
            }
            for (j = 0; j < LEN; j++) {
                    k = FUZ_rand(&seed);
                    if (j == 0 || NEW_SEQ(k))
                            cur_seq = seeds[(FUZ_rand(&seed) >> 16) & SEQ_MSK];
                    if (MOD_SEQ(k)) {
                            k = (FUZ_rand(&seed) >> 16) & SEQ_MSK;
                            seeds[k] = FUZ_rand(&seed) << 8;
                            seeds[k] ^= (FUZ_rand(&seed) >> 8) & 65535;
                    }
                    buf[j] = FUZ_rand(&cur_seq) >> 16;
            }
            ret = LZ4_compress_limitedOutput((const char*)buf, (char*)&cbuf[off_full], LEN, max);
            len = ret;

			// Test compression with output size being exactly what's necessary
            ret = LZ4_compress_limitedOutput((const char*)buf, (char*)&cbuf[avail-len], LEN, len);
            if (!test_canary(&cbuf[avail])) { printf("compression overran output buffer: seed %u, len %d, olen %d\n", seed, LEN, len); return 1; }
            if (ret == 0) { printf("compression failed despite sufficient space: seed %u, len %d\n", seed, LEN); return 1; }

			// Test compression with just one missing byte into output buffer => should fail
            ret = LZ4_compress_limitedOutput((const char*)buf, (char*)&cbuf[avail-(len-1)], LEN, len-1);
            if (ret) { printf("compression overran output buffer: seed %u, len %d, olen %d => ret %d", seed, LEN, len-1, ret); return 1; }
            if (!test_canary(&cbuf[avail])) { printf("compression overran output buffer: seed %u, len %d, olen %d", seed, LEN, len-1); return 1; }

			/* No longer useful
			// Test compression with not enough output size
            ret = LZ4_compress_limitedOutput((const char*)buf, (char*)&cbuf[avail-len/2], LEN, len/2);
            if (!test_canary(&cbuf[avail])) { printf("compression overran output buffer: seed %u, len %d, olen %d", seed, LEN, len/2); return 1; }
			*/

			bytes += LEN;
            cbytes += len;
        }
		printf("all tests completed successfully \n");
        printf("compression ratio: %0.3f%%\n", (double)cbytes/bytes*100);
        return 0;
}
