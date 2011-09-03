/*
   Demo compression program using LZ4 compression
   Copyright (C) Yann Collet 2011,

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
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
#include <stdio.h>     // fprintf, fopen, fread
#include <stdlib.h>    // malloc
#include <string.h>    // strcmp
#ifdef _WIN32 
#include <io.h>		   // _setmode
#include <fcntl.h>	   // _O_BINARY
#endif
#include "lz4.h"


//**************************************
// Basic Types
//**************************************
#if defined(_MSC_VER) || defined(_WIN32) || defined(__WIN32__)
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


//****************************
// Constants
//****************************
#define COMPRESSOR_NAME "Compression CLI using LZ4 algorithm"
#define COMPRESSOR_VERSION ""
#define COMPILED __DATE__
#define AUTHOR "Yann Collet"
#define BINARY_NAME "LZ4.exe"
#define EXTENSION ".lz4"
#define WELCOME_MESSAGE "*** %s %s, by %s (%s) ***\n", COMPRESSOR_NAME, COMPRESSOR_VERSION, AUTHOR, COMPILED

#define CHUNKSIZE (8<<20)    // 8 MB
#define CACHELINE 64
#define OUT_CHUNKSIZE (CHUNKSIZE + (CHUNKSIZE>>8) + CACHELINE)
#define ARCHIVE_MAGICNUMBER 0x184C2102
#define ARCHIVE_MAGICNUMBER_SIZE 4



//****************************
// Functions
//****************************
int usage()
{
	fprintf(stderr, "Usage :\n");
	fprintf(stderr, "      %s [arg] input output\n",BINARY_NAME);
	fprintf(stderr, "Arguments :\n");
	fprintf(stderr, " -c : force compression (default)\n");
	fprintf(stderr, " -d : force decompression \n");
	fprintf(stderr, " -h : help (this text)\n");	
	fprintf(stderr, "input  : can be 'stdin' (pipe)  or a filename\n");
	fprintf(stderr, "output : can be 'stdout' (pipe) or a filename\n");
	return 0;
}


int badusage()
{
	fprintf(stderr, "Wrong parameters\n");
	usage();
	return 0;
}


int compress_file(char* input_filename, char* output_filename)
{
	U64 filesize = 0;
	U64 compressedfilesize = ARCHIVE_MAGICNUMBER_SIZE;
	char* in_buff;
	char* out_buff;
	FILE* finput;
	FILE* foutput;
	char stdinmark[] = "stdin";
	char stdoutmark[] = "stdout";

	if (!strcmp (input_filename, stdinmark)) {
		fprintf(stderr, "Using stdin for input\n");
		finput = stdin;
#ifdef _WIN32 /* We need to set stdin/stdout to binary mode. Damn windows. */
		_setmode( _fileno( stdin ), _O_BINARY );
#endif
	} else {
		finput = fopen( input_filename, "rb" );
	}

	if (!strcmp (output_filename, stdoutmark)) {
		fprintf(stderr, "Using stdout for output\n");
		foutput = stdout;
#ifdef _WIN32 /* We need to set stdin/stdout to binary mode. Damn windows. */
		_setmode( _fileno( stdout ), _O_BINARY );
#endif
	} else {
		foutput = fopen( output_filename, "wb" );
	}
	
	if ( finput==0 ) { fprintf(stderr, "Pb opening %s\n", input_filename);  return 2; }
	if ( foutput==0) { fprintf(stderr, "Pb opening %s\n", output_filename); return 3; }

	// Allocate Memory
	in_buff = malloc(CHUNKSIZE);
	out_buff = malloc(OUT_CHUNKSIZE);
	
	// Write Archive Header
	*(U32*)out_buff = ARCHIVE_MAGICNUMBER;
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
		* (U32*) out_buff = outSize;
		compressedfilesize += outSize+4;

		// Write Block
		fwrite(out_buff, 1, outSize+4, foutput);
	}

	// Status
	fprintf(stderr, "Compressed %llu bytes into %llu bytes ==> %.2f%%\n", 
		(unsigned long long) filesize, (unsigned long long) compressedfilesize, (double)compressedfilesize/filesize*100);

	fclose(finput);
	fclose(foutput);

	return 0;
}


int decode_file(char* input_filename, char* output_filename)
{
	U64 filesize = 0;
	char* in_buff;
	char* out_buff;
	size_t uselessRet;
	int sinkint;
	U32 nextSize;
	FILE* finput;
	FILE* foutput;
	char stdinmark[] = "stdin";
	char stdoutmark[] = "stdout";

	if (!strcmp (input_filename, stdinmark)) {
		fprintf(stderr, "Using stdin for input\n");
		finput = stdin;
#ifdef _WIN32 /* We need to set stdin/stdout to binary mode. Damn windows. */
		_setmode( _fileno( stdin ), _O_BINARY );
#endif
	} else {
		finput = fopen( input_filename, "rb" );
	}

	if (!strcmp (output_filename, stdoutmark)) {
		fprintf(stderr, "Using stdout for output\n");
		foutput = stdout;
#ifdef _WIN32 /* We need to set stdin/stdout to binary mode. Damn windows. */
		_setmode( _fileno( stdout ), _O_BINARY );
#endif
	} else {
		foutput = fopen( output_filename, "wb" );
	}

	// Allocate Memory
	in_buff = malloc(OUT_CHUNKSIZE);
	out_buff = malloc(CHUNKSIZE);
	
	// Check Archive Header
	uselessRet = fread(out_buff, 1, ARCHIVE_MAGICNUMBER_SIZE, finput);
	if (*(U32*)out_buff != ARCHIVE_MAGICNUMBER) { fprintf(stderr,"Wrong file : cannot be decoded\n"); return 6; }
	uselessRet = fread(in_buff, 1, 4, finput);
	nextSize = *(U32*)in_buff;

	// Main Loop
	while (1) 
	{	
		// Read Block
	    uselessRet = fread(in_buff, 1, nextSize, finput);

		// Check Next Block
		uselessRet = (U32) fread(&nextSize, 1, 4, finput);
		if( uselessRet==0 ) break;

		// Decode Block
		sinkint = LZ4_uncompress(in_buff, out_buff, CHUNKSIZE);
		filesize += CHUNKSIZE;

		// Write Block
		fwrite(out_buff, 1, CHUNKSIZE, foutput);
	}

	// Last Block
    uselessRet = fread(in_buff, 1, nextSize, finput);
	sinkint = LZ4_uncompress_unknownOutputSize(in_buff, out_buff, nextSize, CHUNKSIZE);
	filesize += sinkint;
	fwrite(out_buff, 1, sinkint, foutput);

	// Status
	fprintf(stderr, "Successfully decoded %llu bytes \n", (unsigned long long)filesize);

	fclose(finput);
	fclose(foutput);

	return 0;
}


int main(int argc, char** argv) 
{
  int i,
	  compression=1,   // default action if no argument
	  decode=0;
  char *input_filename=0,
	   *output_filename=0;

  // Welcome message
  fprintf(stderr, WELCOME_MESSAGE);

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
		
		// display help on usage
		if ( argument[0] =='h' ) { usage(); return 0; }

		// Forced Compression (default)
		if ( argument[0] =='c' ) { compression=1; continue; }

		// Forced Decoding
		if ( argument[0] =='d' ) { decode=1; continue; }
	}

	// first provided filename is input
    if (!input_filename) { input_filename=argument; continue; }

	// second provided filename is output
    if (!output_filename) { output_filename=argument; continue; }
  }

  // No input filename ==> Error
  if(!input_filename) { badusage(); return 1; }

  // No output filename 
  if (!output_filename) { badusage(); return 1; }

  if (decode) return decode_file(input_filename, output_filename);

  if (compression) return compress_file(input_filename, output_filename);

  badusage();

  return 0;
}
