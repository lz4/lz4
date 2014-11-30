/*
  LZ4io.c - LZ4 File/Stream Interface
  Copyright (C) Yann Collet 2011-2014
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
  - LZ4 source repository : http://code.google.com/p/lz4/
  - LZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
*/
/*
  Note : this is stand-alone program.
  It is not part of LZ4 compression library, it is a user code of the LZ4 library.
  - The license of LZ4 library is BSD.
  - The license of xxHash library is BSD.
  - The license of this source file is GPLv2.
*/

//**************************************
// Compiler Options
//**************************************
#ifdef _MSC_VER    /* Visual Studio */
#  define _CRT_SECURE_NO_WARNINGS
#  define _CRT_SECURE_NO_DEPRECATE     // VS2005
#  pragma warning(disable : 4127)      // disable: C4127: conditional expression is constant
#endif

#define _LARGE_FILES           // Large file support on 32-bits AIX
#define _FILE_OFFSET_BITS 64   // Large file support on 32-bits unix
#define _POSIX_SOURCE 1        // for fileno() within <stdio.h> on unix


//****************************
// Includes
//****************************
#include <stdio.h>    // fprintf, fopen, fread, _fileno, stdin, stdout
#include <stdlib.h>   // malloc
#include <string.h>   // strcmp, strlen
#include <time.h>     // clock
#include "lz4io.h"
#include "lz4.h"
#include "lz4hc.h"
#include "xxhash.h"


//****************************
// OS-specific Includes
//****************************
#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(_WIN32) || defined(__CYGWIN__)
#  include <fcntl.h>    // _O_BINARY
#  include <io.h>       // _setmode, _isatty
#  ifdef __MINGW32__
   int _fileno(FILE *stream);   // MINGW somehow forgets to include this windows declaration into <stdio.h>
#  endif
#  define SET_BINARY_MODE(file) _setmode(_fileno(file), _O_BINARY)
#  define IS_CONSOLE(stdStream) _isatty(_fileno(stdStream))
#else
#  include <unistd.h>   // isatty
#  define SET_BINARY_MODE(file)
#  define IS_CONSOLE(stdStream) isatty(fileno(stdStream))
#endif


//**************************************
// Compiler-specific functions
//**************************************
#define GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)

#if defined(_MSC_VER)    // Visual Studio
#  define swap32 _byteswap_ulong
#elif GCC_VERSION >= 403
#  define swap32 __builtin_bswap32
#else
  static unsigned int swap32(unsigned int x)
  {
    return ((x << 24) & 0xff000000 ) |
           ((x <<  8) & 0x00ff0000 ) |
           ((x >>  8) & 0x0000ff00 ) |
           ((x >> 24) & 0x000000ff );
  }
#endif


//****************************
// Constants
//****************************
#define KB *(1U<<10)
#define MB *(1U<<20)
#define GB *(1U<<30)

#define _1BIT  0x01
#define _2BITS 0x03
#define _3BITS 0x07
#define _4BITS 0x0F
#define _8BITS 0xFF

#define MAGICNUMBER_SIZE   4
#define LZ4S_MAGICNUMBER   0x184D2204
#define LZ4S_SKIPPABLE0    0x184D2A50
#define LZ4S_SKIPPABLEMASK 0xFFFFFFF0
#define LEGACY_MAGICNUMBER 0x184C2102

#define CACHELINE 64
#define LEGACY_BLOCKSIZE   (8 MB)
#define MIN_STREAM_BUFSIZE (192 KB)
#define LZ4S_BLOCKSIZEID_DEFAULT 7
#define LZ4S_CHECKSUM_SEED 0
#define LZ4S_EOS 0
#define LZ4S_MAXHEADERSIZE (MAGICNUMBER_SIZE+2+8+4+1)


//**************************************
// Architecture Macros
//**************************************
static const int one = 1;
#define CPU_LITTLE_ENDIAN   (*(char*)(&one))
#define CPU_BIG_ENDIAN      (!CPU_LITTLE_ENDIAN)
#define LITTLE_ENDIAN_32(i) (CPU_LITTLE_ENDIAN?(i):swap32(i))


//**************************************
// Macros
//**************************************
#define DISPLAY(...)         fprintf(stderr, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...) if (displayLevel>=l) { DISPLAY(__VA_ARGS__); }


//**************************************
// Local Parameters
//**************************************
static int displayLevel = 0;   // 0 : no display  // 1: errors  // 2 : + result + interaction + warnings ;  // 3 : + progression;  // 4 : + information
static int overwrite = 1;
static int globalBlockSizeId = LZ4S_BLOCKSIZEID_DEFAULT;
static int blockChecksum = 0;
static int streamChecksum = 1;
static int blockIndependence = 1;

static const int minBlockSizeID = 4;
static const int maxBlockSizeID = 7;

//**************************************
// Exceptions
//**************************************
#define DEBUG 0
#define DEBUGOUTPUT(...) if (DEBUG) DISPLAY(__VA_ARGS__);
#define EXM_THROW(error, ...)                                             \
{                                                                         \
    DEBUGOUTPUT("Error defined at %s, line %i : \n", __FILE__, __LINE__); \
    DISPLAYLEVEL(1, "Error %i : ", error);                                \
    DISPLAYLEVEL(1, __VA_ARGS__);                                         \
    DISPLAYLEVEL(1, "\n");                                                \
    exit(error);                                                          \
}


//**************************************
// Version modifiers
//**************************************
#define EXTENDED_ARGUMENTS
#define EXTENDED_HELP
#define EXTENDED_FORMAT
#define DEFAULT_COMPRESSOR   compress_file
#define DEFAULT_DECOMPRESSOR decodeLZ4S


/* ************************************************** */
/* ****************** Parameters ******************** */
/* ************************************************** */

/* Default setting : overwrite = 1; return : overwrite mode (0/1) */
int LZ4IO_setOverwrite(int yes)
{
   overwrite = (yes!=0);
   return overwrite;
}

/* blockSizeID : valid values : 4-5-6-7 */
int LZ4IO_setBlockSizeID(int bsid)
{
    static const int blockSizeTable[] = { 64 KB, 256 KB, 1 MB, 4 MB };
    if ((bsid < minBlockSizeID) || (bsid > maxBlockSizeID)) return -1;
    globalBlockSizeId = bsid;
    return blockSizeTable[globalBlockSizeId-minBlockSizeID];
}

int LZ4IO_setBlockMode(blockMode_t blockMode)
{
    blockIndependence = (blockMode == independentBlocks);
    return blockIndependence;
}

/* Default setting : no checksum */
int LZ4IO_setBlockChecksumMode(int xxhash)
{
    blockChecksum = (xxhash != 0);
    return blockChecksum;
}

/* Default setting : checksum enabled */
int LZ4IO_setStreamChecksumMode(int xxhash)
{
    streamChecksum = (xxhash != 0);
    return streamChecksum;
}

/* Default setting : 0 (no notification) */
int LZ4IO_setNotificationLevel(int level)
{
    displayLevel = level;
    return displayLevel;
}



/* ************************************************************************ */
/* ********************** LZ4 File / Pipe compression ********************* */
/* ************************************************************************ */

static int          LZ4S_GetBlockSize_FromBlockId (int id) { return (1 << (8 + (2 * id))); }
static unsigned int LZ4S_GetCheckBits_FromXXH (unsigned int xxh) { return (xxh >> 8) & _8BITS; }
static int          LZ4S_isSkippableMagicNumber(unsigned int magic) { return (magic & LZ4S_SKIPPABLEMASK) == LZ4S_SKIPPABLE0; }


static int get_fileHandle(char* input_filename, char* output_filename, FILE** pfinput, FILE** pfoutput)
{

    if (!strcmp (input_filename, stdinmark))
    {
        DISPLAYLEVEL(4,"Using stdin for input\n");
        *pfinput = stdin;
        SET_BINARY_MODE(stdin);
    }
    else
    {
        *pfinput = fopen(input_filename, "rb");
    }

    if (!strcmp (output_filename, stdoutmark))
    {
        DISPLAYLEVEL(4,"Using stdout for output\n");
        *pfoutput = stdout;
        SET_BINARY_MODE(stdout);
    }
    else
    {
        // Check if destination file already exists
        *pfoutput=0;
        if (output_filename != nulmark) *pfoutput = fopen( output_filename, "rb" );
        if (*pfoutput!=0)
        {
            fclose(*pfoutput);
            if (!overwrite)
            {
                char ch;
                DISPLAYLEVEL(2, "Warning : %s already exists\n", output_filename);
                DISPLAYLEVEL(2, "Overwrite ? (Y/N) : ");
                if (displayLevel <= 1) EXM_THROW(11, "Operation aborted : %s already exists", output_filename);   // No interaction possible
                ch = (char)getchar();
                if ((ch!='Y') && (ch!='y')) EXM_THROW(11, "Operation aborted : %s already exists", output_filename);
            }
        }
        *pfoutput = fopen( output_filename, "wb" );
    }

    if ( *pfinput==0 ) EXM_THROW(12, "Pb opening %s", input_filename);
    if ( *pfoutput==0) EXM_THROW(13, "Pb opening %s", output_filename);

    return 0;
}


// LZ4IO_compressFilename_Legacy : This function is intentionally "hidden" (not published in .h)
// It generates compressed streams using the old 'legacy' format
int LZ4IO_compressFilename_Legacy(char* input_filename, char* output_filename, int compressionlevel)
{
    int (*compressionFunction)(const char*, char*, int);
    unsigned long long filesize = 0;
    unsigned long long compressedfilesize = MAGICNUMBER_SIZE;
    char* in_buff;
    char* out_buff;
    FILE* finput;
    FILE* foutput;
    clock_t start, end;
    size_t sizeCheck;


    // Init
    if (compressionlevel < 3) compressionFunction = LZ4_compress; else compressionFunction = LZ4_compressHC;
    start = clock();
    get_fileHandle(input_filename, output_filename, &finput, &foutput);
    if ((displayLevel==2) && (compressionlevel==1)) displayLevel=3;

    // Allocate Memory
    in_buff = (char*)malloc(LEGACY_BLOCKSIZE);
    out_buff = (char*)malloc(LZ4_compressBound(LEGACY_BLOCKSIZE));
    if (!in_buff || !out_buff) EXM_THROW(21, "Allocation error : not enough memory");

    // Write Archive Header
    *(unsigned int*)out_buff = LITTLE_ENDIAN_32(LEGACY_MAGICNUMBER);
    sizeCheck = fwrite(out_buff, 1, MAGICNUMBER_SIZE, foutput);
    if (sizeCheck!=MAGICNUMBER_SIZE) EXM_THROW(22, "Write error : cannot write header");

    // Main Loop
    while (1)
    {
        unsigned int outSize;
        // Read Block
        int inSize = (int) fread(in_buff, (size_t)1, (size_t)LEGACY_BLOCKSIZE, finput);
        if( inSize<=0 ) break;
        filesize += inSize;
        DISPLAYLEVEL(3, "\rRead : %i MB   ", (int)(filesize>>20));

        // Compress Block
        outSize = compressionFunction(in_buff, out_buff+4, inSize);
        compressedfilesize += outSize+4;
        DISPLAYLEVEL(3, "\rRead : %i MB  ==> %.2f%%   ", (int)(filesize>>20), (double)compressedfilesize/filesize*100);

        // Write Block
        * (unsigned int*) out_buff = LITTLE_ENDIAN_32(outSize);
        sizeCheck = fwrite(out_buff, 1, outSize+4, foutput);
        if (sizeCheck!=(size_t)(outSize+4)) EXM_THROW(23, "Write error : cannot write compressed block");
    }

    // Status
    end = clock();
    DISPLAYLEVEL(2, "\r%79s\r", "");
    DISPLAYLEVEL(2,"Compressed %llu bytes into %llu bytes ==> %.2f%%\n",
        (unsigned long long) filesize, (unsigned long long) compressedfilesize, (double)compressedfilesize/filesize*100);
    {
        double seconds = (double)(end - start)/CLOCKS_PER_SEC;
        DISPLAYLEVEL(4,"Done in %.2f s ==> %.2f MB/s\n", seconds, (double)filesize / seconds / 1024 / 1024);
    }

    // Close & Free
    free(in_buff);
    free(out_buff);
    fclose(finput);
    fclose(foutput);

    return 0;
}


static void* LZ4IO_LZ4_createStream (const char* inputBuffer)
{
    (void)inputBuffer;
    return calloc(8, LZ4_STREAMSIZE_U64);
}

static int LZ4IO_LZ4_compress_limitedOutput_continue (void* ctx, const char* source, char* dest, int inputSize, int maxOutputSize, int compressionLevel)
{
    (void)compressionLevel;
    return LZ4_compress_limitedOutput_continue(ctx, source, dest, inputSize, maxOutputSize);
}

static int LZ4IO_LZ4_saveDict (void* LZ4_stream, char* safeBuffer, int dictSize)
{
    return LZ4_saveDict ((LZ4_stream_t*) LZ4_stream, safeBuffer, dictSize);
}

static int LZ4IO_LZ4_slideInputBufferHC (void* ctx, char* buffer, int size)
{
    (void)size; (void)buffer;
    LZ4_slideInputBufferHC (ctx);
    return 1;
}


static int LZ4IO_free (void* ptr)
{
    free(ptr);
    return 0;
}

static int compress_file_blockDependency(char* input_filename, char* output_filename, int compressionlevel)
{
    void* (*initFunction)       (const char*);
    int   (*compressionFunction)(void*, const char*, char*, int, int, int);
    int   (*nextBlockFunction)  (void*, char*, int);
    int   (*freeFunction)       (void*);
    void* ctx;
    unsigned long long filesize = 0;
    unsigned long long compressedfilesize = 0;
    unsigned int checkbits;
    char* in_buff, *in_blockStart;
    char* out_buff;
    FILE* finput;
    FILE* foutput;
    clock_t start, end;
    unsigned int blockSize, inputBufferSize;
    size_t sizeCheck, header_size;
    XXH32_state_t streamCRC;

    // Init
    start = clock();
    if ((displayLevel==2) && (compressionlevel>=3)) displayLevel=3;

    if (compressionlevel<3)
    {
        initFunction = LZ4IO_LZ4_createStream;
        compressionFunction = LZ4IO_LZ4_compress_limitedOutput_continue;
        nextBlockFunction = LZ4IO_LZ4_saveDict;
        freeFunction = LZ4IO_free;
    }
    else
    {
        initFunction = LZ4_createHC;
        compressionFunction = LZ4_compressHC2_limitedOutput_continue;
        nextBlockFunction = LZ4IO_LZ4_slideInputBufferHC;
        freeFunction = LZ4IO_free;
    }

    get_fileHandle(input_filename, output_filename, &finput, &foutput);
    blockSize = LZ4S_GetBlockSize_FromBlockId (globalBlockSizeId);

    // Allocate Memory
    inputBufferSize = 64 KB + blockSize;
    in_buff  = (char*)malloc(inputBufferSize);
    out_buff = (char*)malloc(blockSize+CACHELINE);
    if (!in_buff || !out_buff) EXM_THROW(31, "Allocation error : not enough memory");
    in_blockStart = in_buff + 64 KB;
    if (compressionlevel>=3) in_blockStart = in_buff;
    if (streamChecksum) XXH32_reset(&streamCRC, LZ4S_CHECKSUM_SEED);
    ctx = initFunction(in_buff);

    // Write Archive Header
    *(unsigned int*)out_buff = LITTLE_ENDIAN_32(LZ4S_MAGICNUMBER);   // Magic Number, in Little Endian convention
    *(out_buff+4)  = (1 & _2BITS) << 6 ;                             // Version('01')
    *(out_buff+4) |= (blockIndependence & _1BIT) << 5;
    *(out_buff+4) |= (blockChecksum & _1BIT) << 4;
    *(out_buff+4) |= (streamChecksum & _1BIT) << 2;
    *(out_buff+5)  = (char)((globalBlockSizeId & _3BITS) << 4);
    checkbits = XXH32((out_buff+4), 2, LZ4S_CHECKSUM_SEED);
    checkbits = LZ4S_GetCheckBits_FromXXH(checkbits);
    *(out_buff+6)  = (unsigned char) checkbits;
    header_size = 7;
    sizeCheck = fwrite(out_buff, 1, header_size, foutput);
    if (sizeCheck!=header_size) EXM_THROW(32, "Write error : cannot write header");
    compressedfilesize += header_size;

    // Main Loop
    while (1)
    {
        unsigned int outSize;
        unsigned int inSize;

        // Read Block
        inSize = (unsigned int) fread(in_blockStart, (size_t)1, (size_t)blockSize, finput);
        if( inSize==0 ) break;   // No more input : end of compression
        filesize += inSize;
        DISPLAYLEVEL(3, "\rRead : %i MB   ", (int)(filesize>>20));
        if (streamChecksum) XXH32_update(&streamCRC, in_blockStart, inSize);

        // Compress Block
        outSize = compressionFunction(ctx, in_blockStart, out_buff+4, inSize, inSize-1, compressionlevel);
        if (outSize > 0) compressedfilesize += outSize+4; else compressedfilesize += inSize+4;
        if (blockChecksum) compressedfilesize+=4;
        DISPLAYLEVEL(3, "==> %.2f%%   ", (double)compressedfilesize/filesize*100);

        // Write Block
        if (outSize > 0)
        {
            int sizeToWrite;
            * (unsigned int*) out_buff = LITTLE_ENDIAN_32(outSize);
            if (blockChecksum)
            {
                unsigned int checksum = XXH32(out_buff+4, outSize, LZ4S_CHECKSUM_SEED);
                * (unsigned int*) (out_buff+4+outSize) = LITTLE_ENDIAN_32(checksum);
            }
            sizeToWrite = 4 + outSize + (4*blockChecksum);
            sizeCheck = fwrite(out_buff, 1, sizeToWrite, foutput);
            if (sizeCheck!=(size_t)(sizeToWrite)) EXM_THROW(33, "Write error : cannot write compressed block");
        }
        else   // Copy Original
        {
            * (unsigned int*) out_buff = LITTLE_ENDIAN_32(inSize|0x80000000);   // Add Uncompressed flag
            sizeCheck = fwrite(out_buff, 1, 4, foutput);
            if (sizeCheck!=(size_t)(4)) EXM_THROW(34, "Write error : cannot write block header");
            sizeCheck = fwrite(in_blockStart, 1, inSize, foutput);
            if (sizeCheck!=(size_t)(inSize)) EXM_THROW(35, "Write error : cannot write block");
            if (blockChecksum)
            {
                unsigned int checksum = XXH32(in_blockStart, inSize, LZ4S_CHECKSUM_SEED);
                * (unsigned int*) out_buff = LITTLE_ENDIAN_32(checksum);
                sizeCheck = fwrite(out_buff, 1, 4, foutput);
                if (sizeCheck!=(size_t)(4)) EXM_THROW(36, "Write error : cannot write block checksum");
            }
        }
        {
            size_t sizeToMove = 64 KB;
            if (inSize < 64 KB) sizeToMove = inSize;
            nextBlockFunction(ctx, in_blockStart - sizeToMove, (int)sizeToMove);
            if (compressionlevel>=3) in_blockStart = in_buff + 64 KB;
        }
    }

    // End of Stream mark
    * (unsigned int*) out_buff = LZ4S_EOS;
    sizeCheck = fwrite(out_buff, 1, 4, foutput);
    if (sizeCheck!=(size_t)(4)) EXM_THROW(37, "Write error : cannot write end of stream");
    compressedfilesize += 4;
    if (streamChecksum)
    {
        unsigned int checksum = XXH32_digest(&streamCRC);
        * (unsigned int*) out_buff = LITTLE_ENDIAN_32(checksum);
        sizeCheck = fwrite(out_buff, 1, 4, foutput);
        if (sizeCheck!=(size_t)(4)) EXM_THROW(37, "Write error : cannot write stream checksum");
        compressedfilesize += 4;
    }

    // Status
    end = clock();
    DISPLAYLEVEL(2, "\r%79s\r", "");
    DISPLAYLEVEL(2, "Compressed %llu bytes into %llu bytes ==> %.2f%%\n",
        (unsigned long long) filesize, (unsigned long long) compressedfilesize, (double)compressedfilesize/filesize*100);
    {
        double seconds = (double)(end - start)/CLOCKS_PER_SEC;
        DISPLAYLEVEL(4, "Done in %.2f s ==> %.2f MB/s\n", seconds, (double)filesize / seconds / 1024 / 1024);
    }

    // Close & Free
    freeFunction(ctx);
    free(in_buff);
    free(out_buff);
    fclose(finput);
    fclose(foutput);

    return 0;
}


static int LZ4_compress_limitedOutput_local(const char* src, char* dst, int size, int maxOut, int clevel)
{ (void)clevel; return LZ4_compress_limitedOutput(src, dst, size, maxOut); }

int LZ4IO_compressFilename(char* input_filename, char* output_filename, int compressionLevel)
{
    int (*compressionFunction)(const char*, char*, int, int, int);
    unsigned long long filesize = 0;
    unsigned long long compressedfilesize = 0;
    unsigned int checkbits;
    char* in_buff;
    char* out_buff;
    char* headerBuffer;
    FILE* finput;
    FILE* foutput;
    clock_t start, end;
    int blockSize;
    size_t sizeCheck, header_size, readSize;
    XXH32_state_t streamCRC;

    // Branch out
    if (blockIndependence==0) return compress_file_blockDependency(input_filename, output_filename, compressionLevel);

    // Init
    start = clock();
    if ((displayLevel==2) && (compressionLevel>=3)) displayLevel=3;
    if (compressionLevel <= 3) compressionFunction = LZ4_compress_limitedOutput_local;
    else { compressionFunction = LZ4_compressHC2_limitedOutput; }
    get_fileHandle(input_filename, output_filename, &finput, &foutput);
    blockSize = LZ4S_GetBlockSize_FromBlockId (globalBlockSizeId);

    // Allocate Memory
    in_buff  = (char*)malloc(blockSize);
    out_buff = (char*)malloc(blockSize+CACHELINE);
    headerBuffer = (char*)malloc(LZ4S_MAXHEADERSIZE);
    if (!in_buff || !out_buff || !(headerBuffer)) EXM_THROW(31, "Allocation error : not enough memory");
    if (streamChecksum) XXH32_reset(&streamCRC, LZ4S_CHECKSUM_SEED);

    // Write Archive Header
    *(unsigned int*)headerBuffer = LITTLE_ENDIAN_32(LZ4S_MAGICNUMBER);   // Magic Number, in Little Endian convention
    *(headerBuffer+4)  = (1 & _2BITS) << 6 ;                             // Version('01')
    *(headerBuffer+4) |= (blockIndependence & _1BIT) << 5;
    *(headerBuffer+4) |= (blockChecksum & _1BIT) << 4;
    *(headerBuffer+4) |= (streamChecksum & _1BIT) << 2;
    *(headerBuffer+5)  = (char)((globalBlockSizeId & _3BITS) << 4);
    checkbits = XXH32((headerBuffer+4), 2, LZ4S_CHECKSUM_SEED);
    checkbits = LZ4S_GetCheckBits_FromXXH(checkbits);
    *(headerBuffer+6)  = (unsigned char) checkbits;
    header_size = 7;

    // Write header
    sizeCheck = fwrite(headerBuffer, 1, header_size, foutput);
    if (sizeCheck!=header_size) EXM_THROW(32, "Write error : cannot write header");
    compressedfilesize += header_size;

    // read first block
    readSize = fread(in_buff, (size_t)1, (size_t)blockSize, finput);

    // Main Loop
    while (readSize>0)
    {
        unsigned int outSize;

        filesize += readSize;
        DISPLAYLEVEL(3, "\rRead : %i MB   ", (int)(filesize>>20));
        if (streamChecksum) XXH32_update(&streamCRC, in_buff, (int)readSize);

        // Compress Block
        outSize = compressionFunction(in_buff, out_buff+4, (int)readSize, (int)readSize-1, compressionLevel);
        if (outSize > 0) compressedfilesize += outSize+4; else compressedfilesize += readSize+4;
        if (blockChecksum) compressedfilesize+=4;
        DISPLAYLEVEL(3, "==> %.2f%%   ", (double)compressedfilesize/filesize*100);

        // Write Block
        if (outSize > 0)
        {
            int sizeToWrite;
            * (unsigned int*) out_buff = LITTLE_ENDIAN_32(outSize);
            if (blockChecksum)
            {
                unsigned int checksum = XXH32(out_buff+4, outSize, LZ4S_CHECKSUM_SEED);
                * (unsigned int*) (out_buff+4+outSize) = LITTLE_ENDIAN_32(checksum);
            }
            sizeToWrite = 4 + outSize + (4*blockChecksum);
            sizeCheck = fwrite(out_buff, 1, sizeToWrite, foutput);
            if (sizeCheck!=(size_t)(sizeToWrite)) EXM_THROW(33, "Write error : cannot write compressed block");
        }
        else  // Copy Original Uncompressed
        {
            * (unsigned int*) out_buff = LITTLE_ENDIAN_32(((unsigned long)readSize)|0x80000000);   // Add Uncompressed flag
            sizeCheck = fwrite(out_buff, 1, 4, foutput);
            if (sizeCheck!=(size_t)(4)) EXM_THROW(34, "Write error : cannot write block header");
            sizeCheck = fwrite(in_buff, 1, readSize, foutput);
            if (sizeCheck!=readSize) EXM_THROW(35, "Write error : cannot write block");
            if (blockChecksum)
            {
                unsigned int checksum = XXH32(in_buff, (int)readSize, LZ4S_CHECKSUM_SEED);
                * (unsigned int*) out_buff = LITTLE_ENDIAN_32(checksum);
                sizeCheck = fwrite(out_buff, 1, 4, foutput);
                if (sizeCheck!=(size_t)(4)) EXM_THROW(36, "Write error : cannot write block checksum");
            }
        }

        // Read next block
        readSize = fread(in_buff, (size_t)1, (size_t)blockSize, finput);
    }

    // End of Stream mark
    * (unsigned int*) out_buff = LZ4S_EOS;
    sizeCheck = fwrite(out_buff, 1, 4, foutput);
    if (sizeCheck!=(size_t)(4)) EXM_THROW(37, "Write error : cannot write end of stream");
    compressedfilesize += 4;
    if (streamChecksum)
    {
        unsigned int checksum = XXH32_digest(&streamCRC);
        *(unsigned int*) out_buff = LITTLE_ENDIAN_32(checksum);
        sizeCheck = fwrite(out_buff, 1, 4, foutput);
        if (sizeCheck!=(size_t)(4)) EXM_THROW(37, "Write error : cannot write stream checksum");
        compressedfilesize += 4;
    }

    // Close & Free
    free(in_buff);
    free(out_buff);
    free(headerBuffer);
    fclose(finput);
    fclose(foutput);

    // Final Status
    end = clock();
    DISPLAYLEVEL(2, "\r%79s\r", "");
    DISPLAYLEVEL(2, "Compressed %llu bytes into %llu bytes ==> %.2f%%\n",
        (unsigned long long) filesize, (unsigned long long) compressedfilesize, (double)compressedfilesize/filesize*100);
    {
        double seconds = (double)(end - start)/CLOCKS_PER_SEC;
        DISPLAYLEVEL(4, "Done in %.2f s ==> %.2f MB/s\n", seconds, (double)filesize / seconds / 1024 / 1024);
    }

    return 0;
}


/* ********************************************************************* */
/* ********************** LZ4 File / Stream decoding ******************* */
/* ********************************************************************* */

static unsigned long long decodeLegacyStream(FILE* finput, FILE* foutput)
{
    unsigned long long filesize = 0;
    char* in_buff;
    char* out_buff;
    unsigned int blockSize;


    // Allocate Memory
    in_buff = (char*)malloc(LZ4_compressBound(LEGACY_BLOCKSIZE));
    out_buff = (char*)malloc(LEGACY_BLOCKSIZE);
    if (!in_buff || !out_buff) EXM_THROW(51, "Allocation error : not enough memory");

    // Main Loop
    while (1)
    {
        int decodeSize;
        size_t sizeCheck;

        // Block Size
        sizeCheck = fread(&blockSize, 1, 4, finput);
        if (sizeCheck==0) break;                   // Nothing to read : file read is completed
        blockSize = LITTLE_ENDIAN_32(blockSize);   // Convert to Little Endian
        if (blockSize > LZ4_COMPRESSBOUND(LEGACY_BLOCKSIZE))
        {   // Cannot read next block : maybe new stream ?
            fseek(finput, -4, SEEK_CUR);
            break;
        }

        // Read Block
        sizeCheck = fread(in_buff, 1, blockSize, finput);

        // Decode Block
        decodeSize = LZ4_decompress_safe(in_buff, out_buff, blockSize, LEGACY_BLOCKSIZE);
        if (decodeSize < 0) EXM_THROW(52, "Decoding Failed ! Corrupted input detected !");
        filesize += decodeSize;

        // Write Block
        sizeCheck = fwrite(out_buff, 1, decodeSize, foutput);
        if (sizeCheck != (size_t)decodeSize) EXM_THROW(53, "Write error : cannot write decoded block into output\n");
    }

    // Free
    free(in_buff);
    free(out_buff);

    return filesize;
}


static unsigned long long decodeLZ4S(FILE* finput, FILE* foutput)
{
    unsigned long long filesize = 0;
    char* in_buff;
    char* out_buff, *out_start, *out_end;
    unsigned char descriptor[LZ4S_MAXHEADERSIZE];
    size_t nbReadBytes;
    int decodedBytes=0;
    unsigned int maxBlockSize;
    size_t sizeCheck;
    int blockChecksumFlag, streamChecksumFlag, blockIndependenceFlag;
    XXH32_state_t streamCRC;
    int (*decompressionFunction)(LZ4_streamDecode_t* ctx, const char* src, char* dst, int cSize, int maxOSize) = LZ4_decompress_safe_continue;
    LZ4_streamDecode_t ctx;

    // init
    memset(&ctx, 0, sizeof(ctx));

    // Decode stream descriptor
    nbReadBytes = fread(descriptor, 1, 3, finput);
    if (nbReadBytes != 3) EXM_THROW(61, "Unreadable header");
    {
        int version       = (descriptor[0] >> 6) & _2BITS;
        int streamSize    = (descriptor[0] >> 3) & _1BIT;
        int reserved1     = (descriptor[0] >> 1) & _1BIT;
        int dictionary    = (descriptor[0] >> 0) & _1BIT;

        int reserved2     = (descriptor[1] >> 7) & _1BIT;
        int blockSizeId   = (descriptor[1] >> 4) & _3BITS;
        int reserved3     = (descriptor[1] >> 0) & _4BITS;
        int checkBits     = (descriptor[2] >> 0) & _8BITS;
        int checkBits_xxh32;

        blockIndependenceFlag=(descriptor[0] >> 5) & _1BIT;
        blockChecksumFlag = (descriptor[0] >> 4) & _1BIT;
        streamChecksumFlag= (descriptor[0] >> 2) & _1BIT;

        if (version != 1)       EXM_THROW(62, "Wrong version number");
        if (streamSize == 1)    EXM_THROW(64, "Does not support stream size");
        if (reserved1 != 0)     EXM_THROW(65, "Wrong value for reserved bits");
        if (dictionary == 1)    EXM_THROW(66, "Does not support dictionary");
        if (reserved2 != 0)     EXM_THROW(67, "Wrong value for reserved bits");
        if (blockSizeId < 4)    EXM_THROW(68, "Unsupported block size");
        if (reserved3 != 0)     EXM_THROW(67, "Wrong value for reserved bits");
        maxBlockSize = LZ4S_GetBlockSize_FromBlockId(blockSizeId);
        // Checkbits verification
        descriptor[1] &= 0xF0;
        checkBits_xxh32 = XXH32(descriptor, 2, LZ4S_CHECKSUM_SEED);
        checkBits_xxh32 = LZ4S_GetCheckBits_FromXXH(checkBits_xxh32);
        if (checkBits != checkBits_xxh32) EXM_THROW(69, "Stream descriptor error detected");
    }

    // Allocate Memory
    {
        size_t outBuffSize = maxBlockSize + 64 KB;
        if (outBuffSize < MIN_STREAM_BUFSIZE) outBuffSize = MIN_STREAM_BUFSIZE;
        in_buff  = (char*)malloc(maxBlockSize);
        out_buff = (char*)malloc(outBuffSize);
        out_start = out_buff;
        out_end = out_start + outBuffSize;
        if (!in_buff || !out_buff) EXM_THROW(70, "Allocation error : not enough memory");
        if (streamChecksumFlag) XXH32_reset(&streamCRC, LZ4S_CHECKSUM_SEED);
    }

    // Main Loop
    while (1)
    {
        unsigned int blockSize, uncompressedFlag;

        // Block Size
        nbReadBytes = fread(&blockSize, 1, 4, finput);
        if( nbReadBytes != 4 ) EXM_THROW(71, "Read error : cannot read next block size");
        if (blockSize == LZ4S_EOS) break;          // End of Stream Mark : stream is completed
        blockSize = LITTLE_ENDIAN_32(blockSize);   // Convert to little endian
        uncompressedFlag = blockSize >> 31;
        blockSize &= 0x7FFFFFFF;
        if (blockSize > maxBlockSize) EXM_THROW(72, "Error : invalid block size");

        // Read Block
        nbReadBytes = fread(in_buff, 1, blockSize, finput);
        if( nbReadBytes != blockSize ) EXM_THROW(73, "Read error : cannot read data block" );

        // Check Block
        if (blockChecksumFlag)
        {
            unsigned int checksum = XXH32(in_buff, blockSize, LZ4S_CHECKSUM_SEED);
            unsigned int readChecksum;
            sizeCheck = fread(&readChecksum, 1, 4, finput);
            if( sizeCheck != 4 ) EXM_THROW(74, "Read error : cannot read next block size");
            readChecksum = LITTLE_ENDIAN_32(readChecksum);   // Convert to little endian
            if (checksum != readChecksum) EXM_THROW(75, "Error : invalid block checksum detected");
        }

        if (uncompressedFlag)
        {
            // Write uncompressed Block
            sizeCheck = fwrite(in_buff, 1, blockSize, foutput);
            if (sizeCheck != (size_t)blockSize) EXM_THROW(76, "Write error : cannot write data block");
            filesize += blockSize;
            if (streamChecksumFlag) XXH32_update(&streamCRC, in_buff, blockSize);
            if (!blockIndependenceFlag)
            {
                // handle dictionary for streaming
                memcpy(in_buff + blockSize - 64 KB, out_buff, 64 KB);
                LZ4_setStreamDecode(&ctx, out_buff, 64 KB);
                out_start = out_buff + 64 KB;
            }
        }
        else
        {
            // Decode Block
            if (out_start + maxBlockSize > out_end) out_start = out_buff;
            decodedBytes = decompressionFunction(&ctx, in_buff, out_start, blockSize, maxBlockSize);
            if (decodedBytes < 0) EXM_THROW(77, "Decoding Failed ! Corrupted input detected !");
            filesize += decodedBytes;
            if (streamChecksumFlag) XXH32_update(&streamCRC, out_start, decodedBytes);

            // Write Block
            sizeCheck = fwrite(out_start, 1, decodedBytes, foutput);
            if (sizeCheck != (size_t)decodedBytes) EXM_THROW(78, "Write error : cannot write decoded block\n");
            out_start += decodedBytes;
        }

    }

    // Stream Checksum
    if (streamChecksumFlag)
    {
        unsigned int checksum = XXH32_digest(&streamCRC);
        unsigned int readChecksum;
        sizeCheck = fread(&readChecksum, 1, 4, finput);
        if (sizeCheck != 4) EXM_THROW(74, "Read error : cannot read stream checksum");
        readChecksum = LITTLE_ENDIAN_32(readChecksum);   // Convert to little endian
        if (checksum != readChecksum) EXM_THROW(79, "Error : invalid stream checksum detected");
    }

    // Free
    free(in_buff);
    free(out_buff);

    return filesize;
}


#define ENDOFSTREAM ((unsigned long long)-1)
static unsigned long long selectDecoder( FILE* finput,  FILE* foutput)
{
    unsigned int magicNumber, size;
    int errorNb;
    size_t nbReadBytes;

    // Check Archive Header
    nbReadBytes = fread(&magicNumber, 1, MAGICNUMBER_SIZE, finput);
    if (nbReadBytes==0) return ENDOFSTREAM;                  // EOF
    if (nbReadBytes != MAGICNUMBER_SIZE) EXM_THROW(41, "Unrecognized header : Magic Number unreadable");
    magicNumber = LITTLE_ENDIAN_32(magicNumber);   // Convert to Little Endian format
    if (LZ4S_isSkippableMagicNumber(magicNumber)) magicNumber = LZ4S_SKIPPABLE0;  // fold skippable magic numbers

    switch(magicNumber)
    {
    case LZ4S_MAGICNUMBER:
        return DEFAULT_DECOMPRESSOR(finput, foutput);
    case LEGACY_MAGICNUMBER:
        DISPLAYLEVEL(4, "Detected : Legacy format \n");
        return decodeLegacyStream(finput, foutput);
    case LZ4S_SKIPPABLE0:
        DISPLAYLEVEL(4, "Skipping detected skippable area \n");
        nbReadBytes = fread(&size, 1, 4, finput);
        if (nbReadBytes != 4) EXM_THROW(42, "Stream error : skippable size unreadable");
        size = LITTLE_ENDIAN_32(size);     // Convert to Little Endian format
        errorNb = fseek(finput, size, SEEK_CUR);
        if (errorNb != 0) EXM_THROW(43, "Stream error : cannot skip skippable area");
        return selectDecoder(finput, foutput);
    EXTENDED_FORMAT;
    default:
        if (ftell(finput) == MAGICNUMBER_SIZE) EXM_THROW(44,"Unrecognized header : file cannot be decoded");   // Wrong magic number at the beginning of 1st stream
        DISPLAYLEVEL(2, "Stream followed by unrecognized data\n");
        return ENDOFSTREAM;
    }
}


int LZ4IO_decompressFilename(char* input_filename, char* output_filename)
{
    unsigned long long filesize = 0, decodedSize=0;
    FILE* finput;
    FILE* foutput;
    clock_t start, end;


    // Init
    start = clock();
    get_fileHandle(input_filename, output_filename, &finput, &foutput);

    // Loop over multiple streams
    do
    {
        decodedSize = selectDecoder(finput, foutput);
        if (decodedSize != ENDOFSTREAM)
            filesize += decodedSize;
    } while (decodedSize != ENDOFSTREAM);

    // Final Status
    end = clock();
    DISPLAYLEVEL(2, "\r%79s\r", "");
    DISPLAYLEVEL(2, "Successfully decoded %llu bytes \n", filesize);
    {
        double seconds = (double)(end - start)/CLOCKS_PER_SEC;
        DISPLAYLEVEL(4, "Done in %.2f s ==> %.2f MB/s\n", seconds, (double)filesize / seconds / 1024 / 1024);
    }

    // Close
    fclose(finput);
    fclose(foutput);

    // Error status = OK
    return 0;
}

