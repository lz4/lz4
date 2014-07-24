/*
    datagen.c - compressible data generator test tool
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
//#include <stdlib.h>
#include <stdio.h>      // fgets, sscanf
#include <string.h>     // strcmp


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

#define KB *(1U<<10)
#define MB *(1U<<20)
#define GB *(1U<<30)

#define CDG_SIZE_DEFAULT (64 KB)
#define CDG_SEED_DEFAULT 0
#define CDG_COMPRESSIBILITY_DEFAULT 50
#define PRIME1   2654435761U
#define PRIME2   2246822519U


/**************************************
  Macros
**************************************/
#define DISPLAY(...)         fprintf(stderr, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...) if (displayLevel>=l) { DISPLAY(__VA_ARGS__); }


/**************************************
  Local Parameters
**************************************/
static int no_prompt = 0;
static char* programName;
static int displayLevel = 2;


/*********************************************************
  Fuzzer functions
*********************************************************/

#define CDG_rotl32(x,r) ((x << r) | (x >> (32 - r)))
static unsigned int CDG_rand(U32* src)
{
    U32 rand32 = *src;
    rand32 *= PRIME1;
    rand32 += PRIME2;
    rand32  = CDG_rotl32(rand32, 13);
    *src = rand32;
    return rand32;
}


#define CDG_RAND15BITS  ((CDG_rand(seed) >> 3) & 32767)
#define CDG_RANDLENGTH  ( ((CDG_rand(seed) >> 7) & 3) ? (CDG_rand(seed) % 14) : (CDG_rand(seed) & 511) + 15)
#define CDG_RANDCHAR    (((CDG_rand(seed) >> 9) & 63) + '0')
static void CDG_generate(U64 size, U32* seed, double proba)
{
    BYTE fullbuff[32 KB + 128 KB + 1];
    BYTE* buff = fullbuff + 32 KB;
    U64 total=0;
    U32 P32 = (U32)(32768 * proba);
    U32 pos=0;
    U32 genBlockSize = 128 KB;

    // Build initial prefix
    while (pos<32 KB)
    {
        // Select : Literal (char) or Match (within 32K)
        if (CDG_RAND15BITS < P32)
        {
            // Copy (within 64K)
            U32 d;
            int ref;
            int length = CDG_RANDLENGTH + 4;
            U32 offset = CDG_RAND15BITS + 1;
            if (offset > pos) offset = pos;
            ref = pos - offset;
            d = pos + length;
            while (pos < d) fullbuff[pos++] = fullbuff[ref++];
        }
        else
        {
            // Literal (noise)
            U32 d;
            int length = CDG_RANDLENGTH;
            d = pos + length;
            while (pos < d) fullbuff[pos++] = CDG_RANDCHAR;
        }
    }

    // Generate compressible data
    pos = 0;
    while (total < size)
    {
        if (size-total < 128 KB) genBlockSize = (U32)(size-total);
        total += genBlockSize;
        buff[genBlockSize] = 0;
        pos = 0;
        while (pos<genBlockSize)
        {
            // Select : Literal (char) or Match (within 32K)
            if (CDG_RAND15BITS < P32)
            {
                // Copy (within 64K)
                int ref;
                U32 d;
                int length = CDG_RANDLENGTH + 4;
                U32 offset = CDG_RAND15BITS + 1;
                if (pos + length > genBlockSize ) length = genBlockSize - pos;
                ref = pos - offset;
                d = pos + length;
                while (pos < d) buff[pos++] = buff[ref++];
            }
            else
            {
                // Literal (noise)
                U32 d;
                int length = CDG_RANDLENGTH;
                if (pos + length > genBlockSize) length = genBlockSize - pos;
                d = pos + length;
                while (pos < d) buff[pos++] = CDG_RANDCHAR;
            }
        }
        pos=0;
        for (;pos+512<=genBlockSize;pos+=512) printf("%512.512s", buff+pos);
        for (;pos<genBlockSize;pos++) printf("%c", buff[pos]);
        // Regenerate prefix
        memcpy(fullbuff, buff + 96 KB, 32 KB);
    }
}


int CDG_usage(void)
{
    DISPLAY( "Compressible data generator\n");
    DISPLAY( "Usage :\n");
    DISPLAY( "      %s [size] [args]\n", programName);
    DISPLAY( "\n");
    DISPLAY( "Arguments :\n");
    DISPLAY( " -g#    : generate # data (default:%i)\n", CDG_SIZE_DEFAULT);
    DISPLAY( " -s#    : Select seed (default:%i)\n", CDG_SEED_DEFAULT);
    DISPLAY( " -p#    : Select compressibility in %% (default:%i%%)\n", CDG_COMPRESSIBILITY_DEFAULT);
    DISPLAY( " -h     : display help and exit\n");
    return 0;
}


int main(int argc, char** argv)
{
    int argNb;
    int proba = CDG_COMPRESSIBILITY_DEFAULT;
    U64 size = CDG_SIZE_DEFAULT;
    U32 seed = CDG_SEED_DEFAULT;

    // Check command line
    programName = argv[0];
    for(argNb=1; argNb<argc; argNb++)
    {
        char* argument = argv[argNb];

        if(!argument) continue;   // Protection if argument empty

        // Decode command (note : aggregated commands are allowed)
        if (argument[0]=='-')
        {
            if (!strcmp(argument, "--no-prompt")) { no_prompt=1; continue; }

            while (argument[1]!=0)
            {
                argument++;
                switch(*argument)
                {
                case 'h':
                    return CDG_usage();
                case 'g':
                    argument++;
                    size=0;
                    while ((*argument>='0') && (*argument<='9'))
                    {
                        size *= 10;
                        size += *argument - '0';
                        argument++;
                    }
                    if (*argument=='K') { size <<= 10; argument++; }
                    if (*argument=='M') { size <<= 20; argument++; }
                    if (*argument=='G') { size <<= 30; argument++; }
                    if (*argument=='B') { argument++; }
                    break;
                case 's':
                    argument++;
                    seed=0;
                    while ((*argument>='0') && (*argument<='9'))
                    {
                        seed *= 10;
                        seed += *argument - '0';
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
                case 'v':
                    displayLevel = 4;
                    break;
                default: ;
                }
            }

        }
    }

    // Get Seed
    DISPLAYLEVEL(4, "Data Generator %s \n", LZ4_VERSION);
    DISPLAYLEVEL(3, "Seed = %u \n", seed);
    if (proba!=CDG_COMPRESSIBILITY_DEFAULT) DISPLAYLEVEL(3, "Compressibility : %i%%\n", proba);

    CDG_generate(size, &seed, ((double)proba) / 100);

    return 0;
}
