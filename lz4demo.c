/*
    LZ4Demo - Demo CLI program using LZ4 compression
    Copyright (C) Yann Collet 2011,

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
	Note : this is *only* a demo program, an example to show how LZ4 can be used.
	It is not considered part of LZ4 compression library.
	The license of the demo program is GPL.
	The license of LZ4 is BSD.
*/

//****************************
// Includes
//****************************
#include <stdio.h>		// fprintf, fopen, fread, _fileno(?)
#include <stdlib.h>		// malloc
#include <string.h>		// strcmp
#include <time.h>		// clock
#ifdef _WIN32 
#include <io.h>			// _setmode
#include <fcntl.h>		// _O_BINARY
#endif
#include "lz4.h"
#include "bench.h"


//**************************************
// Compiler functions
//**************************************
#if defined(_MSC_VER)    // Visual Studio 
#define swap32 _byteswap_ulong
#else    // GCC assumed
#define swap32 __builtin_bswap32
#endif


//****************************
// Constants
//****************************
#define COMPRESSOR_NAME "Compression CLI using LZ4 algorithm"
#define COMPRESSOR_VERSION ""
#define COMPILED __DATE__
#define AUTHOR "Yann Collet"
#define BINARY_NAME "lz4demo.exe"
#define EXTENSION ".lz4"
#define WELCOME_MESSAGE "*** %s %s, by %s (%s) ***\n", COMPRESSOR_NAME, COMPRESSOR_VERSION, AUTHOR, COMPILED

#define CHUNKSIZE (8<<20)    // 8 MB
#define CACHELINE 64
#define ARCHIVE_MAGICNUMBER 0x184C2102
#define ARCHIVE_MAGICNUMBER_SIZE 4


//**************************************
// Architecture Macros
//**************************************
static const int one = 1;
#define CPU_LITTLE_ENDIAN (*(char*)(&one))
#define CPU_BIG_ENDIAN (!CPU_LITTLE_ENDIAN)
#define LITTLE_ENDIAN32(i)   if (CPU_BIG_ENDIAN) { i = swap32(i); }


//**************************************
// Macros
//**************************************
#define DISPLAY(...) fprintf(stderr, __VA_ARGS__)


//****************************
// Functions
//****************************
int usage()
{
	DISPLAY( "Usage :\n");
	DISPLAY( "      %s [arg] input output\n", BINARY_NAME);
	DISPLAY( "Arguments :\n");
	DISPLAY( " -c : compression (default)\n");
	DISPLAY( " -d : decompression \n");
	DISPLAY( " -b : benchmark with files\n");
	DISPLAY( " -t : check compressed file \n");
	DISPLAY( " -h : help (this text)\n");	
	DISPLAY( "input  : can be 'stdin' (pipe) or a filename\n");
	DISPLAY( "output : can be 'stdout'(pipe) or a filename or 'null'\n");
	return 0;
}


int badusage()
{
	DISPLAY("Wrong parameters\n");
	usage();
	return 0;
}



int get_fileHandle(char* input_filename, char* output_filename, FILE** pfinput, FILE** pfoutput)
{
	char stdinmark[] = "stdin";
	char stdoutmark[] = "stdout";

	if (!strcmp (input_filename, stdinmark)) {
		DISPLAY( "Using stdin for input\n");
		*pfinput = stdin;
#ifdef _WIN32 // Need to set stdin/stdout to binary mode specifically for windows
		_setmode( _fileno( stdin ), _O_BINARY );
#endif
	} else {
		*pfinput = fopen( input_filename, "rb" );
	}

	if (!strcmp (output_filename, stdoutmark)) {
		DISPLAY( "Using stdout for output\n");
		*pfoutput = stdout;
#ifdef _WIN32 // Need to set stdin/stdout to binary mode specifically for windows
		_setmode( _fileno( stdout ), _O_BINARY );
#endif
	} else {
		*pfoutput = fopen( output_filename, "wb" );
	}
	
	if ( *pfinput==0 ) { DISPLAY( "Pb opening %s\n", input_filename);  return 2; }
	if ( *pfoutput==0) { DISPLAY( "Pb opening %s\n", output_filename); return 3; }

	return 0;
}



int compress_file(char* input_filename, char* output_filename)
{
	unsigned long long filesize = 0;
	unsigned long long compressedfilesize = ARCHIVE_MAGICNUMBER_SIZE;
	unsigned int u32var;
	char* in_buff;
	char* out_buff;
	FILE* finput;
	FILE* foutput;
	int r;
	clock_t start, end;


	// Init
	start = clock();
	r = get_fileHandle(input_filename, output_filename, &finput, &foutput);
	if (r) return r;
	
	// Allocate Memory
	in_buff = (char*)malloc(CHUNKSIZE);
	out_buff = (char*)malloc(LZ4_compressBound(CHUNKSIZE));
	if (!in_buff || !out_buff) { DISPLAY("Allocation error : not enough memory\n"); return 8; }
	
	// Write Archive Header
	u32var = ARCHIVE_MAGICNUMBER;
	LITTLE_ENDIAN32(u32var);
	*(unsigned int*)out_buff = u32var;
	fwrite(out_buff, 1, ARCHIVE_MAGICNUMBER_SIZE, foutput);

	// Main Loop
	while (1) 
	{	
		int outSize;
		// Read Block
	    int inSize = fread(in_buff, 1, CHUNKSIZE, finput);
		if( inSize<=0 ) break;
		filesize += inSize;

		// Compress Block
		outSize = LZ4_compress(in_buff, out_buff+4, inSize);
		compressedfilesize += outSize+4;

		// Write Block
		LITTLE_ENDIAN32(outSize);
		* (unsigned int*) out_buff = outSize;
		LITTLE_ENDIAN32(outSize);
		fwrite(out_buff, 1, outSize+4, foutput);
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


int decode_file(char* input_filename, char* output_filename)
{
	unsigned long long filesize = 0;
	char* in_buff;
	char* out_buff;
	size_t uselessRet;
	int sinkint;
	unsigned int nextSize;
	FILE* finput;
	FILE* foutput;
	clock_t start, end;
	int r;


	// Init
	start = clock();
	r = get_fileHandle(input_filename, output_filename, &finput, &foutput);
	if (r) return r;

	// Allocate Memory
	in_buff = (char*)malloc(LZ4_compressBound(CHUNKSIZE));
	out_buff = (char*)malloc(CHUNKSIZE);
	if (!in_buff || !out_buff) { DISPLAY("Allocation error : not enough memory\n"); return 7; }
	
	// Check Archive Header
	uselessRet = fread(out_buff, 1, ARCHIVE_MAGICNUMBER_SIZE, finput);
	nextSize = *(unsigned int*)out_buff;
	LITTLE_ENDIAN32(nextSize);
	if (nextSize != ARCHIVE_MAGICNUMBER) { DISPLAY("Unrecognized header : file cannot be decoded\n"); return 6; }

	// First Block
	*(unsigned int*)in_buff = 0;
	uselessRet = fread(in_buff, 1, 4, finput);
	nextSize = *(unsigned int*)in_buff;
	LITTLE_ENDIAN32(nextSize);

	// Main Loop
	while (1) 
	{	
		// Read Block
	    uselessRet = fread(in_buff, 1, nextSize, finput);

		// Check Next Block
		uselessRet = (size_t) fread(&nextSize, 1, 4, finput);
		if( uselessRet==0 ) break;   // Nothing read : file read is completed
		LITTLE_ENDIAN32(nextSize);

		// Decode Block
		sinkint = LZ4_uncompress(in_buff, out_buff, CHUNKSIZE);
		if (sinkint < 0) { DISPLAY("Decoding Failed ! Corrupted input !\n"); return 9; }
		filesize += CHUNKSIZE;

		// Write Block
		fwrite(out_buff, 1, CHUNKSIZE, foutput);
	}

	// Last Block (which size is <= CHUNKSIZE, but let LZ4 figure that out)
    uselessRet = fread(in_buff, 1, nextSize, finput);
	sinkint = LZ4_uncompress_unknownOutputSize(in_buff, out_buff, nextSize, CHUNKSIZE);
	filesize += sinkint;
	fwrite(out_buff, 1, sinkint, foutput);

	// Status
	end = clock();
	DISPLAY( "Successfully decoded %llu bytes \n", (unsigned long long)filesize);
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


int main(int argc, char** argv) 
{
  int i,
	  compression=1,   // default action if no argument
	  decode=0,
	  bench=0;
  char* input_filename=0;
  char* output_filename=0;
#ifdef _WIN32 
  char nulmark[] = "nul";
#else
  char nulmark[] = "/dev/null";
#endif
  char nullinput[] = "null";

  // Welcome message
  DISPLAY( WELCOME_MESSAGE);

  if (argc<2) { badusage(); return 1; }

  for(i=1; i<argc; i++)
  {
    char* argument = argv[i];
	char command = 0;

    if(!argument) continue;   // Protection if argument empty

	if (argument[0]=='-') command++;  // valid command trigger

	// Select command
	if (command)
	{
		argument += command;
		
		// Display help on usage
		if ( argument[0] =='h' ) { usage(); return 0; }

		// Compression (default)
		if ( argument[0] =='c' ) { compression=1; continue; }

		// Decoding
		if ( argument[0] =='d' ) { decode=1; continue; }

		// Bench
		if ( argument[0] =='b' ) { bench=1; continue; }

		// Test
		if ( argument[0] =='t' ) { decode=1; output_filename=nulmark; continue; }
	}

	// first provided filename is input
    if (!input_filename) { input_filename=argument; continue; }

	// second provided filename is output
    if (!output_filename) 
	{ 
		output_filename=argument; 
		if (!strcmp (output_filename, nullinput)) output_filename = nulmark;
		continue; 
	}
  }

  // No input filename ==> Error
  if(!input_filename) { badusage(); return 1; }

  if (bench) return BMK_benchFile(argv+2, argc-2);

  // No output filename 
  if (!output_filename) { badusage(); return 1; }

  if (decode) return decode_file(input_filename, output_filename);

  if (compression) return compress_file(input_filename, output_filename);

  badusage();

  return 0;
}
