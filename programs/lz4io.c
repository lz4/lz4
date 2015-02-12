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

/**************************************
*  Compiler Options
***************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  define _CRT_SECURE_NO_WARNINGS
#  define _CRT_SECURE_NO_DEPRECATE     /* VS2005 */
#  pragma warning(disable : 4127)      /* disable: C4127: conditional expression is constant */
#endif

#define GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)
#ifdef __GNUC__
#  pragma GCC diagnostic ignored "-Wmissing-braces"   /* GCC bug 53119 : doesn't accept { 0 } as initializer (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53119) */
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"   /* GCC bug 53119 : doesn't accept { 0 } as initializer (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53119) */
#endif

#define _LARGE_FILES           /* Large file support on 32-bits AIX */
#define _FILE_OFFSET_BITS 64   /* Large file support on 32-bits unix */
#define _POSIX_SOURCE 1        /* for fileno() within <stdio.h> on unix */


/****************************
*  Includes
*****************************/
#include <stdio.h>    /* fprintf, fopen, fread, _fileno, stdin, stdout */
#include <stdlib.h>   /* malloc, free */
#include <string.h>   /* strcmp, strlen */
#include <time.h>     /* clock */
#include "lz4io.h"
#include "lz4.h"      /* still required for legacy format */
#include "lz4hc.h"    /* still required for legacy format */
#include "lz4frame.h"


/****************************
*  OS-specific Includes
*****************************/
#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(_WIN32) || defined(__CYGWIN__)
#  include <fcntl.h>   /* _O_BINARY */
#  include <io.h>      /* _setmode, _isatty */
#  ifdef __MINGW32__
   int _fileno(FILE *stream);   /* MINGW somehow forgets to include this windows declaration into <stdio.h> */
#  endif
#  if defined(_MSC_VER) && (_MSC_VER >= 1400)  /* Avoid MSVC fseek()'s 2GiB barrier */
#    define fseek _fseeki64
#  endif
#  define SET_BINARY_MODE(file) _setmode(_fileno(file), _O_BINARY)
#  define IS_CONSOLE(stdStream) _isatty(_fileno(stdStream))
#  if defined(LZ4IO_ENABLE_SPARSE_FILE)
#    include <windows.h>
#    define SET_SPARSE_FILE_MODE(file) do { DWORD dw; DeviceIoControl((HANDLE) _get_osfhandle(_fileno(file)), FSCTL_SET_SPARSE, 0, 0, 0, 0, &dw, 0); } while(0)
#    if defined(_MSC_VER) && (_MSC_VER >= 1400)
#      define fseek _fseeki64
#    endif
#  endif /* LZ4IO_ENABLE_SPARSE_FILE */
#else
#  include <unistd.h>  /* isatty */
#  define SET_BINARY_MODE(file)
#  define IS_CONSOLE(stdStream) isatty(fileno(stdStream))
#  if defined(LZ4IO_ENABLE_SPARSE_FILE)
#    define SET_SPARSE_FILE_MODE(file)
#  endif /* LZ4IO_ENABLE_SPARSE_FILE */
#endif


/****************************
*  Constants
*****************************/
#define KB *(1 <<10)
#define MB *(1 <<20)
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


/**************************************
*  Macros
***************************************/
#define DISPLAY(...)         fprintf(stderr, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...) if (displayLevel>=l) { DISPLAY(__VA_ARGS__); }
#define DISPLAYUPDATE(l, ...) if (displayLevel>=l) { \
            if ((LZ4IO_GetMilliSpan(g_time) > refreshRate) || (displayLevel>=4)) \
            { g_time = clock(); DISPLAY(__VA_ARGS__); \
            if (displayLevel>=4) fflush(stdout); } }
static const unsigned refreshRate = 150;
static clock_t g_time = 0;


/**************************************
*  Local Parameters
***************************************/
static int displayLevel = 0;   /* 0 : no display  ; 1: errors  ; 2 : + result + interaction + warnings ; 3 : + progression; 4 : + information */
static int overwrite = 1;
static int globalBlockSizeId = LZ4S_BLOCKSIZEID_DEFAULT;
static int blockChecksum = 0;
static int streamChecksum = 1;
static int blockIndependence = 1;
#if defined(LZ4IO_ENABLE_SPARSE_FILE)
static int sparseFile = 0;
#endif /* LZ4IO_ENABLE_SPARSE_FILE */

static const int minBlockSizeID = 4;
static const int maxBlockSizeID = 7;


/**************************************
*  Exceptions
***************************************/
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


/**************************************
*  Version modifiers
***************************************/
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

#if defined(LZ4IO_ENABLE_SPARSE_FILE)
/* Default setting : sparseFile = 0; (Disable)
   return : sparse file mode (0:Disable / 1:Enable) */
int LZ4IO_setSparseFile(int yes)
{
    sparseFile = yes;
    return sparseFile;
}

static int isSparse(const void* p, size_t size)
{
    const char* p8 = p;
    for(; size; --size)
    {
        if(*p8 != 0)
        {
            return 0;
        }
        ++p8;
    }
    return 1;
}
#endif /* LZ4IO_ENABLE_SPARSE_FILE */

/* blockSizeID : valid values : 4-5-6-7 */
int LZ4IO_setBlockSizeID(int bsid)
{
    static const int blockSizeTable[] = { 64 KB, 256 KB, 1 MB, 4 MB };
    if ((bsid < minBlockSizeID) || (bsid > maxBlockSizeID)) return -1;
    globalBlockSizeId = bsid;
    return blockSizeTable[globalBlockSizeId-minBlockSizeID];
}

int LZ4IO_setBlockMode(LZ4IO_blockMode_t blockMode)
{
    blockIndependence = (blockMode == LZ4IO_blockIndependent);
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

static unsigned LZ4IO_GetMilliSpan(clock_t nPrevious)
{
    clock_t nCurrent = clock();
    unsigned nSpan = (unsigned)(((nCurrent - nPrevious) * 1000) / CLOCKS_PER_SEC);
    return nSpan;
}


/* ************************************************************************ */
/* ********************** LZ4 File / Pipe compression ********************* */
/* ************************************************************************ */

static int          LZ4S_GetBlockSize_FromBlockId (int id) { return (1 << (8 + (2 * id))); }
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
        /* Check if destination file already exists */
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
                if (displayLevel <= 1) EXM_THROW(11, "Operation aborted : %s already exists", output_filename);   /* No interaction possible */
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




/***************************************
 *   Legacy Compression
 * *************************************/

/* unoptimized version; solves endianess & alignment issues */
static void LZ4IO_writeLE32 (void* p, unsigned value32)
{
    unsigned char* dstPtr = p;
    dstPtr[0] = (unsigned char)value32;
    dstPtr[1] = (unsigned char)(value32 >> 8);
    dstPtr[2] = (unsigned char)(value32 >> 16);
    dstPtr[3] = (unsigned char)(value32 >> 24);
}

/* LZ4IO_compressFilename_Legacy :
 * This function is intentionally "hidden" (not published in .h)
 * It generates compressed streams using the old 'legacy' format */
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


    /* Init */
    start = clock();
    if (compressionlevel < 3) compressionFunction = LZ4_compress; else compressionFunction = LZ4_compressHC;

    get_fileHandle(input_filename, output_filename, &finput, &foutput);
    if ((displayLevel==2) && (compressionlevel==1)) displayLevel=3;

    /* Allocate Memory */
    in_buff = (char*)malloc(LEGACY_BLOCKSIZE);
    out_buff = (char*)malloc(LZ4_compressBound(LEGACY_BLOCKSIZE));
    if (!in_buff || !out_buff) EXM_THROW(21, "Allocation error : not enough memory");

    /* Write Archive Header */
    LZ4IO_writeLE32(out_buff, LEGACY_MAGICNUMBER);
    sizeCheck = fwrite(out_buff, 1, MAGICNUMBER_SIZE, foutput);
    if (sizeCheck!=MAGICNUMBER_SIZE) EXM_THROW(22, "Write error : cannot write header");

    /* Main Loop */
    while (1)
    {
        unsigned int outSize;
        /* Read Block */
        int inSize = (int) fread(in_buff, (size_t)1, (size_t)LEGACY_BLOCKSIZE, finput);
        if( inSize<=0 ) break;
        filesize += inSize;

        /* Compress Block */
        outSize = compressionFunction(in_buff, out_buff+4, inSize);
        compressedfilesize += outSize+4;
        DISPLAYUPDATE(3, "\rRead : %i MB  ==> %.2f%%   ", (int)(filesize>>20), (double)compressedfilesize/filesize*100);

        /* Write Block */
        LZ4IO_writeLE32(out_buff, outSize);
        sizeCheck = fwrite(out_buff, 1, outSize+4, foutput);
        if (sizeCheck!=(size_t)(outSize+4)) EXM_THROW(23, "Write error : cannot write compressed block");
    }

    /* Status */
    end = clock();
    DISPLAYLEVEL(2, "\r%79s\r", "");
    DISPLAYLEVEL(2,"Compressed %llu bytes into %llu bytes ==> %.2f%%\n",
        (unsigned long long) filesize, (unsigned long long) compressedfilesize, (double)compressedfilesize/filesize*100);
    {
        double seconds = (double)(end - start)/CLOCKS_PER_SEC;
        DISPLAYLEVEL(4,"Done in %.2f s ==> %.2f MB/s\n", seconds, (double)filesize / seconds / 1024 / 1024);
    }

    /* Close & Free */
    free(in_buff);
    free(out_buff);
    fclose(finput);
    fclose(foutput);

    return 0;
}


/***********************************************
 *   Compression using Frame format
 * ********************************************/

int LZ4IO_compressFilename(char* input_filename, char* output_filename, int compressionLevel)
{
    unsigned long long filesize = 0;
    unsigned long long compressedfilesize = 0;
    char* in_buff;
    char* out_buff;
    FILE* finput;
    FILE* foutput;
    clock_t start, end;
    int blockSize;
    size_t sizeCheck, headerSize, readSize, outBuffSize;
    LZ4F_compressionContext_t ctx;
    LZ4F_errorCode_t errorCode;
    LZ4F_preferences_t prefs = {0};


    /* Init */
    start = clock();
    if ((displayLevel==2) && (compressionLevel>=3)) displayLevel=3;
    errorCode = LZ4F_createCompressionContext(&ctx, LZ4F_VERSION);
    if (LZ4F_isError(errorCode)) EXM_THROW(30, "Allocation error : can't create LZ4F context : %s", LZ4F_getErrorName(errorCode));
    get_fileHandle(input_filename, output_filename, &finput, &foutput);
    blockSize = LZ4S_GetBlockSize_FromBlockId (globalBlockSizeId);

    /* Set compression parameters */
    prefs.autoFlush = 1;
    prefs.compressionLevel = compressionLevel;
    prefs.frameInfo.blockMode = blockIndependence;
    prefs.frameInfo.blockSizeID = globalBlockSizeId;
    prefs.frameInfo.contentChecksumFlag = streamChecksum;

    /* Allocate Memory */
    in_buff  = (char*)malloc(blockSize);
    outBuffSize = LZ4F_compressBound(blockSize, &prefs);
    out_buff = (char*)malloc(outBuffSize);
    if (!in_buff || !out_buff) EXM_THROW(31, "Allocation error : not enough memory");

    /* Write Archive Header */
    headerSize = LZ4F_compressBegin(ctx, out_buff, outBuffSize, &prefs);
    if (LZ4F_isError(headerSize)) EXM_THROW(32, "File header generation failed : %s", LZ4F_getErrorName(headerSize));
    sizeCheck = fwrite(out_buff, 1, headerSize, foutput);
    if (sizeCheck!=headerSize) EXM_THROW(33, "Write error : cannot write header");
    compressedfilesize += headerSize;

    /* read first block */
    readSize = fread(in_buff, (size_t)1, (size_t)blockSize, finput);
    filesize += readSize;

    /* Main Loop */
    while (readSize>0)
    {
        size_t outSize;

        /* Compress Block */
        outSize = LZ4F_compressUpdate(ctx, out_buff, outBuffSize, in_buff, readSize, NULL);
        if (LZ4F_isError(outSize)) EXM_THROW(34, "Compression failed : %s", LZ4F_getErrorName(outSize));
        compressedfilesize += outSize;
        DISPLAYUPDATE(3, "\rRead : %i MB   ==> %.2f%%   ", (int)(filesize>>20), (double)compressedfilesize/filesize*100);

        /* Write Block */
        sizeCheck = fwrite(out_buff, 1, outSize, foutput);
        if (sizeCheck!=outSize) EXM_THROW(35, "Write error : cannot write compressed block");

        /* Read next block */
        readSize = fread(in_buff, (size_t)1, (size_t)blockSize, finput);
        filesize += readSize;
    }

    /* End of Stream mark */
    headerSize = LZ4F_compressEnd(ctx, out_buff, outBuffSize, NULL);
    if (LZ4F_isError(headerSize)) EXM_THROW(36, "End of file generation failed : %s", LZ4F_getErrorName(headerSize));

    sizeCheck = fwrite(out_buff, 1, headerSize, foutput);
    if (sizeCheck!=headerSize) EXM_THROW(37, "Write error : cannot write end of stream");
    compressedfilesize += headerSize;

    /* Close & Free */
    free(in_buff);
    free(out_buff);
    fclose(finput);
    fclose(foutput);
    errorCode = LZ4F_freeCompressionContext(ctx);
    if (LZ4F_isError(errorCode)) EXM_THROW(38, "Error : can't free LZ4F context resource : %s", LZ4F_getErrorName(errorCode));

    /* Final Status */
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

static unsigned LZ4IO_readLE32 (const void* s)
{
    const unsigned char* srcPtr = s;
    unsigned value32 = srcPtr[0];
    value32 += (srcPtr[1]<<8);
    value32 += (srcPtr[2]<<16);
    value32 += (srcPtr[3]<<24);
    return value32;
}

static unsigned long long decodeLegacyStream(FILE* finput, FILE* foutput)
{
    unsigned long long filesize = 0;
    char* in_buff;
    char* out_buff;

    /* Allocate Memory */
    in_buff = (char*)malloc(LZ4_compressBound(LEGACY_BLOCKSIZE));
    out_buff = (char*)malloc(LEGACY_BLOCKSIZE);
    if (!in_buff || !out_buff) EXM_THROW(51, "Allocation error : not enough memory");

    /* Main Loop */
    while (1)
    {
        int decodeSize;
        size_t sizeCheck;
        unsigned int blockSize;

        /* Block Size */
        sizeCheck = fread(in_buff, 1, 4, finput);
        if (sizeCheck==0) break;                   /* Nothing to read : file read is completed */
        blockSize = LZ4IO_readLE32(in_buff);       /* Convert to Little Endian */
        if (blockSize > LZ4_COMPRESSBOUND(LEGACY_BLOCKSIZE))
        {   /* Cannot read next block : maybe new stream ? */
            fseek(finput, -4, SEEK_CUR);
            break;
        }

        /* Read Block */
        sizeCheck = fread(in_buff, 1, blockSize, finput);

        /* Decode Block */
        decodeSize = LZ4_decompress_safe(in_buff, out_buff, blockSize, LEGACY_BLOCKSIZE);
        if (decodeSize < 0) EXM_THROW(52, "Decoding Failed ! Corrupted input detected !");
        filesize += decodeSize;

        /* Write Block */
        sizeCheck = fwrite(out_buff, 1, decodeSize, foutput);
        if (sizeCheck != (size_t)decodeSize) EXM_THROW(53, "Write error : cannot write decoded block into output\n");
    }

    /* Free */
    free(in_buff);
    free(out_buff);

    return filesize;
}


static unsigned long long decodeLZ4S(FILE* finput, FILE* foutput)
{
    unsigned long long filesize = 0;
    char* inBuff;
    char* outBuff;
#   define HEADERMAX 20
    char  headerBuff[HEADERMAX];
    size_t sizeCheck, nextToRead, outBuffSize, inBuffSize;
    LZ4F_decompressionContext_t ctx;
    LZ4F_errorCode_t errorCode;
    LZ4F_frameInfo_t frameInfo;
#if defined(LZ4IO_ENABLE_SPARSE_FILE)
    size_t sparsePending = 0;
#endif /* LZ4IO_ENABLE_SPARSE_FILE */

    /* init */
    errorCode = LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION);
    if (LZ4F_isError(errorCode)) EXM_THROW(60, "Allocation error : can't create context : %s", LZ4F_getErrorName(errorCode));
    LZ4IO_writeLE32(headerBuff, LZ4S_MAGICNUMBER);   /* regenerated here, as it was already read from finput */

    /* Decode stream descriptor */
    outBuffSize = 0; inBuffSize = 0; sizeCheck = MAGICNUMBER_SIZE;
    nextToRead = LZ4F_decompress(ctx, NULL, &outBuffSize, headerBuff, &sizeCheck, NULL);
    if (LZ4F_isError(nextToRead)) EXM_THROW(61, "Decompression error : %s", LZ4F_getErrorName(nextToRead));
    if (nextToRead > HEADERMAX) EXM_THROW(62, "Header too large (%i>%i)", (int)nextToRead, HEADERMAX);
    sizeCheck = fread(headerBuff, 1, nextToRead, finput);
    if (sizeCheck!=nextToRead) EXM_THROW(63, "Read error ");
    nextToRead = LZ4F_decompress(ctx, NULL, &outBuffSize, headerBuff, &sizeCheck, NULL);
    errorCode = LZ4F_getFrameInfo(ctx, &frameInfo, NULL, &inBuffSize);
    if (LZ4F_isError(errorCode)) EXM_THROW(64, "can't decode frame header : %s", LZ4F_getErrorName(errorCode));

    /* Allocate Memory */
    outBuffSize = LZ4IO_setBlockSizeID(frameInfo.blockSizeID);
    inBuffSize = outBuffSize + 4;
    inBuff = (char*)malloc(inBuffSize);
    outBuff = (char*)malloc(outBuffSize);
    if (!inBuff || !outBuff) EXM_THROW(65, "Allocation error : not enough memory");

    /* Main Loop */
    while (nextToRead != 0)
    {
        size_t decodedBytes = outBuffSize;

        /* Read Block */
        sizeCheck = fread(inBuff, 1, nextToRead, finput);
        if (sizeCheck!=nextToRead) EXM_THROW(66, "Read error ");

        /* Decode Block */
        errorCode = LZ4F_decompress(ctx, outBuff, &decodedBytes, inBuff, &sizeCheck, NULL);
        if (LZ4F_isError(errorCode)) EXM_THROW(67, "Decompression error : %s", LZ4F_getErrorName(errorCode));
        if (sizeCheck!=nextToRead) EXM_THROW(67, "Synchronization error");
        nextToRead = errorCode;
        filesize += decodedBytes;

        /* Write Block */
#if defined(LZ4IO_ENABLE_SPARSE_FILE)
        if(sparseFile)
        {
            if(isSparse(outBuff, decodedBytes))
            {
                sparsePending += decodedBytes;
                continue;
            }
            if(sparsePending > 0)
            {
                fseek(foutput, sparsePending, SEEK_CUR);
                sparsePending = 0;
            }
        }
#endif /* LZ4IO_ENABLE_SPARSE_FILE */
        sizeCheck = fwrite(outBuff, 1, decodedBytes, foutput);
        if (sizeCheck != decodedBytes) EXM_THROW(68, "Write error : cannot write decoded block\n");
    }
#if defined(LZ4IO_ENABLE_SPARSE_FILE)
    if(sparseFile)
    {
        if(sparsePending > 0)
        {
            fseek(foutput, sparsePending-1, SEEK_CUR);
            fputc(0, foutput);
            sparsePending = 0;
        }
    }
#endif /* LZ4IO_ENABLE_SPARSE_FILE */

    /* Free */
    free(inBuff);
    free(outBuff);
    errorCode = LZ4F_freeDecompressionContext(ctx);
    if (LZ4F_isError(errorCode)) EXM_THROW(69, "Error : can't free LZ4F context resource : %s", LZ4F_getErrorName(errorCode));

    return filesize;
}


#define ENDOFSTREAM ((unsigned long long)-1)
static unsigned long long selectDecoder( FILE* finput,  FILE* foutput)
{
    unsigned char U32store[MAGICNUMBER_SIZE];
    unsigned magicNumber, size;
    int errorNb;
    size_t nbReadBytes;

    /* Check Archive Header */
    nbReadBytes = fread(U32store, 1, MAGICNUMBER_SIZE, finput);
    if (nbReadBytes==0) return ENDOFSTREAM;                  /* EOF */
    if (nbReadBytes != MAGICNUMBER_SIZE) EXM_THROW(40, "Unrecognized header : Magic Number unreadable");
    magicNumber = LZ4IO_readLE32(U32store);   /* Little Endian format */
    if (LZ4S_isSkippableMagicNumber(magicNumber)) magicNumber = LZ4S_SKIPPABLE0;  /* fold skippable magic numbers */

    switch(magicNumber)
    {
    case LZ4S_MAGICNUMBER:
        return DEFAULT_DECOMPRESSOR(finput, foutput);
    case LEGACY_MAGICNUMBER:
        DISPLAYLEVEL(4, "Detected : Legacy format \n");
        return decodeLegacyStream(finput, foutput);
    case LZ4S_SKIPPABLE0:
        DISPLAYLEVEL(4, "Skipping detected skippable area \n");
        nbReadBytes = fread(U32store, 1, 4, finput);
        if (nbReadBytes != 4) EXM_THROW(42, "Stream error : skippable size unreadable");
        size = LZ4IO_readLE32(U32store);     /* Little Endian format */
        errorNb = fseek(finput, size, SEEK_CUR);
        if (errorNb != 0) EXM_THROW(43, "Stream error : cannot skip skippable area");
        return selectDecoder(finput, foutput);
    EXTENDED_FORMAT;
    default:
        if (ftell(finput) == MAGICNUMBER_SIZE) EXM_THROW(44,"Unrecognized header : file cannot be decoded");   /* Wrong magic number at the beginning of 1st stream */
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


    /* Init */
    start = clock();
    get_fileHandle(input_filename, output_filename, &finput, &foutput);

#if defined(LZ4IO_ENABLE_SPARSE_FILE)
    if (sparseFile!=0 && foutput!=0)
    {
        DISPLAY("Experimental : Using sparse file\n");
        SET_SPARSE_FILE_MODE(foutput);
    }
#endif /* LZ4IO_ENABLE_SPARSE_FILE */

    /* Loop over multiple streams */
    do
    {
        decodedSize = selectDecoder(finput, foutput);
        if (decodedSize != ENDOFSTREAM)
            filesize += decodedSize;
    } while (decodedSize != ENDOFSTREAM);

    /* Final Status */
    end = clock();
    DISPLAYLEVEL(2, "\r%79s\r", "");
    DISPLAYLEVEL(2, "Successfully decoded %llu bytes \n", filesize);
    {
        double seconds = (double)(end - start)/CLOCKS_PER_SEC;
        DISPLAYLEVEL(4, "Done in %.2f s ==> %.2f MB/s\n", seconds, (double)filesize / seconds / 1024 / 1024);
    }

    /* Close */
    fclose(finput);
    fclose(foutput);

    /*  Error status = OK */
    return 0;
}

