/*
    bench.c - Demo program to benchmark open-source compression algorithm
    Copyright (C) Yann Collet 2012-2015

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
    - LZ4 source repository : https://github.com/Cyan4973/lz4
    - LZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
*/

/**************************************
*  Compiler Options
**************************************/
/* Disable some Visual warning messages */
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_DEPRECATE     /* VS2005 */

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


/**************************************
*  Includes
**************************************/
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
#include "lz4hc.h"
#include "lz4frame.h"

#include "xxhash.h"


/**************************************
*  Compiler Options
**************************************/
/* S_ISREG & gettimeofday() are not supported by MSVC */
#if !defined(S_ISREG)
#  define S_ISREG(x) (((x) & S_IFMT) == S_IFREG)
#endif

// GCC does not support _rotl outside of Windows
#if !defined(_WIN32)
#  define _rotl(x,r) ((x << r) | (x >> (32 - r)))
#endif


/**************************************
*  Basic Types
**************************************/
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


/**************************************
*  Constants
**************************************/
#define PROGRAM_DESCRIPTION "LZ4 speed analyzer"
#ifndef LZ4_VERSION
#  define LZ4_VERSION ""
#endif
#define AUTHOR "Yann Collet"
#define WELCOME_MESSAGE "*** %s %s %i-bits, by %s (%s) ***\n", PROGRAM_DESCRIPTION, LZ4_VERSION, (int)(sizeof(void*)*8), AUTHOR, __DATE__

#define NBLOOPS    6
#define TIMELOOP   2500

#define KB *(1 <<10)
#define MB *(1 <<20)
#define GB *(1U<<30)

#define KNUTH      2654435761U
#define MAX_MEM    (1984 MB)
#define DEFAULT_CHUNKSIZE   (4 MB)

#define ALL_COMPRESSORS 0
#define ALL_DECOMPRESSORS 0


/**************************************
*  Local structures
**************************************/
struct chunkParameters
{
    U32   id;
    char* origBuffer;
    char* compressedBuffer;
    int   origSize;
    int   compressedSize;
};


/**************************************
*  MACRO
**************************************/
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

void BMK_SetPause(void)
{
    BMK_pause = 1;
}

//*********************************************************
//  Private functions
//*********************************************************

#if defined(BMK_LEGACY_TIMER)

static int BMK_GetMilliStart(void)
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

static int BMK_GetMilliStart(void)
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
    size_t step = 64 MB;
    BYTE* testmem=NULL;

    requiredMem = (((requiredMem >> 26) + 1) << 26);
    requiredMem += 2*step;
    if (requiredMem > MAX_MEM) requiredMem = MAX_MEM;

    while (!testmem)
    {
        if (requiredMem > step) requiredMem -= step;
        else requiredMem >>= 1;
        testmem = (BYTE*) malloc ((size_t)requiredMem);
    }
    free (testmem);

    /* keep some space available */
    if (requiredMem > step) requiredMem -= step;
    else requiredMem >>= 1;

    return (size_t)requiredMem;
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
*  Benchmark function
*********************************************************/
#ifdef __SSSE3__

#include <tmmintrin.h>

/* Idea proposed by Terje Mathisen */
static BYTE stepSize16[17] = {16,16,16,15,16,15,12,14,16,9,10,11,12,13,14,15,16};
static __m128i replicateTable[17] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1},
    {0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0},
    {0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3},
    {0,1,2,3,4,0,1,2,3,4,0,1,2,3,4,0},
    {0,1,2,3,4,5,0,1,2,3,4,5,0,1,2,3},
    {0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1},
    {0,1,2,3,4,5,6,7,0,1,2,3,4,5,6,7},
    {0,1,2,3,4,5,6,7,8,0,1,2,3,4,5,6},
    {0,1,2,3,4,5,6,7,8,9,0,1,2,3,4,5},
    {0,1,2,3,4,5,6,7,8,9,10,0,1,2,3,4},
    {0,1,2,3,4,5,6,7,8,9,10,11,0,1,2,3},
    {0,1,2,3,4,5,6,7,8,9,10,11,12,0,1,2},
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,0,1},
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,0},
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}};
static BYTE stepSize32[17] = {32,32,32,30,32,30,30,28,32,27,30,22,24,26,28,30,16};
static __m128i replicateTable2[17] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1},
    {1,2,0,1,2,0,1,2,0,1,2,0,1,2,0,1},
    {0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3},
    {1,2,3,4,0,1,2,3,4,0,1,2,3,4,0,1},
    {4,5,0,1,2,3,4,5,0,1,2,3,4,5,0,1},
    {2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3},
    {0,1,2,3,4,5,6,7,0,1,2,3,4,5,6,7},
    {7,8,0,1,2,3,4,5,6,7,8,0,1,2,3,4},
    {6,7,8,9,0,1,2,3,4,5,6,7,8,9,0,1},
    {5,6,7,8,9,10,0,1,2,3,4,5,6,7,8,9},
    {4,5,6,7,8,9,10,11,0,1,2,3,4,5,6,7},
    {3,4,5,6,7,8,9,10,11,12,0,1,2,3,4,5},
    {2,3,4,5,6,7,8,9,10,11,12,13,0,1,2,3},
    {1,2,3,4,5,6,7,8,9,10,11,12,13,14,0,1},
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}};

U32 lz4_decode_sse(BYTE* dest, BYTE* src, U32 srcLength)
{
    BYTE* d = dest, *e = src+srcLength;
    unsigned token, lit_len, mat_len;
    __m128i a;
    BYTE* dstore, *msrc;

    if (!srcLength) return 0;
    goto start;

    do {
        U32 step;
        unsigned mat_offset = src[0] + (src[1] << 8);
        src += 2;
        msrc = d - mat_offset;
        if (mat_len == 15) {
            do {
                token = *src++;
                mat_len += token;
            } while (token == 255);
        }
        mat_len += 4;

        dstore = d;
        d += mat_len;

        if (mat_offset <= 16)
        { // Bulk store only!
            __m128i a2;
            a = _mm_loadu_si128((const __m128i *)msrc);
            a2 = _mm_shuffle_epi8(a, replicateTable2[mat_offset]);
            a = _mm_shuffle_epi8(a, replicateTable[mat_offset]);
            step = stepSize32[mat_offset];
            do {
                _mm_storeu_si128((__m128i *)dstore, a);
                _mm_storeu_si128((__m128i *)(dstore+16), a2);
                dstore += step;
            } while (dstore < d);
        }
        else
        {
            do
            {
                a = _mm_loadu_si128((const __m128i *)msrc);
                _mm_storeu_si128((__m128i *)dstore, a);
                msrc += sizeof(a);
                dstore += sizeof(a);
            } while (dstore < d);
        }
start:
        token = *src++;
        lit_len = token >> 4;
        mat_len = token & 15;
        if (token >= 0xf0) { // lit_len == 15
            do {
                token = *src++;
                lit_len += token;
            } while (token == 255);
        }
        dstore = d;
        msrc = src;
        d += lit_len;
        src += lit_len;
        do {
            a = _mm_loadu_si128((const __m128i *)msrc);
            _mm_storeu_si128((__m128i *)dstore, a);
            msrc += sizeof(a);
            dstore += sizeof(a);
        } while (dstore < d);
    } while (src < e);

    return (U32)(d-dest);
}
#endif // __SSSE3__


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

static LZ4_stream_t* ctx;
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
    return LZ4_compressHC_continue((LZ4_streamHC_t*)ctx, in, out, inSize);
}

static int local_LZ4_compressHC_limitedOutput_continue(const char* in, char* out, int inSize)
{
    return LZ4_compressHC_limitedOutput_continue((LZ4_streamHC_t*)ctx, in, out, inSize, LZ4_compressBound(inSize));
}

static int local_LZ4F_compressFrame(const char* in, char* out, int inSize)
{
    return (int)LZ4F_compressFrame(out, 2*inSize + 16, in, inSize, NULL);
}

static int local_LZ4_saveDict(const char* in, char* out, int inSize)
{
    (void)in;
    return LZ4_saveDict(&LZ4_dict, out, inSize);
}

LZ4_streamHC_t LZ4_dictHC;
static int local_LZ4_saveDictHC(const char* in, char* out, int inSize)
{
    (void)in;
    return LZ4_saveDictHC(&LZ4_dictHC, out, inSize);
}


static int local_LZ4_decompress_fast(const char* in, char* out, int inSize, int outSize)
{
    (void)inSize;
    //lz4_decode_sse((BYTE*)out, (BYTE*)in, inSize);
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
    LZ4_decompress_fast_usingDict(in, out, outSize, out - 65536, 65536);
    return outSize;
}

static int local_LZ4_decompress_safe_usingDict(const char* in, char* out, int inSize, int outSize)
{
    (void)inSize;
    LZ4_decompress_safe_usingDict(in, out, inSize, outSize, out - 65536, 65536);
    return outSize;
}

extern int LZ4_decompress_safe_forceExtDict(const char* in, char* out, int inSize, int outSize, const char* dict, int dictSize);

static int local_LZ4_decompress_safe_forceExtDict(const char* in, char* out, int inSize, int outSize)
{
    (void)inSize;
    LZ4_decompress_safe_forceExtDict(in, out, inSize, outSize, out - 65536, 65536);
    return outSize;
}

static int local_LZ4_decompress_safe_partial(const char* in, char* out, int inSize, int outSize)
{
    return LZ4_decompress_safe_partial(in, out, inSize, outSize - 5, outSize);
}

static LZ4F_decompressionContext_t g_dCtx;

static int local_LZ4F_decompress(const char* in, char* out, int inSize, int outSize)
{
    size_t srcSize = inSize;
    size_t dstSize = outSize;
    size_t result;
    result = LZ4F_decompress(g_dCtx, out, &dstSize, in, &srcSize, NULL);
    if (result!=0) { DISPLAY("Error decompressing frame : unfinished frame\n"); exit(8); }
    if (srcSize != (size_t)inSize) { DISPLAY("Error decompressing frame : read size incorrect\n"); exit(9); }
    return (int)dstSize;
}


int fullSpeedBench(char** fileNamesTable, int nbFiles)
{
  int fileIdx=0;
  char* orig_buff;
# define NB_COMPRESSION_ALGORITHMS 16
  double totalCTime[NB_COMPRESSION_ALGORITHMS+1] = {0};
  double totalCSize[NB_COMPRESSION_ALGORITHMS+1] = {0};
# define NB_DECOMPRESSION_ALGORITHMS 9
  double totalDTime[NB_DECOMPRESSION_ALGORITHMS+1] = {0};
  size_t errorCode;

  errorCode = LZ4F_createDecompressionContext(&g_dCtx, LZ4F_VERSION);
  if (LZ4F_isError(errorCode))
  {
     DISPLAY("dctx allocation issue \n");
     return 10;
  }

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
      stateLZ4   = LZ4_createStream();
      stateLZ4HC = LZ4_createStreamHC();

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
      if (inFileSize==0) { DISPLAY( "file is empty\n"); return 11; }
      benchedSize = (size_t) BMK_findMaxMem(inFileSize) / 2;
      if (benchedSize==0) { DISPLAY( "not enough memory\n"); return 11; }
      if ((U64)benchedSize > inFileSize) benchedSize = (size_t)inFileSize;
      if (benchedSize < inFileSize)
      {
          DISPLAY("Not enough memory for '%s' full size; testing %i MB only...\n", inFileName, (int)(benchedSize>>20));
      }

      // Alloc
      chunkP = (struct chunkParameters*) malloc(((benchedSize / (size_t)chunkSize)+1) * sizeof(struct chunkParameters));
      orig_buff = (char*) malloc((size_t)benchedSize);
      nbChunks = (int) (((int)benchedSize + (chunkSize-1))/ chunkSize);
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
            const char* compressorName;
            int (*compressionFunction)(const char*, char*, int);
            void* (*initFunction)(const char*) = NULL;
            double bestTime = 100000000.;

            // Init data chunks
            {
              int i;
              size_t remaining = benchedSize;
              char* in = orig_buff;
              char* out = compressed_buff;
                nbChunks = (int) (((int)benchedSize + (chunkSize-1))/ chunkSize);
              for (i=0; i<nbChunks; i++)
              {
                  chunkP[i].id = i;
                  chunkP[i].origBuffer = in; in += chunkSize;
                  if ((int)remaining > chunkSize) { chunkP[i].origSize = chunkSize; remaining -= chunkSize; } else { chunkP[i].origSize = (int)remaining; remaining = 0; }
                  chunkP[i].compressedBuffer = out; out += maxCompressedChunkSize;
                  chunkP[i].compressedSize = 0;
              }
            }

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
            case 14: compressionFunction = local_LZ4F_compressFrame; compressorName = "LZ4F_compressFrame";
                        chunkP[0].origSize = (int)benchedSize; nbChunks=1;
                        break;
            case 15: compressionFunction = local_LZ4_saveDict; compressorName = "LZ4_saveDict";
                        LZ4_loadDict(&LZ4_dict, chunkP[0].origBuffer, chunkP[0].origSize);
                        break;
            case 16: compressionFunction = local_LZ4_saveDictHC; compressorName = "LZ4_saveDictHC";
                        LZ4_loadDictHC(&LZ4_dictHC, chunkP[0].origBuffer, chunkP[0].origSize);
                        break;
            default : DISPLAY("ERROR ! Bad algorithm Id !! \n"); free(chunkP); return 1;
            }

            for (loopNb = 1; loopNb <= nbIterations; loopNb++)
            {
                double averageTime;
                int milliTime;

                PROGRESS("%1i- %-28.28s :%9i ->\r", loopNb, compressorName, (int)benchedSize);
                { size_t i; for (i=0; i<benchedSize; i++) compressed_buff[i]=(char)i; }     // warming up memory

                nb_loops = 0;
                milliTime = BMK_GetMilliStart();
                while(BMK_GetMilliStart() == milliTime);
                milliTime = BMK_GetMilliStart();
                while(BMK_GetMilliSpan(milliTime) < TIMELOOP)
                {
                    if (initFunction!=NULL) ctx = (LZ4_stream_t*)initFunction(chunkP[0].origBuffer);
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
                PROGRESS("%1i- %-28.28s :%9i ->%9i (%5.2f%%),%7.1f MB/s\r", loopNb, compressorName, (int)benchedSize, (int)cSize, ratio, (double)benchedSize / bestTime / 1000.);
            }

            if (ratio<100.)
                DISPLAY("%2i-%-28.28s :%9i ->%9i (%5.2f%%),%7.1f MB/s\n", cAlgNb, compressorName, (int)benchedSize, (int)cSize, ratio, (double)benchedSize / bestTime / 1000.);
            else
                DISPLAY("%2i-%-28.28s :%9i ->%9i (%5.1f%%),%7.1f MB/s\n", cAlgNb, compressorName, (int)benchedSize, (int)cSize, ratio, (double)benchedSize / bestTime / 1000.);

            totalCTime[cAlgNb] += bestTime;
            totalCSize[cAlgNb] += cSize;
        }

        // Prepare layout for decompression
        // Init data chunks
        {
          int i;
          size_t remaining = benchedSize;
          char* in = orig_buff;
          char* out = compressed_buff;
            nbChunks = (int) (((int)benchedSize + (chunkSize-1))/ chunkSize);
          for (i=0; i<nbChunks; i++)
          {
              chunkP[i].id = i;
              chunkP[i].origBuffer = in; in += chunkSize;
              if ((int)remaining > chunkSize) { chunkP[i].origSize = chunkSize; remaining -= chunkSize; } else { chunkP[i].origSize = (int)remaining; remaining = 0; }
              chunkP[i].compressedBuffer = out; out += maxCompressedChunkSize;
              chunkP[i].compressedSize = 0;
          }
        }
        for (chunkNb=0; chunkNb<nbChunks; chunkNb++)
        {
            chunkP[chunkNb].compressedSize = LZ4_compress(chunkP[chunkNb].origBuffer, chunkP[chunkNb].compressedBuffer, chunkP[chunkNb].origSize);
            if (chunkP[chunkNb].compressedSize==0) DISPLAY("ERROR ! %s() = 0 !! \n", "LZ4_compress"), exit(1);
        }

        // Decompression Algorithms
        for (dAlgNb=1; (dAlgNb <= NB_DECOMPRESSION_ALGORITHMS) && (decompressionTest); dAlgNb++)
        {
            //const char* dName = decompressionNames[dAlgNb];
            const char* dName;
            int (*decompressionFunction)(const char*, char*, int, int);
            double bestTime = 100000000.;

            if ((decompressionAlgo != ALL_DECOMPRESSORS) && (decompressionAlgo != dAlgNb)) continue;

            switch(dAlgNb)
            {
            case 1: decompressionFunction = local_LZ4_decompress_fast; dName = "LZ4_decompress_fast"; break;
            case 2: decompressionFunction = local_LZ4_decompress_fast_withPrefix64k; dName = "LZ4_decompress_fast_withPrefix64k"; break;
            case 3: decompressionFunction = local_LZ4_decompress_fast_usingDict; dName = "LZ4_decompress_fast_usingDict"; break;
            case 4: decompressionFunction = LZ4_decompress_safe; dName = "LZ4_decompress_safe"; break;
            case 5: decompressionFunction = LZ4_decompress_safe_withPrefix64k; dName = "LZ4_decompress_safe_withPrefix64k"; break;
            case 6: decompressionFunction = local_LZ4_decompress_safe_usingDict; dName = "LZ4_decompress_safe_usingDict"; break;
            case 7: decompressionFunction = local_LZ4_decompress_safe_partial; dName = "LZ4_decompress_safe_partial"; break;
            case 8: decompressionFunction = local_LZ4_decompress_safe_forceExtDict; dName = "LZ4_decompress_safe_forceExtDict"; break;
            case 9: decompressionFunction = local_LZ4F_decompress; dName = "LZ4F_decompress";
                    errorCode = LZ4F_compressFrame(compressed_buff, compressedBuffSize, orig_buff, benchedSize, NULL);
                    if (LZ4F_isError(errorCode)) { DISPLAY("Preparation error compressing frame\n"); return 1; }
                    chunkP[0].origSize = (int)benchedSize;
                    chunkP[0].compressedSize = (int)errorCode;
                    nbChunks = 1;
                    break;
            default : DISPLAY("ERROR ! Bad decompression algorithm Id !! \n"); free(chunkP); return 1;
            }

            { size_t i; for (i=0; i<benchedSize; i++) orig_buff[i]=0; }     // zeroing source area, for CRC checking

            for (loopNb = 1; loopNb <= nbIterations; loopNb++)
            {
                double averageTime;
                int milliTime;
                U32 crcDecoded;

                PROGRESS("%1i- %-29.29s :%10i ->\r", loopNb, dName, (int)benchedSize);

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

                PROGRESS("%1i- %-29.29s :%10i -> %7.1f MB/s\r", loopNb, dName, (int)benchedSize, (double)benchedSize / bestTime / 1000.);

                /* CRC Checking */
                crcDecoded = XXH32(orig_buff, (int)benchedSize, 0);
                if (crcOriginal!=crcDecoded) { DISPLAY("\n!!! WARNING !!! %14s : Invalid Checksum : %x != %x\n", inFileName, (unsigned)crcOriginal, (unsigned)crcDecoded); exit(1); }
            }

            DISPLAY("%2i-%-29.29s :%10i -> %7.1f MB/s\n", dAlgNb, dName, (int)benchedSize, (double)benchedSize / bestTime / 1000.);

            totalDTime[dAlgNb] += bestTime;
        }

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

int usage_advanced(void)
{
    DISPLAY( "\nAdvanced options :\n");
    DISPLAY( " -c#    : test only compression function # [1-%i]\n", NB_COMPRESSION_ALGORITHMS);
    DISPLAY( " -d#    : test only decompression function # [1-%i]\n", NB_DECOMPRESSION_ALGORITHMS);
    DISPLAY( " -i#    : iteration loops [1-9](default : %i)\n", NBLOOPS);
    DISPLAY( " -B#    : Block size [4-7](default : 7)\n");
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

                    // Unknown command
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

