/*
    frameTest - test tool for lz4frame
    Copyright (C) Yann Collet 2014
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

/**************************************
  Compiler specific
**************************************/
#define _CRT_SECURE_NO_WARNINGS   // fgets
#ifdef _MSC_VER    /* Visual Studio */
#  pragma warning(disable : 4127)        /* disable: C4127: conditional expression is constant */
#  pragma warning(disable : 4146)        /* disable: C4146: minus unsigned expression */
#endif
#define GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)
#ifdef __GNUC__
#  pragma GCC diagnostic ignored "-Wmissing-braces"   /* GCC bug 53119 : doesn't accept { 0 } as initializer (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53119) */
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"   /* GCC bug 53119 : doesn't accept { 0 } as initializer (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53119) */
#endif


/**************************************
 Includes
**************************************/
#include <stdlib.h>
#include <stdio.h>      // fgets, sscanf
#include <sys/timeb.h>  // timeb
#include <string.h>     // strcmp
#include "lz4frame.h"
#include "xxhash.h"     // XXH64


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
#  define LZ4_VERSION ""
#endif

#define KB *(1U<<10)
#define MB *(1U<<20)
#define GB *(1U<<30)

static const U32 nbTestsDefault = 128 KB;
#define COMPRESSIBLE_NOISE_LENGTH (2 MB)
#define FUZ_COMPRESSIBILITY_DEFAULT 50
static const U32 prime1 = 2654435761U;
static const U32 prime2 = 2246822519U;



/**************************************
  Macros
**************************************/
#define DISPLAY(...)          fprintf(stderr, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...)  if (displayLevel>=l) { DISPLAY(__VA_ARGS__); }
#define DISPLAYUPDATE(l, ...) if (displayLevel>=l) { \
            if ((FUZ_GetMilliSpan(g_time) > refreshRate) || (displayLevel>=4)) \
            { g_time = FUZ_GetMilliStart(); DISPLAY(__VA_ARGS__); \
            if (displayLevel>=4) fflush(stdout); } }
static const U32 refreshRate = 150;
static U32 g_time = 0;


/*****************************************
  Local Parameters
*****************************************/
static U32 no_prompt = 0;
static char* programName;
static U32 displayLevel = 2;


/*********************************************************
  Fuzzer functions
*********************************************************/
static U32 FUZ_GetMilliStart(void)
{
   struct timeb tb;
   U32 nCount;
   ftime( &tb );
   nCount = (U32) (((tb.time & 0xFFFFF) * 1000) +  tb.millitm);
   return nCount;
}


static U32 FUZ_GetMilliSpan(U32 nTimeStart)
{
   U32 nCurrent = FUZ_GetMilliStart();
   U32 nSpan = nCurrent - nTimeStart;
   if (nTimeStart > nCurrent)
      nSpan += 0x100000 * 1000;
   return nSpan;
}



#  define FUZ_rotl32(x,r) ((x << r) | (x >> (32 - r)))
unsigned int FUZ_rand(unsigned int* src)
{
    U32 rand32 = *src;
    rand32 *= prime1;
    rand32 += prime2;
    rand32  = FUZ_rotl32(rand32, 13);
    *src = rand32;
    return rand32 >> 5;
}


#define FUZ_RAND15BITS  (FUZ_rand(seed) & 0x7FFF)
#define FUZ_RANDLENGTH  ( (FUZ_rand(seed) & 3) ? (FUZ_rand(seed) % 15) : (FUZ_rand(seed) % 510) + 15)
static void FUZ_fillCompressibleNoiseBuffer(void* buffer, unsigned bufferSize, double proba, U32* seed)
{
    BYTE* BBuffer = (BYTE*)buffer;
    unsigned pos = 0;
    U32 P32 = (U32)(32768 * proba);

    // First Byte
    BBuffer[pos++] = (BYTE)(FUZ_rand(seed));

    while (pos < bufferSize)
    {
        // Select : Literal (noise) or copy (within 64K)
        if (FUZ_RAND15BITS < P32)
        {
            // Copy (within 64K)
            unsigned match, end;
            unsigned length = FUZ_RANDLENGTH + 4;
            unsigned offset = FUZ_RAND15BITS + 1;
            if (offset > pos) offset = pos;
            if (pos + length > bufferSize) length = bufferSize - pos;
            match = pos - offset;
            end = pos + length;
            while (pos < end) BBuffer[pos++] = BBuffer[match++];
        }
        else
        {
            // Literal (noise)
            unsigned end;
            unsigned length = FUZ_RANDLENGTH;
            if (pos + length > bufferSize) length = bufferSize - pos;
            end = pos + length;
            while (pos < end) BBuffer[pos++] = (BYTE)(FUZ_rand(seed) >> 5);
        }
    }
}


static unsigned FUZ_highbit(U32 v32)
{
    unsigned nbBits = 0;
    if (v32==0) return 0;
    while (v32) { v32 >>= 1; nbBits ++; }
    return nbBits;
}


int basicTests(U32 seed, double compressibility)
{
	int testResult = 0;
	void* CNBuffer;
	void* compressedBuffer;
	void* decodedBuffer;
	U32 randState = seed;
	size_t cSize, testSize;
	LZ4F_preferences_t prefs = { 0 };
	LZ4F_decompressionContext_t dCtx;
	U64 crcOrig;

	// Create compressible test buffer
	CNBuffer = malloc(COMPRESSIBLE_NOISE_LENGTH);
	compressedBuffer = malloc(LZ4F_compressFrameBound(COMPRESSIBLE_NOISE_LENGTH, NULL));
	decodedBuffer = malloc(COMPRESSIBLE_NOISE_LENGTH);
	FUZ_fillCompressibleNoiseBuffer(CNBuffer, COMPRESSIBLE_NOISE_LENGTH, compressibility, &randState);
	crcOrig = XXH64(CNBuffer, COMPRESSIBLE_NOISE_LENGTH, 1);

	// Trivial tests : one-step frame
	testSize = COMPRESSIBLE_NOISE_LENGTH;
	DISPLAYLEVEL(3, "Using NULL preferences : \n");
	cSize = LZ4F_compressFrame(compressedBuffer, LZ4F_compressFrameBound(testSize, NULL), CNBuffer, testSize, NULL);
	if (LZ4F_isError(cSize)) goto _output_error;
	DISPLAYLEVEL(3, "Compressed %i bytes into a %i bytes frame \n", (int)testSize, (int)cSize);

	DISPLAYLEVEL(3, "Decompression test : \n");
	{
        size_t decodedBufferSize = COMPRESSIBLE_NOISE_LENGTH;
        size_t compressedBufferSize = cSize;
        BYTE* op = (BYTE*)decodedBuffer;
        BYTE* const oend = (BYTE*)decodedBuffer + COMPRESSIBLE_NOISE_LENGTH;
        BYTE* ip = (BYTE*)compressedBuffer;
        BYTE* const iend = (BYTE*)compressedBuffer + cSize;
        U64 crcDest;

        LZ4F_errorCode_t errorCode = LZ4F_createDecompressionContext(&dCtx, LZ4F_VERSION);
        if (LZ4F_isError(errorCode)) goto _output_error;

        DISPLAYLEVEL(3, "Single Block : \n");
        errorCode = LZ4F_decompress(dCtx, decodedBuffer, &decodedBufferSize, compressedBuffer, &compressedBufferSize, NULL);
        crcDest = XXH64(decodedBuffer, COMPRESSIBLE_NOISE_LENGTH, 1);
        if (crcDest != crcOrig) goto _output_error;
        DISPLAYLEVEL(3, "Regenerated %i bytes \n", (int)decodedBufferSize);

        DISPLAYLEVEL(3, "Byte after byte : \n");
        while (ip < iend)
        {
            size_t oSize = oend-op;
            size_t iSize = 1;
            //DISPLAY("%7i \n", (int)(ip-(BYTE*)compressedBuffer));
            errorCode = LZ4F_decompress(dCtx, op, &oSize, ip, &iSize, NULL);
            if (LZ4F_isError(errorCode)) goto _output_error;
            op += oSize;
            ip += iSize;
        }
        crcDest = XXH64(decodedBuffer, COMPRESSIBLE_NOISE_LENGTH, 1);
        if (crcDest != crcOrig) goto _output_error;
        DISPLAYLEVEL(3, "Regenerated %i bytes \n", (int)decodedBufferSize);

        errorCode = LZ4F_freeDecompressionContext(dCtx);
        if (LZ4F_isError(errorCode)) goto _output_error;
	}

	DISPLAYLEVEL(3, "Using 64 KB block : \n");
	prefs.frameInfo.blockSizeID = max64KB;
	prefs.frameInfo.contentChecksumFlag = contentChecksumEnabled;
	cSize = LZ4F_compressFrame(compressedBuffer, LZ4F_compressFrameBound(testSize, &prefs), CNBuffer, testSize, &prefs);
	if (LZ4F_isError(cSize)) goto _output_error;
	DISPLAYLEVEL(3, "Compressed %i bytes into a %i bytes frame \n", (int)testSize, (int)cSize);

	DISPLAYLEVEL(3, "without checksum : \n");
	prefs.frameInfo.contentChecksumFlag = noContentChecksum;
	cSize = LZ4F_compressFrame(compressedBuffer, LZ4F_compressFrameBound(testSize, &prefs), CNBuffer, testSize, &prefs);
	if (LZ4F_isError(cSize)) goto _output_error;
	DISPLAYLEVEL(3, "Compressed %i bytes into a %i bytes frame \n", (int)testSize, (int)cSize);

	DISPLAYLEVEL(3, "Using 256 KB block : \n");
	prefs.frameInfo.blockSizeID = max256KB;
	prefs.frameInfo.contentChecksumFlag = contentChecksumEnabled;
	cSize = LZ4F_compressFrame(compressedBuffer, LZ4F_compressFrameBound(testSize, &prefs), CNBuffer, testSize, &prefs);
	if (LZ4F_isError(cSize)) goto _output_error;
	DISPLAYLEVEL(3, "Compressed %i bytes into a %i bytes frame \n", (int)testSize, (int)cSize);

	DISPLAYLEVEL(3, "Decompression test : \n");
	{
        size_t decodedBufferSize = COMPRESSIBLE_NOISE_LENGTH;
        unsigned maxBits = FUZ_highbit(decodedBufferSize);
        BYTE* op = (BYTE*)decodedBuffer;
        BYTE* const oend = (BYTE*)decodedBuffer + COMPRESSIBLE_NOISE_LENGTH;
        BYTE* ip = (BYTE*)compressedBuffer;
        BYTE* const iend = (BYTE*)compressedBuffer + cSize;
        U64 crcDest;

        LZ4F_errorCode_t errorCode = LZ4F_createDecompressionContext(&dCtx, LZ4F_VERSION);
        if (LZ4F_isError(errorCode)) goto _output_error;

        DISPLAYLEVEL(3, "random segment sizes : \n");
        while (ip < iend)
        {
            unsigned nbBits = FUZ_rand(&randState) % maxBits;
            size_t iSize = (FUZ_rand(&randState) & ((1<<nbBits)-1)) + 1;
            size_t oSize = oend-op;
            if (iSize > (size_t)(iend-ip)) iSize = iend-ip;
            //DISPLAY("%7i : + %6i\n", (int)(ip-(BYTE*)compressedBuffer), (int)iSize);
            errorCode = LZ4F_decompress(dCtx, op, &oSize, ip, &iSize, NULL);
            if (LZ4F_isError(errorCode)) goto _output_error;
            op += oSize;
            ip += iSize;
        }
        crcDest = XXH64(decodedBuffer, COMPRESSIBLE_NOISE_LENGTH, 1);
        if (crcDest != crcOrig) goto _output_error;
        DISPLAYLEVEL(3, "Regenerated %i bytes \n", (int)decodedBufferSize);

        errorCode = LZ4F_freeDecompressionContext(dCtx);
        if (LZ4F_isError(errorCode)) goto _output_error;
	}

	DISPLAYLEVEL(3, "without checksum : \n");
	prefs.frameInfo.contentChecksumFlag = noContentChecksum;
	cSize = LZ4F_compressFrame(compressedBuffer, LZ4F_compressFrameBound(testSize, &prefs), CNBuffer, testSize, &prefs);
	if (LZ4F_isError(cSize)) goto _output_error;
	DISPLAYLEVEL(3, "Compressed %i bytes into a %i bytes frame \n", (int)testSize, (int)cSize);

	DISPLAYLEVEL(3, "Using 1 MB block : \n");
	prefs.frameInfo.blockSizeID = max1MB;
	prefs.frameInfo.contentChecksumFlag = contentChecksumEnabled;
	cSize = LZ4F_compressFrame(compressedBuffer, LZ4F_compressFrameBound(testSize, &prefs), CNBuffer, testSize, &prefs);
	if (LZ4F_isError(cSize)) goto _output_error;
	DISPLAYLEVEL(3, "Compressed %i bytes into a %i bytes frame \n", (int)testSize, (int)cSize);

	DISPLAYLEVEL(3, "without checksum : \n");
	prefs.frameInfo.contentChecksumFlag = noContentChecksum;
	cSize = LZ4F_compressFrame(compressedBuffer, LZ4F_compressFrameBound(testSize, &prefs), CNBuffer, testSize, &prefs);
	if (LZ4F_isError(cSize)) goto _output_error;
	DISPLAYLEVEL(3, "Compressed %i bytes into a %i bytes frame \n", (int)testSize, (int)cSize);

	DISPLAYLEVEL(3, "Using 4 MB block : \n");
	prefs.frameInfo.blockSizeID = max4MB;
	prefs.frameInfo.contentChecksumFlag = contentChecksumEnabled;
	cSize = LZ4F_compressFrame(compressedBuffer, LZ4F_compressFrameBound(testSize, &prefs), CNBuffer, testSize, &prefs);
	if (LZ4F_isError(cSize)) goto _output_error;
	DISPLAYLEVEL(3, "Compressed %i bytes into a %i bytes frame \n", (int)testSize, (int)cSize);

	DISPLAYLEVEL(3, "without checksum : \n");
	prefs.frameInfo.contentChecksumFlag = noContentChecksum;
	cSize = LZ4F_compressFrame(compressedBuffer, LZ4F_compressFrameBound(testSize, &prefs), CNBuffer, testSize, &prefs);
	if (LZ4F_isError(cSize)) goto _output_error;
	DISPLAYLEVEL(3, "Compressed %i bytes into a %i bytes frame \n", (int)testSize, (int)cSize);

	DISPLAY("Basic tests completed \n");
_end:
	free(CNBuffer);
	free(compressedBuffer);
	free(decodedBuffer);
	return testResult;

_output_error:
	testResult = 1;
	DISPLAY("Error detected ! \n");
	goto _end;
}


static void locateBuffDiff(const void* buff1, const void* buff2, size_t size)
{
    int p=0;
    BYTE* b1=(BYTE*)buff1;
    BYTE* b2=(BYTE*)buff2;
    while (b1[p]==b2[p]) p++;
    printf("Error at pos %i/%i : %02X != %02X \n", p, (int)size, b1[p], b2[p]);
 }


static const U32 srcDataLength = 9 MB;  /* needs to be > 2x4MB to test large blocks */

int fuzzerTests(U32 seed, unsigned nbTests, unsigned startTest, double compressibility)
{
	unsigned testResult = 0;
	unsigned testNb = 0;
	void* srcBuffer = NULL;
	void* compressedBuffer = NULL;
	void* decodedBuffer = NULL;
	U32 coreRand = seed;
	LZ4F_decompressionContext_t dCtx;
	LZ4F_compressionContext_t cCtx;
    size_t result;
#   define CHECK(cond, ...) if (cond) { DISPLAY("Error => "); DISPLAY(__VA_ARGS__); \
                            DISPLAY(" (seed %u, test nb %i)  \n", seed, testNb); goto _output_error; }

	// Create buffers
	result = LZ4F_createDecompressionContext(&dCtx, LZ4F_VERSION);
    CHECK(LZ4F_isError(result), "Allocation failed (error %i)", (int)result);
	result = LZ4F_createCompressionContext(&cCtx, LZ4F_VERSION);
    CHECK(LZ4F_isError(result), "Allocation failed (error %i)", (int)result);
	srcBuffer = malloc(srcDataLength);
    CHECK(srcBuffer==NULL, "srcBuffer Allocation failed");
	compressedBuffer = malloc(LZ4F_compressFrameBound(srcDataLength, NULL));
    CHECK(compressedBuffer==NULL, "compressedBuffer Allocation failed");
	decodedBuffer = malloc(srcDataLength);
    CHECK(decodedBuffer==NULL, "decodedBuffer Allocation failed");
	FUZ_fillCompressibleNoiseBuffer(srcBuffer, srcDataLength, compressibility, &coreRand);

    // jump to requested testNb
	for (testNb =0; testNb < startTest; testNb++) (void)FUZ_rand(&coreRand);   // sync randomizer

    // main fuzzer loop
    for ( ; testNb < nbTests; testNb++)
    {
        U32 randState = coreRand ^ prime1;
        unsigned BSId   = 4 + (FUZ_rand(&randState) & 3);
        unsigned BMId   = FUZ_rand(&randState) & 1;
        unsigned CCflag = FUZ_rand(&randState) & 1;
        unsigned autoflush = (FUZ_rand(&randState) & 3) == 2;
        LZ4F_preferences_t prefs = { 0 };
        LZ4F_compressOptions_t options = { 0 };
        unsigned nbBits = (FUZ_rand(&randState) % (FUZ_highbit(srcDataLength-1) - 1)) + 1;
        size_t srcSize = (FUZ_rand(&randState) & ((1<<nbBits)-1)) + 1;
        size_t srcStart = FUZ_rand(&randState) % (srcDataLength - srcSize);
        size_t cSize;
        U64 crcOrig, crcDecoded;

        prefs.frameInfo.blockMode = BMId;
        prefs.frameInfo.blockSizeID = BSId;
        prefs.frameInfo.contentChecksumFlag = CCflag;
        prefs.autoFlush = autoflush;

        DISPLAYUPDATE(2, "\r%5i   ", testNb);
        crcOrig = XXH64((BYTE*)srcBuffer+srcStart, srcSize, 1);

        {
            const BYTE* ip = (const BYTE*)srcBuffer + srcStart;
            const BYTE* const iend = ip + srcSize;
            BYTE* op = compressedBuffer;
            BYTE* const oend = op + LZ4F_compressFrameBound(srcDataLength, NULL);
            unsigned maxBits = FUZ_highbit(srcSize);
            result = LZ4F_compressBegin(cCtx, op, oend-op, &prefs);
            CHECK(LZ4F_isError(result), "Compression header failed (error %i)", (int)result);
            op += result;
            while (ip < iend)
            {
                unsigned nbBitsSeg = FUZ_rand(&randState) % maxBits;
                size_t iSize = (FUZ_rand(&randState) & ((1<<nbBitsSeg)-1)) + 1;
                size_t oSize = oend-op;
                unsigned forceFlush = ((FUZ_rand(&randState) & 3) == 1);
                if (iSize > (size_t)(iend-ip)) iSize = iend-ip;
                options.stableSrc = ((FUZ_rand(&randState) && 3) == 2);

                result = LZ4F_compressUpdate(cCtx, op, oSize, ip, iSize, &options);
                CHECK(LZ4F_isError(result), "Compression failed (error %i)", (int)result);
                op += result;
                ip += iSize;

                if (forceFlush)
                {
                    result = LZ4F_flush(cCtx, op, oend-op, &options);
                    CHECK(LZ4F_isError(result), "Compression failed (error %i)", (int)result);
                    op += result;
                }
            }
            result = LZ4F_compressEnd(cCtx, op, oend-op, &options);
            CHECK(LZ4F_isError(result), "Compression completion failed (error %i)", (int)result);
            op += result;
            cSize = op-(BYTE*)compressedBuffer;
        }

        {
            const BYTE* ip = compressedBuffer;
            const BYTE* const iend = ip + cSize;
            BYTE* op = decodedBuffer;
            BYTE* const oend = op + srcDataLength;
            unsigned maxBits = FUZ_highbit(cSize);
            while (ip < iend)
            {
                unsigned nbBitsI = (FUZ_rand(&randState) % (maxBits-1)) + 1;
                unsigned nbBitsO = (FUZ_rand(&randState) % (maxBits-1)) + 1;
                size_t iSize = (FUZ_rand(&randState) & ((1<<nbBitsI)-1)) + 1;
                size_t oSize = (FUZ_rand(&randState) & ((1<<nbBitsO)-1)) + 1;
                if (iSize > (size_t)(iend-ip)) iSize = iend-ip;
                if (oSize > (size_t)(oend-op)) oSize = oend-op;
                oSize = oend-op;
                result = LZ4F_decompress(dCtx, op, &oSize, ip, &iSize, NULL);
                if (result == (size_t)-ERROR_checksum_invalid) locateBuffDiff((BYTE*)srcBuffer+srcStart, decodedBuffer, srcSize);
                CHECK(LZ4F_isError(result), "Decompression failed (error %i)", (int)result);
                op += oSize;
                ip += iSize;
            }
            CHECK(result != 0, "Frame decompression failed (error %i)", (int)result);
            crcDecoded = XXH64(decodedBuffer, op-(BYTE*)decodedBuffer, 1);
            if (crcDecoded != crcOrig) locateBuffDiff((BYTE*)srcBuffer+srcStart, decodedBuffer, srcSize);
            CHECK(crcDecoded != crcOrig, "Decompression corruption");
        }

        (void)FUZ_rand(&coreRand);   // update rand seed
    }

	DISPLAYLEVEL(2, "\rAll tests completed   \n");

_end:
    LZ4F_freeDecompressionContext(dCtx);
    LZ4F_freeCompressionContext(cCtx);
	free(srcBuffer);
	free(compressedBuffer);
	free(decodedBuffer);
	return testResult;

_output_error:
	testResult = 1;
	goto _end;
}


int FUZ_usage(void)
{
    DISPLAY( "Usage :\n");
    DISPLAY( "      %s [args]\n", programName);
    DISPLAY( "\n");
    DISPLAY( "Arguments :\n");
    DISPLAY( " -i#    : Nb of tests (default:%i) \n", nbTestsDefault);
    DISPLAY( " -s#    : Select seed (default:prompt user)\n");
    DISPLAY( " -t#    : Select starting test number (default:0)\n");
    DISPLAY( " -p#    : Select compressibility in %% (default:%i%%)\n", FUZ_COMPRESSIBILITY_DEFAULT);
    DISPLAY( " -v     : verbose\n");
    DISPLAY( " -h     : display help and exit\n");
    return 0;
}


int main(int argc, char** argv)
{
    U32 seed=0;
    int seedset=0;
    int argNb;
    int nbTests = nbTestsDefault;
    int testNb = 0;
    int proba = FUZ_COMPRESSIBILITY_DEFAULT;
    int result=0;

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
                case 'q':
                    argument++;
                    displayLevel--;
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
    printf("Starting lz4frame tester (%i-bits, %s)\n", (int)(sizeof(size_t)*8), LZ4_VERSION);

    if (!seedset) seed = FUZ_GetMilliStart() % 10000;
    printf("Seed = %u\n", seed);
    if (proba!=FUZ_COMPRESSIBILITY_DEFAULT) printf("Compressibility : %i%%\n", proba);

    if (nbTests<=0) nbTests=1;

    //if (testNb==0) result = basicTests(seed, ((double)proba) / 100);
    if (result) return 1;
    return fuzzerTests(seed, nbTests, testNb, ((double)proba) / 100);
}
