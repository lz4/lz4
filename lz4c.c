/*
  LZ4c - LZ4 Compression CLI program 
  Copyright (C) Yann Collet 2011-2013
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
/*
  Note : this is stand-alone program.
  It is not part of LZ4 compression library, it is a user program of LZ4 library.
  The license of LZ4 library is BSD.
  The license of xxHash library is BSD.
  The license of this compression CLI program is GPLv2.
*/

//**************************************
// Compiler Options
//**************************************
// Disable some Visual warning messages
#ifdef _MSC_VER  // Visual Studio
#  define _CRT_SECURE_NO_WARNINGS
#  define _CRT_SECURE_NO_DEPRECATE     // VS2005
#  pragma warning(disable : 4127)      // disable: C4127: conditional expression is constant
#endif


//****************************
// Includes
//****************************
#include <stdio.h>    // fprintf, fopen, fread, _fileno(?)
#include <stdlib.h>   // malloc
#include <string.h>   // strcmp
#include <time.h>     // clock
#ifdef _WIN32
#include <io.h>       // _setmode
#include <fcntl.h>    // _O_BINARY
#endif
#include "lz4.h"
#include "lz4hc.h"
#include "bench.h"
#include "xxhash.h"


//**************************************
// Compiler-specific functions
//**************************************
#define GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)

#if defined(_MSC_VER)    // Visual Studio
#  define swap32 _byteswap_ulong
#elif GCC_VERSION >= 403
#  define swap32 __builtin_bswap32
#else
  static inline unsigned int swap32(unsigned int x) {
    return	((x << 24) & 0xff000000 ) |
        ((x <<  8) & 0x00ff0000 ) |
        ((x >>  8) & 0x0000ff00 ) |
        ((x >> 24) & 0x000000ff );
  }
#endif


//****************************
// Constants
//****************************
#define COMPRESSOR_NAME "LZ4 Compression CLI"
#define COMPRESSOR_VERSION ""
#define COMPILED __DATE__
#define AUTHOR "Yann Collet"
#define EXTENSION ".lz4"
#define WELCOME_MESSAGE "*** %s %s, by %s (%s) ***\n", COMPRESSOR_NAME, COMPRESSOR_VERSION, AUTHOR, COMPILED

#define KB *(1U<<10)
#define MB *(1U<<20)
#define GB *(1U<<30)

#define _1BIT  0x01
#define _2BITS 0x03
#define _3BITS 0x07
#define _4BITS 0x0F
#define _8BITS 0xFF

#define MAGICNUMBER_SIZE 4
#define LZ4S_MAGICNUMBER   0x184D2204
#define LZ4S_SKIPPABLE0    0x184D2A50
#define LZ4S_SKIPPABLEMASK 0xFFFFFFF0
#define LEGACY_MAGICNUMBER 0x184C2102

#define CACHELINE 64
#define LEGACY_BLOCKSIZE   (8 MB)
#define MIN_STREAM_BUFSIZE (1 MB + 64 KB)
#define LZ4S_BLOCKSIZEID_DEFAULT 7
#define LZ4S_CHECKSUM_SEED 0
#define LZ4S_EOS 0
#define LZ4S_MAXHEADERSIZE (4+2+8+4+1)


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
#define DISPLAY(...) fprintf(stderr, __VA_ARGS__)


//**************************************
// Special input/output
//**************************************
#define NULL_INPUT "null"
char stdinmark[] = "stdin";
char stdoutmark[] = "stdout";
#ifdef _WIN32
char nulmark[] = "nul";
#else
char nulmark[] = "/dev/null";
#endif


//**************************************
// Local Parameters
//**************************************
static int overwrite = 0;
static int blockSizeId = LZ4S_BLOCKSIZEID_DEFAULT;
static int blockChecksum = 0;
static int streamChecksum = 1;
static int blockIndependence = 1;

//**************************************
// Exceptions
//**************************************
#define DEBUG 0
#define DEBUGOUTPUT(...) if (DEBUG) DISPLAY(__VA_ARGS__);
#define EXM_THROW(error, ...)                                             \
{                                                                         \
    DEBUGOUTPUT("Error defined at %s, line %i : \n", __FILE__, __LINE__); \
    DISPLAY("Error %i : ", error);                                        \
    DISPLAY(__VA_ARGS__);                                                 \
    DISPLAY("\n");                                                        \
    exit(error);                                                          \
}



//****************************
// Functions
//****************************
int usage(char* exename)
{
    DISPLAY( "Usage :\n");
    DISPLAY( "      %s [arg] input [output]\n", exename);
    DISPLAY( "Arguments :\n");
    DISPLAY( " -c0/-c : Fast compression (default) \n");
    DISPLAY( " -c1/-hc: High compression \n");
    DISPLAY( " -d     : decompression \n");
    DISPLAY( " -y     : overwrite without prompting \n");
    DISPLAY( " -H     : Help (this text + advanced options)\n");
    return 0;
}

int usage_advanced()
{
    DISPLAY( "\nAdvanced options :\n");
    DISPLAY( " -t     : test compressed file \n");
    DISPLAY( " -B#    : Block size [4-7](default : 7)\n");
    DISPLAY( " -BD    : Block dependency (improve compression ratio)\n");
    DISPLAY( " -BX    : enable block checksum (default:disabled)\n");
    DISPLAY( " -Sx    : disable stream checksum (default:enabled)\n");
    DISPLAY( " -b#    : benchmark files, using # [0-1] compression level\n");
    DISPLAY( " -i#    : iteration loops [1-9](default : 3), benchmark mode only\n");
    DISPLAY( "input   : can be 'stdin' (pipe) or a filename\n");
    DISPLAY( "output  : can be 'stdout'(pipe) or a filename or 'null'\n");
    DISPLAY( "          example 1 : lz4c -hc stdin compressedfile.lz4\n");
    DISPLAY( "          example 2 : lz4c -hcyB4D filename \n");
    return 0;
}

int badusage(char* exename)
{
    DISPLAY("Wrong parameters\n");
    usage(exename);
    return 0;
}


static int          LZ4S_GetBlockSize_FromBlockId (int id) { return (1 << (8 + (2 * id))); }
static unsigned int LZ4S_GetCheckBits_FromXXH (unsigned int xxh) { return (xxh >> 8) & _8BITS; }
static int          LZ4S_isSkippableMagicNumber(unsigned int magic) { return (magic & LZ4S_SKIPPABLEMASK) == LZ4S_SKIPPABLE0; }


int get_fileHandle(char* input_filename, char* output_filename, FILE** pfinput, FILE** pfoutput)
{

    if (!strcmp (input_filename, stdinmark)) 
    {
        DISPLAY( "Using stdin for input\n");
        *pfinput = stdin;
#ifdef _WIN32 // Need to set stdin/stdout to binary mode specifically for windows
        _setmode( _fileno( stdin ), _O_BINARY );
#endif
    } 
    else 
    {
        *pfinput = fopen(input_filename, "rb");
    }

    if (!strcmp (output_filename, stdoutmark)) 
    {
        DISPLAY( "Using stdout for output\n");
        *pfoutput = stdout;
#ifdef _WIN32 // Need to set stdin/stdout to binary mode specifically for windows
        _setmode( _fileno( stdout ), _O_BINARY );
#endif
    } 
    else 
    {
        // Check if destination file already exists
        *pfoutput=0;
        if (output_filename != nulmark) *pfoutput = fopen( output_filename, "rb" );
        if (*pfoutput!=0) 
        { 
            char ch;
            fclose(*pfoutput); 
            DISPLAY( "Warning : %s already exists\n", output_filename); 
            if (!overwrite)
            {
                DISPLAY( "Overwrite ? (Y/N) : ");
                ch = (char)getchar();
                if (ch!='Y') EXM_THROW(11, "Operation aborted : %s already exists", output_filename);
            }
        }
        *pfoutput = fopen( output_filename, "wb" );
    }

    if ( *pfinput==0 ) EXM_THROW(12, "Pb opening %s", input_filename);
    if ( *pfoutput==0) EXM_THROW(13, "Pb opening %s", output_filename); 

    return 0;
}



int legacy_compress_file(char* input_filename, char* output_filename, int compressionlevel)
{
    int (*compressionFunction)(const char*, char*, int);
    unsigned long long filesize = 0;
    unsigned long long compressedfilesize = MAGICNUMBER_SIZE;
    char* in_buff;
    char* out_buff;
    FILE* finput;
    FILE* foutput;
    int displayLevel = (compressionlevel>0);
    clock_t start, end;
    size_t sizeCheck;


    // Init
    switch (compressionlevel)
    {
    case 0 : compressionFunction = LZ4_compress; break;
    case 1 : compressionFunction = LZ4_compressHC; break;
    default: compressionFunction = LZ4_compress;
    }
    start = clock();
    get_fileHandle(input_filename, output_filename, &finput, &foutput);

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
        if (displayLevel) DISPLAY("Read : %i MB  \r", (int)(filesize>>20));

        // Compress Block
        outSize = compressionFunction(in_buff, out_buff+4, inSize);
        compressedfilesize += outSize+4;
        if (displayLevel) DISPLAY("Read : %i MB  ==> %.2f%%\r", (int)(filesize>>20), (double)compressedfilesize/filesize*100);

        // Write Block
        * (unsigned int*) out_buff = LITTLE_ENDIAN_32(outSize);
        sizeCheck = fwrite(out_buff, 1, outSize+4, foutput);
        if (sizeCheck!=(size_t)(outSize+4)) EXM_THROW(23, "Write error : cannot write compressed block");
    }

    // Status
    end = clock();
    DISPLAY( "Compressed %llu bytes into %llu bytes ==> %.2f%%\n",
        (unsigned long long) filesize, (unsigned long long) compressedfilesize, (double)compressedfilesize/filesize*100);
    {
        double seconds = (double)(end - start)/CLOCKS_PER_SEC;
        DISPLAY( "Done in %.2f s ==> %.2f MB/s\n", seconds, (double)filesize / seconds / 1024 / 1024);
    }

    // Close & Free
    free(in_buff);
    free(out_buff);
    fclose(finput);
    fclose(foutput);

    return 0;
}


int compress_file_blockDependency(char* input_filename, char* output_filename, int compressionlevel)
{
    void* (*initFunction)       (const char*);
    int   (*compressionFunction)(void*, const char*, char*, int, int);
    char* (*translateFunction)  (void*);
    int   (*freeFunction)       (void*);
    void* ctx;
    unsigned long long filesize = 0;
    unsigned long long compressedfilesize = 0;
    unsigned int checkbits;
    char* in_buff, *in_start, *in_end;
    char* out_buff;
    FILE* finput;
    FILE* foutput;
    int errorcode;
    int displayLevel = (compressionlevel>0);
    clock_t start, end;
    unsigned int blockSize, inputBufferSize;
    size_t sizeCheck, header_size;
    void* streamChecksumState=NULL;


    // Init
    start = clock();
    switch (compressionlevel)
    {
    case 0 :
    case 1 :
    default:
        initFunction = LZ4_createHC;
        compressionFunction = LZ4_compressHC_limitedOutput_continue;
        translateFunction = LZ4_slideInputBufferHC;
        freeFunction = LZ4_freeHC;
    }
    errorcode = get_fileHandle(input_filename, output_filename, &finput, &foutput);
    if (errorcode) return errorcode;
    blockSize = LZ4S_GetBlockSize_FromBlockId (blockSizeId);

    // Allocate Memory
    inputBufferSize = blockSize + 64 KB;
    if (inputBufferSize < MIN_STREAM_BUFSIZE) inputBufferSize = MIN_STREAM_BUFSIZE;
    in_buff  = (char*)malloc(inputBufferSize);
    out_buff = (char*)malloc(blockSize+CACHELINE);
    if (!in_buff || !out_buff) EXM_THROW(31, "Allocation error : not enough memory");
    in_start = in_buff; in_end = in_buff + inputBufferSize;
    if (streamChecksum) streamChecksumState = XXH32_init(LZ4S_CHECKSUM_SEED);
    ctx = initFunction(in_buff);

    // Write Archive Header
    *(unsigned int*)out_buff = LITTLE_ENDIAN_32(LZ4S_MAGICNUMBER);   // Magic Number, in Little Endian convention
    *(out_buff+4)  = (1 & _2BITS) << 6 ;                             // Version('01')
    *(out_buff+4) |= (blockIndependence & _1BIT) << 5;
    *(out_buff+4) |= (blockChecksum & _1BIT) << 4;
    *(out_buff+4) |= (streamChecksum & _1BIT) << 2;
    *(out_buff+5)  = (char)((blockSizeId & _3BITS) << 4);
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
        if ((in_start+blockSize) > in_end) in_start = translateFunction(ctx);
        inSize = (unsigned int) fread(in_start, (size_t)1, (size_t)blockSize, finput);
        if( inSize<=0 ) break;   // No more input : end of compression
        filesize += inSize;
        if (displayLevel) DISPLAY("Read : %i MB  \r", (int)(filesize>>20));
        if (streamChecksum) XXH32_update(streamChecksumState, in_start, inSize);

        // Compress Block
        outSize = compressionFunction(ctx, in_start, out_buff+4, inSize, inSize-1);
        if (outSize > 0) compressedfilesize += outSize+4; else compressedfilesize += inSize+4;
        if (blockChecksum) compressedfilesize+=4;
        if (displayLevel) DISPLAY("Read : %i MB  ==> %.2f%%\r", (int)(filesize>>20), (double)compressedfilesize/filesize*100);

        // Write Block
        if (outSize > 0)
        {
            unsigned int checksum;
            int sizeToWrite;
            * (unsigned int*) out_buff = LITTLE_ENDIAN_32(outSize);
            if (blockChecksum)
            {
                checksum = XXH32(out_buff+4, outSize, LZ4S_CHECKSUM_SEED);
                * (unsigned int*) (out_buff+4+outSize) = LITTLE_ENDIAN_32(checksum);
            }
            sizeToWrite = 4 + outSize + (4*blockChecksum);
            sizeCheck = fwrite(out_buff, 1, sizeToWrite, foutput);
            if (sizeCheck!=(size_t)(sizeToWrite)) EXM_THROW(33, "Write error : cannot write compressed block");

        }
        else   // Copy Original
        {
            unsigned int checksum;
            * (unsigned int*) out_buff = LITTLE_ENDIAN_32(inSize|0x80000000);   // Add Uncompressed flag
            sizeCheck = fwrite(out_buff, 1, 4, foutput);
            if (sizeCheck!=(size_t)(4)) EXM_THROW(34, "Write error : cannot write block header");
            sizeCheck = fwrite(in_start, 1, inSize, foutput);
            if (sizeCheck!=(size_t)(inSize)) EXM_THROW(35, "Write error : cannot write block");
            if (blockChecksum)
            {
                checksum = XXH32(in_start, inSize, LZ4S_CHECKSUM_SEED);
                * (unsigned int*) out_buff = LITTLE_ENDIAN_32(checksum);
                sizeCheck = fwrite(out_buff, 1, 4, foutput);
                if (sizeCheck!=(size_t)(4)) EXM_THROW(36, "Write error : cannot write block checksum");
            }
        }
        in_start += inSize;
    }

    // End of Stream mark
    * (unsigned int*) out_buff = LZ4S_EOS;
    sizeCheck = fwrite(out_buff, 1, 4, foutput);
    if (sizeCheck!=(size_t)(4)) EXM_THROW(37, "Write error : cannot write end of stream");
    compressedfilesize += 4;
    if (streamChecksum)
    {
        unsigned int checksum = XXH32_digest(streamChecksumState);
        * (unsigned int*) out_buff = LITTLE_ENDIAN_32(checksum);
        sizeCheck = fwrite(out_buff, 1, 4, foutput);
        if (sizeCheck!=(size_t)(4)) EXM_THROW(37, "Write error : cannot write stream checksum");
        compressedfilesize += 4;
    }

    // Status
    end = clock();
    DISPLAY( "Compressed %llu bytes into %llu bytes ==> %.2f%%\n",
        (unsigned long long) filesize, (unsigned long long) compressedfilesize, (double)compressedfilesize/filesize*100);
    {
        double seconds = (double)(end - start)/CLOCKS_PER_SEC;
        DISPLAY( "Done in %.2f s ==> %.2f MB/s\n", seconds, (double)filesize / seconds / 1024 / 1024);
    }

    // Close & Free
    freeFunction(ctx);
    free(in_buff);
    free(out_buff);
    fclose(finput);
    fclose(foutput);

    return 0;
}


int compress_file(char* input_filename, char* output_filename, int compressionlevel)
{
    int (*compressionFunction)(const char*, char*, int, int);
    unsigned long long filesize = 0;
    unsigned long long compressedfilesize = 0;
    unsigned int checkbits;
    char* in_buff;
    char* out_buff;
    FILE* finput;
    FILE* foutput;
    int errorcode;
    int displayLevel = (compressionlevel>0);
    clock_t start, end;
    int blockSize;
    size_t sizeCheck, header_size;
    void* streamChecksumState=NULL;

    // Branch out
    if (blockIndependence==0) return compress_file_blockDependency(input_filename, output_filename, compressionlevel);

    // Init
    start = clock();
    switch (compressionlevel)
    {
    case 0 : compressionFunction = LZ4_compress_limitedOutput; break;
    case 1 : compressionFunction = LZ4_compressHC_limitedOutput; break;
    default: compressionFunction = LZ4_compress_limitedOutput;
    }
    errorcode = get_fileHandle(input_filename, output_filename, &finput, &foutput);
    if (errorcode) return errorcode;
    blockSize = LZ4S_GetBlockSize_FromBlockId (blockSizeId);

    // Allocate Memory
    in_buff  = (char*)malloc(blockSize);
    out_buff = (char*)malloc(blockSize+CACHELINE);
    if (!in_buff || !out_buff) EXM_THROW(31, "Allocation error : not enough memory");
    if (streamChecksum) streamChecksumState = XXH32_init(LZ4S_CHECKSUM_SEED);

    // Write Archive Header
    *(unsigned int*)out_buff = LITTLE_ENDIAN_32(LZ4S_MAGICNUMBER);   // Magic Number, in Little Endian convention
    *(out_buff+4)  = (1 & _2BITS) << 6 ;                             // Version('01')
    *(out_buff+4) |= (blockIndependence & _1BIT) << 5;
    *(out_buff+4) |= (blockChecksum & _1BIT) << 4;
    *(out_buff+4) |= (streamChecksum & _1BIT) << 2;
    *(out_buff+5)  = (char)((blockSizeId & _3BITS) << 4);
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
        // Read Block
        unsigned int inSize = (unsigned int) fread(in_buff, (size_t)1, (size_t)blockSize, finput);
        if( inSize<=0 ) break;   // No more input : end of compression
        filesize += inSize;
        if (displayLevel) DISPLAY("Read : %i MB  \r", (int)(filesize>>20));
        if (streamChecksum) XXH32_update(streamChecksumState, in_buff, inSize);

        // Compress Block
        outSize = compressionFunction(in_buff, out_buff+4, inSize, inSize-1);
        if (outSize > 0) compressedfilesize += outSize+4; else compressedfilesize += inSize+4;
        if (blockChecksum) compressedfilesize+=4;
        if (displayLevel) DISPLAY("Read : %i MB  ==> %.2f%%\r", (int)(filesize>>20), (double)compressedfilesize/filesize*100);

        // Write Block
        if (outSize > 0)
        {
            unsigned int checksum;
            int sizeToWrite;
            * (unsigned int*) out_buff = LITTLE_ENDIAN_32(outSize);
            if (blockChecksum)
            {
                checksum = XXH32(out_buff+4, outSize, LZ4S_CHECKSUM_SEED);
                * (unsigned int*) (out_buff+4+outSize) = LITTLE_ENDIAN_32(checksum);
            }
            sizeToWrite = 4 + outSize + (4*blockChecksum);
            sizeCheck = fwrite(out_buff, 1, sizeToWrite, foutput);
            if (sizeCheck!=(size_t)(sizeToWrite)) EXM_THROW(33, "Write error : cannot write compressed block");

        }
        else  // Copy Original
        {
            unsigned int checksum;
            * (unsigned int*) out_buff = LITTLE_ENDIAN_32(inSize|0x80000000);   // Add Uncompressed flag
            sizeCheck = fwrite(out_buff, 1, 4, foutput);
            if (sizeCheck!=(size_t)(4)) EXM_THROW(34, "Write error : cannot write block header");
            sizeCheck = fwrite(in_buff, 1, inSize, foutput);
            if (sizeCheck!=(size_t)(inSize)) EXM_THROW(35, "Write error : cannot write block");
            if (blockChecksum)
            {
                checksum = XXH32(in_buff, inSize, LZ4S_CHECKSUM_SEED);
                * (unsigned int*) out_buff = LITTLE_ENDIAN_32(checksum);
                sizeCheck = fwrite(out_buff, 1, 4, foutput);
                if (sizeCheck!=(size_t)(4)) EXM_THROW(36, "Write error : cannot write block checksum");
            }
        }
    }

    // End of Stream mark
    * (unsigned int*) out_buff = LZ4S_EOS;
    sizeCheck = fwrite(out_buff, 1, 4, foutput);
    if (sizeCheck!=(size_t)(4)) EXM_THROW(37, "Write error : cannot write end of stream");
    compressedfilesize += 4;
    if (streamChecksum)
    {
        unsigned int checksum = XXH32_digest(streamChecksumState);
        * (unsigned int*) out_buff = LITTLE_ENDIAN_32(checksum);
        sizeCheck = fwrite(out_buff, 1, 4, foutput);
        if (sizeCheck!=(size_t)(4)) EXM_THROW(37, "Write error : cannot write stream checksum");
        compressedfilesize += 4;
    }

    // Status
    end = clock();
    DISPLAY( "Compressed %llu bytes into %llu bytes ==> %.2f%%\n",
        (unsigned long long) filesize, (unsigned long long) compressedfilesize, (double)compressedfilesize/filesize*100);
    {
        double seconds = (double)(end - start)/CLOCKS_PER_SEC;
        DISPLAY( "Done in %.2f s ==> %.2f MB/s\n", seconds, (double)filesize / seconds / 1024 / 1024);
    }

    // Close & Free
    free(in_buff);
    free(out_buff);
    fclose(finput);
    fclose(foutput);

    return 0;
}


unsigned long long decodeLegacyStream(FILE* finput, FILE* foutput)
{
    unsigned long long filesize = 0;
    char* in_buff;
    char* out_buff;
    size_t uselessRet;
    int sinkint;
    unsigned int blockSize;
    size_t sizeCheck;


    // Allocate Memory
    in_buff = (char*)malloc(LZ4_compressBound(LEGACY_BLOCKSIZE));
    out_buff = (char*)malloc(LEGACY_BLOCKSIZE);
    if (!in_buff || !out_buff) EXM_THROW(51, "Allocation error : not enough memory");

    // Main Loop
    while (1)
    {
        // Block Size
        uselessRet = fread(&blockSize, 1, 4, finput);
        if( uselessRet==0 ) break;                 // Nothing to read : file read is completed
        blockSize = LITTLE_ENDIAN_32(blockSize);   // Convert to Little Endian
        if (blockSize > LZ4_COMPRESSBOUND(LEGACY_BLOCKSIZE)) 
        {   // Cannot read next block : maybe new stream ?
            fseek(finput, -4, SEEK_CUR);
            break;
        }

        // Read Block
        uselessRet = fread(in_buff, 1, blockSize, finput);

        // Decode Block
        sinkint = LZ4_decompress_safe(in_buff, out_buff, blockSize, LEGACY_BLOCKSIZE);
        if (sinkint < 0) EXM_THROW(52, "Decoding Failed ! Corrupted input detected !");
        filesize += sinkint;

        // Write Block
        sizeCheck = fwrite(out_buff, 1, sinkint, foutput);
        if (sizeCheck != (size_t)sinkint) EXM_THROW(53, "Write error : cannot write decoded block into output\n");
    }

    // Free
    free(in_buff);
    free(out_buff);

    return filesize;
}


unsigned long long decodeLZ4S(FILE* finput, FILE* foutput)
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
    void* streamChecksumState=NULL;
    int (*decompressionFunction)(const char*, char*, int, int) = LZ4_decompress_safe;
    unsigned int prefix64k = 0;

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

    if (!blockIndependenceFlag)
    {
        decompressionFunction = LZ4_decompress_safe_withPrefix64k;
        prefix64k = 64 KB;
    }

    // Allocate Memory
    {
        unsigned int outbuffSize = prefix64k+maxBlockSize;
        in_buff  = (char*)malloc(maxBlockSize);
        if (outbuffSize < MIN_STREAM_BUFSIZE) outbuffSize = MIN_STREAM_BUFSIZE;
        out_buff = (char*)malloc(outbuffSize); 
        out_end = out_buff + outbuffSize;
        out_start = out_buff + prefix64k;
        if (!in_buff || !out_buff) EXM_THROW(70, "Allocation error : not enough memory");
    }
    if (streamChecksumFlag) streamChecksumState = XXH32_init(LZ4S_CHECKSUM_SEED);

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
            if (streamChecksumFlag) XXH32_update(streamChecksumState, in_buff, blockSize);
            if (!blockIndependenceFlag)
            {
                if (blockSize >= prefix64k)
                {
                    memcpy(out_buff, in_buff + (blockSize - prefix64k), prefix64k);   // Required for reference for next blocks
                    out_start = out_buff + prefix64k;
                    continue;
                }
                else
                {
                    memcpy(out_start, in_buff, blockSize);
                }
            }
        }
        else
        {
            // Decode Block
            decodedBytes = decompressionFunction(in_buff, out_start, blockSize, maxBlockSize);
            if (decodedBytes < 0) EXM_THROW(77, "Decoding Failed ! Corrupted input detected !");
            filesize += decodedBytes;
            if (streamChecksumFlag) XXH32_update(streamChecksumState, out_start, decodedBytes);

            // Write Block
            sizeCheck = fwrite(out_start, 1, decodedBytes, foutput);
            if (sizeCheck != (size_t)decodedBytes) EXM_THROW(78, "Write error : cannot write decoded block\n");
        }

        if (!blockIndependenceFlag)
        {
            out_start += decodedBytes;
            if (out_start + maxBlockSize > out_end) 
            {
                memcpy(out_buff, out_start - prefix64k, prefix64k); 
                out_start = out_buff + prefix64k; 
            }
        }
    }

    // Stream Checksum
    if (streamChecksumFlag)
    {
        unsigned int checksum = XXH32_digest(streamChecksumState);
        unsigned int readChecksum;
        sizeCheck = fread(&readChecksum, 1, 4, finput);
        if (sizeCheck != 4) EXM_THROW(74, "Read error : cannot read stream checksum");
        readChecksum = LITTLE_ENDIAN_32(readChecksum);   // Convert to little endian
        if (checksum != readChecksum) EXM_THROW(75, "Error : invalid stream checksum detected");
    }

    // Free
    free(in_buff);
    free(out_buff);

    return filesize;
}


unsigned long long selectDecoder( FILE* finput,  FILE* foutput)
{
    unsigned int magicNumber, size;
    int errorNb;
    size_t nbReadBytes;

    // Check Archive Header
    nbReadBytes = fread(&magicNumber, 1, MAGICNUMBER_SIZE, finput);
    if (nbReadBytes==0) return 0;                  // EOF
    if (nbReadBytes != MAGICNUMBER_SIZE) EXM_THROW(41, "Unrecognized header : Magic Number unreadable");
    magicNumber = LITTLE_ENDIAN_32(magicNumber);   // Convert to Little Endian format
    if (LZ4S_isSkippableMagicNumber(magicNumber)) magicNumber = LZ4S_SKIPPABLE0;  // fold skippable magic numbers

    switch(magicNumber)
    {
    case LZ4S_MAGICNUMBER:
        return decodeLZ4S(finput, foutput);
    case LEGACY_MAGICNUMBER:
        DISPLAY("Detected : Legacy format \n");
        return decodeLegacyStream(finput, foutput);
    case LZ4S_SKIPPABLE0:
        nbReadBytes = fread(&size, 1, 4, finput);
        if (nbReadBytes != 4) EXM_THROW(42, "Stream error : skippable size unreadable");
        size = LITTLE_ENDIAN_32(size);     // Convert to Little Endian format
        errorNb = fseek(finput, size, SEEK_CUR);
        if (errorNb != 0) EXM_THROW(43, "Stream error : cannot skip skippable area");
        return selectDecoder(finput, foutput);
    default:
        if (ftell(finput) == MAGICNUMBER_SIZE) EXM_THROW(44,"Unrecognized header : file cannot be decoded");   // Wrong magic number at the beginning of 1st stream
        DISPLAY("Stream followed by unrecognized data\n");
        return 0;
    }
}


int decodeFile(char* input_filename, char* output_filename)
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
        filesize += decodedSize;
    } while (decodedSize);

    // Final Status
    end = clock();
    DISPLAY( "Successfully decoded %llu bytes \n", filesize);
    {
        double seconds = (double)(end - start)/CLOCKS_PER_SEC;
        DISPLAY( "Done in %.2f s ==> %.2f MB/s\n", seconds, (double)filesize / seconds / 1024 / 1024);
    }

    // Close
    fclose(finput);
    fclose(foutput);

    // Error status = OK
    return 0;
}


int main(int argc, char** argv)
{
    int i,
        cLevel=0,
        decode=0,
        bench=0,
        filenamesStart=2,
        legacy_format=0;
    char* exename=argv[0];
    char* input_filename=0;
    char* output_filename=0;
    char nullinput[] = NULL_INPUT;
    char extension[] = EXTENSION;

    // Welcome message
    DISPLAY( WELCOME_MESSAGE);

    if (argc<2) { badusage(exename); return 1; }

    for(i=1; i<argc; i++)
    {
        char* argument = argv[i];

        if(!argument) continue;   // Protection if argument empty

        // Decode command (note : aggregated commands are allowed)
        if (argument[0]=='-')
        {
            while (argument[1]!=0)
            {
                argument ++;

                switch(argument[0])
                {
                    // Display help on usage
                case 'H': usage(exename); usage_advanced(); return 0;

                    // Compression (default)
                case 'c': if ((argument[1] >='0') && (argument[1] <='1')) { cLevel=argument[1] - '0'; argument++; } break;
                case 'h': if (argument[1]=='c') { cLevel=1; argument++; } break;

                    // Use Legacy format (hidden option)
                case 'l': legacy_format=1; break;

                    // Decoding
                case 'd': decode=1; break;

                    // Test
                case 't': decode=1; output_filename=nulmark; break;

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
                        blockSizeId = B;
                        argument++;
                        break;
                    }
                    case 'D': blockIndependence = 0, argument++; break;
                    case 'X': blockChecksum = 1, argument ++; break;
                    default : goto _exit_blockProperties;
                    }
_exit_blockProperties:
                    break;

                    // Modify Stream properties
                case 'S': if (argument[1]=='x') { streamChecksum=0; argument++; break; } else { badusage(exename); return 1; }

                    // Bench
                case 'b': bench=1; 
                    if ((argument[1] >='0') && (argument[1] <='1')) { cLevel=argument[1] - '0'; argument++; } 
                    break;

                    // Modify Nb Iterations (benchmark only)
                case 'i': 
                    if ((argument[1] >='1') && (argument[1] <='9'))
                    {
                        int iters = argument[1] - '0'; 
                        BMK_SetNbIterations(iters); 
                        argument++;
                    }
                    break;

                    // Pause at the end (benchmark only) (hidden option)
                case 'p': BMK_SetPause(); break;

                    // Overwrite
                case 'y': overwrite=1; break;

                    // Unrecognised command
                default : badusage(exename); return 1;
                }
            }
            continue;
        }

        // first provided filename is input
        if (!input_filename) { input_filename=argument; filenamesStart=i; continue; }

        // second provided filename is output
        if (!output_filename)
        {
            output_filename=argument;
            if (!strcmp (output_filename, nullinput)) output_filename = nulmark;
            continue;
        }
    }

    // No input filename ==> Error
    if(!input_filename) { badusage(exename); return 1; }

    if (bench) return BMK_benchFile(argv+filenamesStart, argc-filenamesStart, cLevel);

    // No output filename ==> build one automatically (when possible)
    if (!output_filename) 
    { 
        if (!decode)   // compression
        {
            int i=0, l=0;
            while (input_filename[l]!=0) l++;
            output_filename = (char*)calloc(1,l+5);
            for (i=0;i<l;i++) output_filename[i] = input_filename[i];
            for (i=l;i<l+4;i++) output_filename[i] = extension[i-l];
        }
        else           // decompression (input file must respect format extension ".lz4")
        {
            int inl=0,outl;
            while (input_filename[inl]!=0) inl++;
            output_filename = (char*)calloc(1,inl+1);
            for (outl=0;outl<inl;outl++) output_filename[outl] = input_filename[outl];
            if (inl>4)
                while ((outl >= inl-4) && (input_filename[outl] ==  extension[outl-inl+4])) output_filename[outl--]=0;
            if (outl != inl-5) output_filename = NULL;
        }
        if (!output_filename) { badusage(exename); return 1; }
    }

    if (decode) return decodeFile(input_filename, output_filename);

    // compression is default action
    if (legacy_format)
    {
        DISPLAY("! Generating compressed LZ4 using Legacy format (deprecated !) ! \n");
        return legacy_compress_file(input_filename, output_filename, cLevel);   
    }
    else
    {
        return compress_file(input_filename, output_filename, cLevel);   
    }
}
