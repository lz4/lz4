/*
    bench.c - Demo program to benchmark open-source compression algorithm
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

//**************************************
// Compiler Options
//**************************************
// Disable some Visual warning messages
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_DEPRECATE     // VS2005

// Unix Large Files support (>4GB)
#if (defined(__sun__) && (!defined(__LP64__)))   // Sun Solaris 32-bits requires specific definitions
#  define _LARGEFILE_SOURCE
#  define _FILE_OFFSET_BITS 64
#elif ! defined(__LP64__)                        // No point defining Large file for 64 bit
#  define _LARGEFILE64_SOURCE
#endif

// S_ISREG & gettimeofday() are not supported by MSVC
#if defined(_MSC_VER) || defined(_WIN32)
#  define BMK_LEGACY_TIMER 1
#endif


//**************************************
// Includes
//**************************************
#include <stdlib.h>      // malloc
#include <stdio.h>       // fprintf, fopen, ftello64
#include <sys/types.h>   // stat64
#include <sys/stat.h>    // stat64
#include <string.h>      // strcmp

// Use ftime() if gettimeofday() is not available on your target
#if defined(BMK_LEGACY_TIMER)
#  include <sys/timeb.h>   // timeb, ftime
#else
#  include <sys/time.h>    // gettimeofday
#endif

#include "lz4.h"
#define COMPRESSOR0 LZ4_compress
#include "lz4hc.h"
#define COMPRESSOR1 LZ4_compressHC
#define DEFAULTCOMPRESSOR COMPRESSOR0

#include "xxhash.h"


//**************************************
// Compiler Options
//**************************************
// S_ISREG & gettimeofday() are not supported by MSVC
#if !defined(S_ISREG)
#  define S_ISREG(x) (((x) & S_IFMT) == S_IFREG)
#endif

// GCC does not support _rotl outside of Windows
#if !defined(_WIN32)
#  define _rotl(x,r) ((x << r) | (x >> (32 - r)))
#endif


//**************************************
// Basic Types
//**************************************
#if defined (__STDC_VERSION__) && __STDC_VERSION__ >= 199901L   // C99
# include <stdint.h>
  typedef uint8_t  BYTE;
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


//****************************
// Constants
//****************************
#define PROGRAM_DESCRIPTION "LZ4 speed analyzer"
#ifndef LZ4_VERSION
#  define LZ4_VERSION ""
#endif
#define AUTHOR "Yann Collet"
#define WELCOME_MESSAGE "*** %s %s %i-bits, by %s (%s) ***\n", PROGRAM_DESCRIPTION, LZ4_VERSION, (int)(sizeof(void*)*8), AUTHOR, __DATE__

#define NBLOOPS    6
#define TIMELOOP   2500

#define KNUTH      2654435761U
#define MAX_MEM    (1984<<20)
#define DEFAULT_CHUNKSIZE   (4<<20)

#define ALL_COMPRESSORS 0
#define ALL_DECOMPRESSORS 0


//**************************************
// Local structures
//**************************************
struct chunkParameters
{
    U32   id;
    char* origBuffer;
    char* compressedBuffer;
    int   origSize;
    int   compressedSize;
};


//**************************************
// MACRO
//**************************************
#define DISPLAY(...) fprintf(stderr, __VA_ARGS__)
#define PROGRESS(...) no_prompt ? 0 : DISPLAY(__VA_ARGS__)



//**************************************
// Benchmark Parameters
//**************************************
static int chunkSize = DEFAULT_CHUNKSIZE;
static int nbIterations = NBLOOPS;
static int BMK_pause = 0;
static int compressionTest = 1;
static int decompressionTest = 1;
static int compressionAlgo = ALL_COMPRESSORS;
static int decompressionAlgo = ALL_DECOMPRESSORS;
static int no_prompt = 0;

void BMK_SetBlocksize(int bsize)
{
    chunkSize = bsize;
    DISPLAY("-Using Block Size of %i KB-\n", chunkSize>>10);
}

void BMK_SetNbIterations(int nbLoops)
{
    nbIterations = nbLoops;
    DISPLAY("- %i iterations -\n", nbIterations);
}

void BMK_SetPause()
{
    BMK_pause = 1;
}

//*********************************************************
//  Private functions
//*********************************************************

#if defined(BMK_LEGACY_TIMER)

static int BMK_GetMilliStart()
{
  // Based on Legacy ftime()
  // Rolls over every ~ 12.1 days (0x100000/24/60/60)
  // Use GetMilliSpan to correct for rollover
  struct timeb tb;
  int nCount;
  ftime( &tb );
  nCount = (int) (tb.millitm + (tb.time & 0xfffff) * 1000);
  return nCount;
}

#else

static int BMK_GetMilliStart()
{
  // Based on newer gettimeofday()
  // Use GetMilliSpan to correct for rollover
  struct timeval tv;
  int nCount;
  gettimeofday(&tv, NULL);
  nCount = (int) (tv.tv_usec/1000 + (tv.tv_sec & 0xfffff) * 1000);
  return nCount;
}

#endif


static int BMK_GetMilliSpan( int nTimeStart )
{
  int nSpan = BMK_GetMilliStart() - nTimeStart;
  if ( nSpan < 0 )
    nSpan += 0x100000 * 1000;
  return nSpan;
}


static size_t BMK_findMaxMem(U64 requiredMem)
{
    size_t step = (64U<<20);   // 64 MB
    BYTE* testmem=NULL;

    requiredMem = (((requiredMem >> 25) + 1) << 26);
    if (requiredMem > MAX_MEM) requiredMem = MAX_MEM;

    requiredMem += 2*step;
    while (!testmem)
    {
        requiredMem -= step;
        testmem = (BYTE*) malloc ((size_t)requiredMem);
    }

    free (testmem);
    return (size_t) (requiredMem - step);
}


static U64 BMK_GetFileSize(char* infilename)
{
    int r;
#if defined(_MSC_VER)
    struct _stat64 statbuf;
    r = _stat64(infilename, &statbuf);
#else
    struct stat statbuf;
    r = stat(infilename, &statbuf);
#endif
    if (r || !S_ISREG(statbuf.st_mode)) return 0;   // No good...
    return (U64)statbuf.st_size;
}


/*********************************************************
  Benchmark function
*********************************************************/

static int local_LZ4_compress_limitedOutput(const char* in, char* out, int inSize)
{
    return LZ4_compress_limitedOutput(in, out, inSize, LZ4_compressBound(inSize));
}

static void* stateLZ4;
static int local_LZ4_compress_withState(const char* in, char* out, int inSize)
{
    return LZ4_compress_withState(stateLZ4, in, out, inSize);
}

static int local_LZ4_compress_limitedOutput_withState(const char* in, char* out, int inSize)
{
    return LZ4_compress_limitedOutput_withState(stateLZ4, in, out, inSize, LZ4_compressBound(inSize));
}

static void* ctx;
static int local_LZ4_compress_continue(const char* in, char* out, int inSize)
{
    return LZ4_compress_continue(ctx, in, out, inSize);
}

static int local_LZ4_compress_limitedOutput_continue(const char* in, char* out, int inSize)
{
    return LZ4_compress_limitedOutput_continue(ctx, in, out, inSize, LZ4_compressBound(inSize));
}


LZ4_stream_t LZ4_dict;
static void* local_LZ4_resetDictT(const char* fake)
{
    (void)fake;
    memset(&LZ4_dict, 0, sizeof(LZ4_stream_t));
    return NULL;
}

int LZ4_compress_forceExtDict (LZ4_stream_t* LZ4_dict, const char* source, char* dest, int inputSize);
static int local_LZ4_compress_forceDict(const char* in, char* out, int inSize)
{
    return LZ4_compress_forceExtDict(&LZ4_dict, in, out, inSize);
}


static void* stateLZ4HC;
static int local_LZ4_compressHC_withStateHC(const char* in, char* out, int inSize)
{
    return LZ4_compressHC_withStateHC(stateLZ4HC, in, out, inSize);
}

static int local_LZ4_compressHC_limitedOutput_withStateHC(const char* in, char* out, int inSize)
{
    return LZ4_compressHC_limitedOutput_withStateHC(stateLZ4HC, in, out, inSize, LZ4_compressBound(inSize));
}

static int local_LZ4_compressHC_limitedOutput(const char* in, char* out, int inSize)
{
    return LZ4_compressHC_limitedOutput(in, out, inSize, LZ4_compressBound(inSize));
}

static int local_LZ4_compressHC_continue(const char* in, char* out, int inSize)
{
    return LZ4_compressHC_continue(ctx, in, out, inSize);
}

static int local_LZ4_compressHC_limitedOutput_continue(const char* in, char* out, int inSize)
{
    return LZ4_compressHC_limitedOutput_continue(ctx, in, out, inSize, LZ4_compressBound(inSize));
}

static int local_LZ4_decompress_fast(const char* in, char* out, int inSize, int outSize)
{
    (void)inSize;
    LZ4_decompress_fast(in, out, outSize);
    return outSize;
}

static int local_LZ4_decompress_fast_withPrefix64k(const char* in, char* out, int inSize, int outSize)
{
    (void)inSize;
    LZ4_decompress_fast_withPrefix64k(in, out, outSize);
    return outSize;
}

static int local_LZ4_decompress_fast_usingDict(const char* in, char* out, int inSize, int outSize)
{
    (void)inSize;
    LZ4_decompress_fast_usingDict(in, out, outSize, in - 65536, 65536);
    return outSize;
}

static int local_LZ4_decompress_safe_usingDict(const char* in, char* out, int inSize, int outSize)
{
    (void)inSize;
    LZ4_decompress_safe_usingDict(in, out, inSize, outSize, in - 65536, 65536);
    return outSize;
}

static int local_LZ4_decompress_safe_partial(const char* in, char* out, int inSize, int outSize)
{
    return LZ4_decompress_safe_partial(in, out, inSize, outSize - 5, outSize);
}

int fullSpeedBench(char** fileNamesTable, int nbFiles)
{
  int fileIdx=0;
  char* orig_buff;
# define NB_COMPRESSION_ALGORITHMS 13
# define MINCOMPRESSIONCHAR '0'
  double totalCTime[NB_COMPRESSION_ALGORITHMS+1] = {0};
  double totalCSize[NB_COMPRESSION_ALGORITHMS+1] = {0};
# define NB_DECOMPRESSION_ALGORITHMS 7
# define MINDECOMPRESSIONCHAR '0'
# define MAXDECOMPRESSIONCHAR (MINDECOMPRESSIONCHAR + NB_DECOMPRESSION_ALGORITHMS)
  static char* decompressionNames[] = { "LZ4_decompress_fast", "LZ4_decompress_fast_withPrefix64k", "LZ4_decompress_fast_usingDict",
                                        "LZ4_decompress_safe", "LZ4_decompress_safe_withPrefix64k", "LZ4_decompress_safe_usingDict", "LZ4_decompress_safe_partial" };
  double totalDTime[NB_DECOMPRESSION_ALGORITHMS+1] = {0};

  U64 totals = 0;


  // Loop for each file
  while (fileIdx<nbFiles)
  {
      FILE* inFile;
      char* inFileName;
      U64   inFileSize;
      size_t benchedSize;
      int nbChunks;
      int maxCompressedChunkSize;
      struct chunkParameters* chunkP;
      size_t readSize;
      char* compressed_buff; int compressedBuffSize;
      U32 crcOriginal;


      // Init
      stateLZ4   = malloc(LZ4_sizeofState());
      stateLZ4HC = malloc(LZ4_sizeofStateHC());

      // Check file existence
      inFileName = fileNamesTable[fileIdx++];
      inFile = fopen( inFileName, "rb" );
      if (inFile==NULL)
      {
        DISPLAY( "Pb opening %s\n", inFileName);
        return 11;
      }

      // Memory allocation & restrictions
      inFileSize = BMK_GetFileSize(inFileName);
      benchedSize = (size_t) BMK_findMaxMem(inFileSize) / 2;
      if ((U64)benchedSize > inFileSize) benchedSize = (size_t)inFileSize;
      if (benchedSize < inFileSize)
      {
          DISPLAY("Not enough memory for '%s' full size; testing %i MB only...\n", inFileName, (int)(benchedSize>>20));
      }

      // Alloc
      chunkP = (struct chunkParameters*) malloc(((benchedSize / (size_t)chunkSize)+1) * sizeof(struct chunkParameters));
      orig_buff = (char*) malloc((size_t)benchedSize);
      nbChunks = (int) ((int)benchedSize / chunkSize) + 1;
      maxCompressedChunkSize = LZ4_compressBound(chunkSize);
      compressedBuffSize = nbChunks * maxCompressedChunkSize;
      compressed_buff = (char*)malloc((size_t)compressedBuffSize);


      if(!orig_buff || !compressed_buff)
      {
        DISPLAY("\nError: not enough memory!\n");
        free(orig_buff);
        free(compressed_buff);
        free(chunkP);
        fclose(inFile);
        return 12;
      }

      // Init chunks data
      {
          int i;
          size_t remaining = benchedSize;
          char* in = orig_buff;
          char* out = compressed_buff;
          for (i=0; i<nbChunks; i++)
          {
              chunkP[i].id = i;
              chunkP[i].origBuffer = in; in += chunkSize;
              if ((int)remaining > chunkSize) { chunkP[i].origSize = chunkSize; remaining -= chunkSize; } else { chunkP[i].origSize = (int)remaining; remaining = 0; }
              chunkP[i].compressedBuffer = out; out += maxCompressedChunkSize;
              chunkP[i].compressedSize = 0;
          }
      }

      // Fill input buffer
      DISPLAY("Loading %s...       \r", inFileName);
      readSize = fread(orig_buff, 1, benchedSize, inFile);
      fclose(inFile);

      if(readSize != benchedSize)
      {
        DISPLAY("\nError: problem reading file '%s' !!    \n", inFileName);
        free(orig_buff);
        free(compressed_buff);
        free(chunkP);
        return 13;
      }

      // Calculating input Checksum
      crcOriginal = XXH32(orig_buff, (unsigned int)benchedSize,0);


      // Bench
      {
        int loopNb, nb_loops, chunkNb, cAlgNb, dAlgNb;
        size_t cSize=0;
        double ratio=0.;

        DISPLAY("\r%79s\r", "");
        DISPLAY(" %s : \n", inFileName);

        // Compression Algorithms
        for (cAlgNb=1; (cAlgNb <= NB_COMPRESSION_ALGORITHMS) && (compressionTest); cAlgNb++)
        {
            char* compressorName;
            int (*compressionFunction)(const char*, char*, int);
            void* (*initFunction)(const char*) = NULL;
            double bestTime = 100000000.;

            if ((compressionAlgo != ALL_COMPRESSORS) && (compressionAlgo != cAlgNb)) continue;

            switch(cAlgNb)
            {
            case 1 : compressionFunction = LZ4_compress; compressorName = "LZ4_compress"; break;
            case 2 : compressionFunction = local_LZ4_compress_limitedOutput; compressorName = "LZ4_compress_limitedOutput"; break;
            case 3 : compressionFunction = local_LZ4_compress_withState; compressorName = "LZ4_compress_withState"; break;
            case 4 : compressionFunction = local_LZ4_compress_limitedOutput_withState; compressorName = "LZ4_compress_limitedOutput_withState"; break;
            case 5 : compressionFunction = local_LZ4_compress_continue; initFunction = LZ4_create; compressorName = "LZ4_compress_continue"; break;
            case 6 : compressionFunction = local_LZ4_compress_limitedOutput_continue; initFunction = LZ4_create; compressorName = "LZ4_compress_limitedOutput_continue"; break;
            case 7 : compressionFunction = LZ4_compressHC; compressorName = "LZ4_compressHC"; break;
            case 8 : compressionFunction = local_LZ4_compressHC_limitedOutput; compressorName = "LZ4_compressHC_limitedOutput"; break;
            case 9 : compressionFunction = local_LZ4_compressHC_withStateHC; compressorName = "LZ4_compressHC_withStateHC"; break;
            case 10: compressionFunction = local_LZ4_compressHC_limitedOutput_withStateHC; compressorName = "LZ4_compressHC_limitedOutput_withStateHC"; break;
            case 11: compressionFunction = local_LZ4_compressHC_continue; initFunction = LZ4_createHC; compressorName = "LZ4_compressHC_continue"; break;
            case 12: compressionFunction = local_LZ4_compressHC_limitedOutput_continue; initFunction = LZ4_createHC; compressorName = "LZ4_compressHC_limitedOutput_continue"; break;
            case 13: compressionFunction = local_LZ4_compress_forceDict; initFunction = local_LZ4_resetDictT; compressorName = "LZ4_compress_forceDict"; break;
            default : DISPLAY("ERROR ! Bad algorithm Id !! \n"); free(chunkP); return 1;
            }

            for (loopNb = 1; loopNb <= nbIterations; loopNb++)
            {
                double averageTime;
                int milliTime;

                PROGRESS("%1i-%-25.25s : %9i ->\r", loopNb, compressorName, (int)benchedSize);
                { size_t i; for (i=0; i<benchedSize; i++) compressed_buff[i]=(char)i; }     // warmimg up memory

                nb_loops = 0;
                milliTime = BMK_GetMilliStart();
                while(BMK_GetMilliStart() == milliTime);
                milliTime = BMK_GetMilliStart();
                while(BMK_GetMilliSpan(milliTime) < TIMELOOP)
                {
                    if (initFunction!=NULL) ctx = initFunction(chunkP[0].origBuffer);
                    for (chunkNb=0; chunkNb<nbChunks; chunkNb++)
                    {
                        chunkP[chunkNb].compressedSize = compressionFunction(chunkP[chunkNb].origBuffer, chunkP[chunkNb].compressedBuffer, chunkP[chunkNb].origSize);
                        if (chunkP[chunkNb].compressedSize==0) DISPLAY("ERROR ! %s() = 0 !! \n", compressorName), exit(1);
                    }
                    if (initFunction!=NULL) free(ctx);
                    nb_loops++;
                }
                milliTime = BMK_GetMilliSpan(milliTime);

                averageTime = (double)milliTime / nb_loops;
                if (averageTime < bestTime) bestTime = averageTime;
                cSize=0; for (chunkNb=0; chunkNb<nbChunks; chunkNb++) cSize += chunkP[chunkNb].compressedSize;
                ratio = (double)cSize/(double)benchedSize*100.;
                PROGRESS("%1i-%-25.25s : %9i -> %9i (%5.2f%%),%7.1f MB/s\r", loopNb, compressorName, (int)benchedSize, (int)cSize, ratio, (double)benchedSize / bestTime / 1000.);
            }

            if (ratio<100.)
                DISPLAY("%-27.27s : %9i -> %9i (%5.2f%%),%7.1f MB/s\n", compressorName, (int)benchedSize, (int)cSize, ratio, (double)benchedSize / bestTime / 1000.);
            else
                DISPLAY("%-27.27s : %9i -> %9i (%5.1f%%),%7.1f MB/s\n", compressorName, (int)benchedSize, (int)cSize, ratio, (double)benchedSize / bestTime / 1000.);

            totalCTime[cAlgNb] += bestTime;
            totalCSize[cAlgNb] += cSize;
        }

        // Prepare layout for decompression
        for (chunkNb=0; chunkNb<nbChunks; chunkNb++)
        {
            chunkP[chunkNb].compressedSize = LZ4_compress(chunkP[chunkNb].origBuffer, chunkP[chunkNb].compressedBuffer, chunkP[chunkNb].origSize);
            if (chunkP[chunkNb].compressedSize==0) DISPLAY("ERROR ! %s() = 0 !! \n", "LZ4_compress"), exit(1);
        }
        { size_t i; for (i=0; i<benchedSize; i++) orig_buff[i]=0; }     // zeroing source area, for CRC checking

        // Decompression Algorithms
        for (dAlgNb=0; (dAlgNb < NB_DECOMPRESSION_ALGORITHMS) && (decompressionTest); dAlgNb++)
        {
            char* dName = decompressionNames[dAlgNb];
            int (*decompressionFunction)(const char*, char*, int, int);
            double bestTime = 100000000.;

            if ((decompressionAlgo != ALL_DECOMPRESSORS) && (decompressionAlgo != dAlgNb+1)) continue;

            switch(dAlgNb)
            {
            case 0: decompressionFunction = local_LZ4_decompress_fast; break;
            case 1: decompressionFunction = local_LZ4_decompress_fast_withPrefix64k; break;
            case 2: decompressionFunction = local_LZ4_decompress_fast_usingDict; break;
            case 3: decompressionFunction = LZ4_decompress_safe; break;
            case 4: decompressionFunction = LZ4_decompress_safe_withPrefix64k; break;
            case 5: decompressionFunction = local_LZ4_decompress_safe_usingDict; break;
            case 6: decompressionFunction = local_LZ4_decompress_safe_partial; break;
            default : DISPLAY("ERROR ! Bad decompression algorithm Id !! \n"); free(chunkP); return 1;
            }

            for (loopNb = 1; loopNb <= nbIterations; loopNb++)
            {
                double averageTime;
                int milliTime;
                U32 crcDecoded;

                PROGRESS("%1i-%-29.29s :%10i ->\r", loopNb, dName, (int)benchedSize);

                nb_loops = 0;
                milliTime = BMK_GetMilliStart();
                while(BMK_GetMilliStart() == milliTime);
                milliTime = BMK_GetMilliStart();
                while(BMK_GetMilliSpan(milliTime) < TIMELOOP)
                {
                    for (chunkNb=0; chunkNb<nbChunks; chunkNb++)
                    {
                        int decodedSize = decompressionFunction(chunkP[chunkNb].compressedBuffer, chunkP[chunkNb].origBuffer, chunkP[chunkNb].compressedSize, chunkP[chunkNb].origSize);
                        if (chunkP[chunkNb].origSize != decodedSize) DISPLAY("ERROR ! %s() == %i != %i !! \n", dName, decodedSize, chunkP[chunkNb].origSize), exit(1);
                    }
                    nb_loops++;
                }
                milliTime = BMK_GetMilliSpan(milliTime);

                averageTime = (double)milliTime / nb_loops;
                if (averageTime < bestTime) bestTime = averageTime;

                PROGRESS("%1i-%-29.29s :%10i -> %7.1f MB/s\r", loopNb, dName, (int)benchedSize, (double)benchedSize / bestTime / 1000.);

                // CRC Checking
                crcDecoded = XXH32(orig_buff, (int)benchedSize, 0);
                if (crcOriginal!=crcDecoded) { DISPLAY("\n!!! WARNING !!! %14s : Invalid Checksum : %x != %x\n", inFileName, (unsigned)crcOriginal, (unsigned)crcDecoded); exit(1); }
            }

            DISPLAY("%-31.31s :%10i -> %7.1f MB/s\n", dName, (int)benchedSize, (double)benchedSize / bestTime / 1000.);

            totalDTime[dAlgNb] += bestTime;
        }

        totals += benchedSize;
      }

      free(orig_buff);
      free(compressed_buff);
      free(chunkP);
  }

  if (BMK_pause) { printf("press enter...\n"); getchar(); }

  return 0;
}


int usage(char* exename)
{
    DISPLAY( "Usage :\n");
    DISPLAY( "      %s [arg] file1 file2 ... fileX\n", exename);
    DISPLAY( "Arguments :\n");
    DISPLAY( " -c     : compression tests only\n");
    DISPLAY( " -d     : decompression tests only\n");
    DISPLAY( " -H/-h  : Help (this text + advanced options)\n");
    return 0;
}

int usage_advanced()
{
    DISPLAY( "\nAdvanced options :\n");
    DISPLAY( " -c#    : test only compression function # [1-%i]\n", NB_COMPRESSION_ALGORITHMS);
    DISPLAY( " -d#    : test only decompression function # [1-%i]\n", NB_DECOMPRESSION_ALGORITHMS);
    DISPLAY( " -i#    : iteration loops [1-9](default : %i)\n", NBLOOPS);
    DISPLAY( " -B#    : Block size [4-7](default : 7)\n");
    //DISPLAY( " -BD    : Block dependency (improve compression ratio)\n");
    return 0;
}

int badusage(char* exename)
{
    DISPLAY("Wrong parameters\n");
    usage(exename);
    return 0;
}

int main(int argc, char** argv)
{
    int i,
        filenamesStart=2;
    char* exename=argv[0];
    char* input_filename=0;

    // Welcome message
    DISPLAY(WELCOME_MESSAGE);

    if (argc<2) { badusage(exename); return 1; }

    for(i=1; i<argc; i++)
    {
        char* argument = argv[i];

        if(!argument) continue;   // Protection if argument empty
        if (!strcmp(argument, "--no-prompt"))
        {
            no_prompt = 1;
            continue;
        }

        // Decode command (note : aggregated commands are allowed)
        if (argument[0]=='-')
        {
            while (argument[1]!=0)
            {
                argument ++;

                switch(argument[0])
                {
                    // Select compression algorithm only
                case 'c':
                    decompressionTest = 0;
                    while ((argument[1]>= '0') && (argument[1]<= '9'))
                    {
                        compressionAlgo *= 10;
                        compressionAlgo += argument[1] - '0';
                        argument++;
                    }
                    break;

                    // Select decompression algorithm only
                case 'd':
                    compressionTest = 0;
                    while ((argument[1]>= '0') && (argument[1]<= '9'))
                    {
                        decompressionAlgo *= 10;
                        decompressionAlgo += argument[1] - '0';
                        argument++;
                    }
                    break;

                    // Display help on usage
                case 'h' :
                case 'H': usage(exename); usage_advanced(); return 0;

                    // Modify Block Properties
                case 'B':
                    while (argument[1]!=0)
                    switch(argument[1])
                    {
                    case '4':
                    case '5':
                    case '6':
                    case '7':
                    {
                        int B = argument[1] - '0';
                        int S = 1 << (8 + 2*B);
                        BMK_SetBlocksize(S);
                        argument++;
                        break;
                    }
                    case 'D': argument++; break;
                    default : goto _exit_blockProperties;
                    }
_exit_blockProperties:
                    break;

                    // Modify Nb Iterations
                case 'i':
                    if ((argument[1] >='1') && (argument[1] <='9'))
                    {
                        int iters = argument[1] - '0';
                        BMK_SetNbIterations(iters);
                        argument++;
                    }
                    break;

                    // Pause at the end (hidden option)
                case 'p': BMK_SetPause(); break;

                    // Unrecognised command
                default : badusage(exename); return 1;
                }
            }
            continue;
        }

        // first provided filename is input
        if (!input_filename) { input_filename=argument; filenamesStart=i; continue; }

    }

    // No input filename ==> Error
    if(!input_filename) { badusage(exename); return 1; }

    return fullSpeedBench(argv+filenamesStart, argc-filenamesStart);

}

