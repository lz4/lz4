/*
    bench.c - Demo program to benchmark open-source compression algorithm
    Copyright (C) Yann Collet 2012

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
// Compilation Directives
//**************************************


//**************************************
// Includes
//**************************************
#include <stdio.h>      // fprintf, fopen, ftello64
#include <stdlib.h>     // malloc
#include <sys/timeb.h>  // timeb
#include "lz4.h"


//**************************************
// Basic Types
//**************************************
#if defined(_MSC_VER)    // Visual Studio does not support 'stdint' natively
#define BYTE	unsigned __int8
#define U16		unsigned __int16
#define U32		unsigned __int32
#define S32		__int32
#define U64		unsigned __int64
#else
#include <stdint.h>
#define BYTE	uint8_t
#define U16		uint16_t
#define U32		uint32_t
#define S32		int32_t
#define U64		uint64_t
#endif


//**************************************
// Constants
//**************************************
#define NBLOOPS		3
#define TIMELOOP	2000

#define KNUTH		2654435761U
#define MAX_MEM		(1984<<20)
#define CHUNKSIZE   (8<<20)
#define MAX_NB_CHUNKS ((MAX_MEM / CHUNKSIZE) + 1)

 
//**************************************
// Local structures
//**************************************
struct chunkParameters
{
	U32   id;
	char* inputBuffer;
	char* outputBuffer;
	int   inputSize;
	int   outputSize;
};

struct compressionParameters
{
	int (*compressionFunction)(char*, char*, int);
	int (*decompressionFunction)(char*, char*, int);
};


//**************************************
// MACRO
//**************************************
#define DISPLAY(...) fprintf(stderr, __VA_ARGS__)



//*********************************************************
//  Private functions
//*********************************************************


static int BMK_GetMilliStart()
{
  // Supposed to be portable
  // Rolls over every ~ 12.1 days (0x100000/24/60/60)
  // Use GetMilliSpan to correct for rollover
  struct timeb tb;
  int nCount;
  ftime( &tb );
  nCount = tb.millitm + (tb.time & 0xfffff) * 1000;
  return nCount;
}


static int BMK_GetMilliSpan( int nTimeStart )
{
  int nSpan = BMK_GetMilliStart() - nTimeStart;
  if ( nSpan < 0 )
    nSpan += 0x100000 * 1000;
  return nSpan;
}


static U32 BMK_checksum(char* buff, U32 length)
{
	BYTE* p = (BYTE*)buff;
	BYTE* bEnd = p + length;
	BYTE* limit = bEnd - 3;
	U32 idx = 1;
	U32 crc = KNUTH;
	
	while (p<limit)
	{
		crc += ((*(U32*)p) + idx++);
		crc *= KNUTH;
		p+=4;
	}
	while (p<bEnd)
	{
		crc += ((*p) + idx++);
		crc *= KNUTH;
		p++;
	}
	return crc;
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
		testmem = malloc ((size_t)requiredMem);
	}

	free (testmem);
	return (size_t) (requiredMem - step);
}


static U64 BMK_GetFileSize(FILE* f)
{
	U64 r;
#ifdef _MSC_VER
	r = _fseeki64(f, 0L, SEEK_END);
	r = (U64) _ftelli64(f);
	_fseeki64(f, 0L, SEEK_SET);
#else
	r = (U64) fseeko64(f, 0LL, SEEK_END);
	r = (U64) ftello64(f);
	fseeko64(f, 0LL, SEEK_SET);
#endif
	return r;
}


//*********************************************************
//  Public function
//*********************************************************

int BMK_benchFile(char** fileNamesTable, int nbFiles) 
{
  int fileIdx=0;
  FILE* fileIn;
  char* infilename;
  U64 largefilesize;
  size_t benchedsize;
  int nbChunks;
  int maxCChunkSize;
  size_t readSize;
  char* in_buff;
  char* out_buff; int out_buff_size;
  struct chunkParameters chunkP[MAX_NB_CHUNKS];
  U32 crcc, crcd;
  struct compressionParameters compP;

  U64 totals = 0;
  U64 totalz = 0;
  double totalc = 0.;
  double totald = 0.;
  

  // Init
  compP.compressionFunction = LZ4_compress;
  compP.decompressionFunction = LZ4_uncompress;

  // Loop for each file
  while (fileIdx<nbFiles)
  {
	  // Check file existence
	  infilename = fileNamesTable[fileIdx++];
	  fileIn = fopen( infilename, "rb" );
	  if (fileIn==NULL)
	  {
		DISPLAY( "Pb opening %s\n", infilename);
		return 11;
	  }

	  // Memory allocation & restrictions
	  largefilesize = BMK_GetFileSize(fileIn);
	  benchedsize = (size_t) BMK_findMaxMem(largefilesize) / 2;
	  if ((U64)benchedsize > largefilesize) benchedsize = (size_t)largefilesize;
	  if (benchedsize < largefilesize)
	  {
		  DISPLAY("Not enough memory for '%s' full size; testing %i MB only...\n", infilename, (int)(benchedsize>>20));
	  }

	  // Alloc
	  in_buff = malloc((size_t )benchedsize);
	  nbChunks = (benchedsize / CHUNKSIZE) + 1;
	  maxCChunkSize = CHUNKSIZE + CHUNKSIZE/255 + 64;
	  out_buff_size = nbChunks * maxCChunkSize;
	  out_buff = malloc((size_t )out_buff_size);

	  if(!in_buff || !out_buff)
	  {
		DISPLAY("\nError: not enough memory!\n");
		free(in_buff);
		free(out_buff);
		fclose(fileIn);
		return 12;
	  }

	  // Init chunks data
	  {
		  int i;
		  size_t remaining = benchedsize;
		  char* in = in_buff;
		  char* out = out_buff;
		  for (i=0; i<nbChunks; i++)
		  {
			  chunkP[i].id = i;
			  chunkP[i].inputBuffer = in; in += CHUNKSIZE;
			  if (remaining > CHUNKSIZE) { chunkP[i].inputSize = CHUNKSIZE; remaining -= CHUNKSIZE; } else { chunkP[i].inputSize = remaining; remaining = 0; }
			  chunkP[i].outputBuffer = out; out += maxCChunkSize;
			  chunkP[i].outputSize = 0;
		  }
	  }

	  // Fill input buffer
	  DISPLAY("Loading %s...    \r", infilename);
	  readSize = fread(in_buff, 1, benchedsize, fileIn);
	  fclose(fileIn);

	  if(readSize != benchedsize)
	  {
		DISPLAY("\nError: problem reading file '%s' !!    \n", infilename);
		free(in_buff);
		free(out_buff);
		return 13;
	  }

	  // Calculating input Checksum
	  crcc = BMK_checksum(in_buff, benchedsize);


	  // Bench
	  {
		int loopNb, nb_loops, chunkNb;
		size_t cSize;
	    int milliTime;
		double fastestC = 100000000., fastestD = 100000000.;

		for (loopNb = 1; loopNb <= NBLOOPS; loopNb++)
		{
		  // Compression 
		  DISPLAY("%1i-%-14.14s : %9i ->\r", loopNb, infilename, (int)benchedsize);
		  { size_t i; for (i=0; i<benchedsize; i++) out_buff[i]=(char)i; }     // warmimg up memory
		  
		  nb_loops = 0;
		  milliTime = BMK_GetMilliStart();
		  while(BMK_GetMilliStart() == milliTime);
		  milliTime = BMK_GetMilliStart();
		  while(BMK_GetMilliSpan(milliTime) < TIMELOOP) 
		  {
			for (chunkNb=0; chunkNb<nbChunks; chunkNb++) 
				chunkP[chunkNb].outputSize = compP.compressionFunction(chunkP[chunkNb].inputBuffer, chunkP[chunkNb].outputBuffer, chunkP[chunkNb].inputSize);  
			nb_loops++;
		  }
		  milliTime = BMK_GetMilliSpan(milliTime);

		  if ((double)milliTime < fastestC*nb_loops) fastestC = (double)milliTime/nb_loops;
		  cSize=0; for (chunkNb=0; chunkNb<nbChunks; chunkNb++) cSize += chunkP[chunkNb].outputSize; 

		  DISPLAY("%1i-%-14.14s : %9i -> %9i (%5.2f%%), %6.1f MB/s\r", loopNb, infilename, (int)benchedsize, (int)cSize, (double)cSize/(double)benchedsize*100., (double)benchedsize / fastestC / 1000.);

		  // Decompression 
		  { size_t i; for (i=0; i<benchedsize; i++) in_buff[i]=0; }     // zeroing area, for CRC checking

		  nb_loops = 0;
		  milliTime = BMK_GetMilliStart();
		  while(BMK_GetMilliStart() == milliTime);
		  milliTime = BMK_GetMilliStart();
		  while(BMK_GetMilliSpan(milliTime) < TIMELOOP) 
		  {
			for (chunkNb=0; chunkNb<nbChunks; chunkNb++) 
				chunkP[chunkNb].outputSize = compP.decompressionFunction(chunkP[chunkNb].outputBuffer, chunkP[chunkNb].inputBuffer, chunkP[chunkNb].inputSize);  
			nb_loops++;
		  }
		  milliTime = BMK_GetMilliSpan(milliTime);

		  if ((double)milliTime < fastestD*nb_loops) fastestD = (double)milliTime/nb_loops;
		  DISPLAY("%1i-%-14.14s : %9i -> %9i (%5.2f%%), %6.1f MB/s , %6.1f MB/s\r", loopNb, infilename, (int)benchedsize, (int)cSize, (double)cSize/(double)benchedsize*100., (double)benchedsize / fastestC / 1000., (double)benchedsize / fastestD / 1000.);
		  
		  // CRC Checking
		  crcd = BMK_checksum(in_buff, benchedsize);
		  if (crcc!=crcd) { DISPLAY("\n!!! WARNING !!! %14s : Invalid Checksum : %x != %x\n", infilename, (unsigned)crcc, (unsigned)crcd); break; }
		}

	    DISPLAY("%-16.16s : %9i -> %9i (%5.2f%%), %6.1f MB/s , %6.1f MB/s\n", infilename, (int)benchedsize, (int)cSize, (double)cSize/(double)benchedsize*100., (double)benchedsize / fastestC / 1000., (double)benchedsize / fastestD / 1000.);
		totals += benchedsize;
		totalz += cSize;
		totalc += fastestC;
		totald += fastestD;
	  }

	  free(in_buff);
	  free(out_buff);
  }

  if (nbFiles > 1)
		printf("%-16.16s :%10llu ->%10llu (%5.2f%%), %6.1f MB/s , %6.1f MB/s\n", "  TOTAL", (long long unsigned int)totals, (long long unsigned int)totalz, (double)totalz/(double)totals*100., (double)totals/totalc/1000., (double)totals/totald/1000.);

  return 0;
}



