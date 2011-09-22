/*
   LZ4 - Fast LZ compression algorithm
   Copyright (C) 2011, Yann Collet.
   BSD License

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:
  
       * Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above
   copyright notice, this list of conditions and the following disclaimer
   in the documentation and/or other materials provided with the
   distribution.
  
   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

//**************************************
// Includes
//**************************************
#include <stdlib.h>   // for malloc
#include <string.h>   // for memset
#include "lz4.h"


//**************************************
// Performance parameter               
//**************************************
// Lowering this value reduce memory usage
// It may also improve speed, especially if you reach L1 cache size (32KB for Intel, 64KB for AMD)
// Expanding memory usage typically improves compression ratio
// Memory usage formula for 32 bits systems : N->2^(N+2) Bytes (examples : 17 -> 512KB ; 12 -> 16KB)
#define HASH_LOG 12


//**************************************
// Basic Types
//**************************************
#if defined(_MSC_VER) 
#define BYTE	unsigned __int8
#define U16		unsigned __int16
#define U32		unsigned __int32
#define S32		__int32
#else
#include <stdint.h>
#define BYTE	uint8_t
#define U16		uint16_t
#define U32		uint32_t
#define S32		int32_t
#endif


//**************************************
// Constants
//**************************************
#define MINMATCH 4
#define MINLENGTH 6
#define SKIPSTRENGTH 6
#define STACKLIMIT 13
#define HEAPMODE (HASH_LOG>STACKLIMIT)  // Defines if memory is allocated into the stack (local variable), or into the heap (malloc()).

#define MAXD_LOG 16
#define MAX_DISTANCE ((1 << MAXD_LOG) - 1)

#define HASHTABLESIZE (1 << HASH_LOG)
#define HASH_MASK (HASHTABLESIZE - 1)

#define ML_BITS 4
#define ML_MASK ((1U<<ML_BITS)-1)
#define RUN_BITS (8-ML_BITS)
#define RUN_MASK ((1U<<RUN_BITS)-1)


//**************************************
// Local structures
//**************************************
struct refTables
{
	const BYTE* hashTable[HASHTABLESIZE];
};


//**************************************
// Macros
//**************************************
#define HASH_FUNCTION(i)	(((i) * 2654435761U) >> ((MINMATCH*8)-HASH_LOG))
#define HASH_VALUE(p)		HASH_FUNCTION(*(U32*)(p))



//****************************
// Compression CODE
//****************************

int LZ4_compressCtx(void** ctx,
				 char* source, 
				 char* dest,
				 int isize)
{	
#if HEAPMODE
	struct refTables *srt = (struct refTables *) (*ctx);
	const BYTE**  HashTable;
#else
	const BYTE* HashTable[HASHTABLESIZE] = {0};
#endif

	const BYTE* ip = (BYTE*) source;       
	const BYTE* anchor = ip;
	const BYTE* const iend = ip + isize;
	const BYTE* const ilm = iend - 1;
	const BYTE* const ilimit = iend - MINMATCH - 1;

	BYTE* op = (BYTE*) dest;
	BYTE* token;
	BYTE* l_end;
	
	int len, length;
	const int skipStrength = SKIPSTRENGTH;
	U32 forwardH;


	// Init 
	if (isize<MINLENGTH) goto _last_literals;
#if HEAPMODE
	if (*ctx == NULL) 
	{
		srt = (struct refTables *) malloc ( sizeof(struct refTables) );
		*ctx = (void*) srt;
	}
	HashTable = srt->hashTable;
	memset((void*)HashTable, 0, sizeof(srt->hashTable));
#else
	(void) ctx;
#endif


	// First Byte
	HashTable[HASH_VALUE(ip)] = ip;
	ip++; forwardH = HASH_VALUE(ip);
	
	// Main Loop
    for ( ; ; ) 
	{
		int findMatchAttempts = (1U << skipStrength) + 3;
		const BYTE* forwardIp = ip;
		const BYTE* ref;

		// Find a match
		do {
			U32 h = forwardH;
			int step = findMatchAttempts++ >> skipStrength;
			ip = forwardIp;
			forwardIp = ip + step;

			if (forwardIp > ilimit) { goto _last_literals; }

			forwardH = HASH_VALUE(forwardIp);
			ref = HashTable[h];
			HashTable[h] = ip;

		} while ((ref < ip - MAX_DISTANCE) || (*(U32*)ref != *(U32*)ip));

		// Catch up
		while ((ip>anchor) && (ref>(BYTE*)source) && (ip[-1]==ref[-1])) { ip--; ref--; }  

		// Encode Literal length
		length = ip - anchor;
		token = op++;
		if (length>=(int)RUN_MASK) { *token=(RUN_MASK<<ML_BITS); len = length-RUN_MASK; for(; len > 254 ; len-=255) *op++ = 255; *op++ = (BYTE)len; } 
		else *token = (length<<ML_BITS);

		// Copy Literals
		l_end = op + length;
		do { *(U32*)op = *(U32*)anchor; op+=4; anchor+=4; } while (op<l_end) ;
		op = l_end;

_next_match:
		// Encode Offset
		*(U16*)op = (ip-ref); op+=2;

		// Start Counting
		ip+=MINMATCH; ref+=MINMATCH;   // MinMatch verified
		anchor = ip;
		while (ip<(iend-4))
		{
			if (*(U32*)ref == *(U32*)ip) { ip+=4; ref+=4; continue; }
			if (*(U16*)ref == *(U16*)ip) { ip+=2; ref+=2; }
			if (*ref == *ip) ip++;
			goto _endCount;
		}
		if ((ip<(iend-2)) && (*(U16*)ref == *(U16*)ip)) { ip+=2; ref+=2; }
		if ((ip<iend-1) && (*ref == *ip)) ip++;
_endCount:
		len = (ip - anchor);
		
		// Encode MatchLength
		if (len>=(int)ML_MASK) { *token+=ML_MASK; len-=ML_MASK; for(; len > 509 ; len-=510) { *op++ = 255; *op++ = 255; } if (len > 254) { len-=255; *op++ = 255; } *op++ = (BYTE)len; } 
		else *token += len;	

		// Test end of chunk
		if (ip > ilimit-1) { anchor = ip;  break; }

		// Test next position
		ref = HashTable[HASH_VALUE(ip)];
		HashTable[HASH_VALUE(ip)] = ip;
		if ((ref > ip - (MAX_DISTANCE + 1)) && (*(U32*)ref == *(U32*)ip)) { token = op++; *token=0; goto _next_match; }

		// Prepare next loop
		anchor = ip++; 
		forwardH = HASH_VALUE(ip);
	}

_last_literals:
	// Encode Last Literals
	{
		int lastLitRun = iend - anchor;
		if (lastLitRun>=(int)RUN_MASK) { *op++=(RUN_MASK<<ML_BITS); lastLitRun-=RUN_MASK; for(; lastLitRun > 254 ; lastLitRun-=255) *op++ = 255; *op++ = (BYTE) lastLitRun; } 
		else *op++ = (lastLitRun<<ML_BITS);
		while (anchor < iend - 3) { *(U32*)op = *(U32*)anchor; op+=4; anchor+=4; }
		while (anchor < iend ) *op++ = *anchor++;
	} 

	// End
	return (int) (((char*)op)-dest);
}



int LZ4_compress(char* source, 
				 char* dest,
				 int isize)
{
#if HEAPMODE
	void* ctx = malloc(sizeof(struct refTables));
	int result = LZ4_compressCtx(&ctx, source, dest, isize);
	free(ctx);
	return result;
#else
	return LZ4_compressCtx(NULL, source, dest, isize);
#endif
}




//****************************
// Decompression CODE
//****************************

// Note : The decoding functions LZ4_uncompress() and LZ4_uncompress_unknownOutputSize() 
//		are safe against "buffer overflow" attack type
//		since they will *never* write outside of the provided output buffer :
//		they both check this condition *before* writing anything.
//		A corrupted packet however can make them *read* within the first 64K before the output buffer.

int LZ4_uncompress(char* source, 
				 char* dest,
				 int osize)
{	
	// Local Variables
	const BYTE* ip = (const BYTE*) source;
	BYTE* ref;

	BYTE* op = (BYTE*) dest;
	BYTE* const olimit = op + osize - 4;
	BYTE* cpy;

	BYTE token;
	
	U32		dec[4]={0, 3, 2, 3};
	int		length;


	// Main Loop
	while (1)
	{
		// get runlength
		token = *ip++;
		if ((length=(token>>ML_BITS)) == RUN_MASK)  { for (;*ip==255;length+=255) {ip++;} length += *ip++; } 

		// copy literals
		ref = op+length;
		if (ref > olimit) 
		{ 
			if (ref > olimit+4) goto _output_error;
			while(op <= olimit) { *(U32*)op=*(U32*)ip; op+=4; ip+=4; } 
			while(op < ref) *op++=*ip++; 
			break;    // Necessarily EOF
		}
		do { *(U32*)op = *(U32*)ip; op+=4; ip+=4; } while (op<ref) ;
		ip -= (op-ref); op = ref;	// correction

		// get offset
		ref -= *(U16*)ip; ip+=2;

		// get matchlength
		if ((length=(token&ML_MASK)) == ML_MASK) { for (;*ip==255;length+=255) {ip++;} length += *ip++; } 
		length += MINMATCH;

		// copy repeated sequence
		cpy = op + length;
		if (op-ref<4)
		{
			*op++ = *ref++;
			*op++ = *ref++;
			*op++ = *ref++;
			*op++ = *ref++;
			ref -= dec[op-ref];
		} else { *(U32*)op=*(U32*)ref; op+=4; ref+=4; }
		if (cpy > olimit)
		{
			if (cpy > olimit+4) goto _output_error;
			while(op < cpy-3) { *(U32*)op=*(U32*)ref; op+=4; ref+=4; }
			while(op < cpy) *op++=*ref++;
			if (op >= olimit+4) break;    // Check EOF
			continue;
		}
		do { *(U32*)op = *(U32*)ref; op+=4; ref+=4; } while (op<cpy) ;
		op=cpy;		// correction
	}

	// end of decoding
	return (int) (((char*)ip)-source);

	// write overflow error detected
_output_error:
	return (int) (-(((char*)ip)-source));
}


int LZ4_uncompress_unknownOutputSize(
				char* source, 
				char* dest,
				int isize,
				int maxOutputSize)
{	
	// Local Variables
	const BYTE* ip = (const BYTE*) source;
	const BYTE* const iend = ip + isize;
	BYTE* ref;

	BYTE* op = (BYTE*) dest;
	BYTE* const oend = op + maxOutputSize;
	BYTE* cpy;

	BYTE token;
	
	U32		dec[4]={0, 3, 2, 3};
	int		len, length;


	// Main Loop
	while (ip<iend)
	{
		// get runlength
		token = *ip++;
		if ((length=(token>>ML_BITS)) == RUN_MASK)  { for (;(len=*ip++)==255;length+=255){} length += len; } 

		// copy literals
		ref = op+length;
		if (ref>oend-4) 
		{ 
			if (ref > oend) goto _output_error;
			while(op<oend-3) { *(U32*)op=*(U32*)ip; op+=4; ip+=4; } 
			while(op<ref) *op++=*ip++; 
			break;    // Necessarily EOF
		}
		do { *(U32*)op = *(U32*)ip; op+=4; ip+=4; } while (op<ref) ;
		ip-=(op-ref); op=ref;	// correction
		if (ip>=iend) break;    // check EOF

		// get offset
		ref -= *(U16*)ip; ip+=2;

		// get matchlength
		if ((length=(token&ML_MASK)) == ML_MASK) { for (;(len=*ip++)==255;length+=255){} length += len; }
		length += MINMATCH;

		// copy repeated sequence
		cpy = op + length;
		if (op-ref<4)
		{
			*op++ = *ref++;
			*op++ = *ref++;
			*op++ = *ref++;
			*op++ = *ref++;
			ref -= dec[op-ref];
		} else { *(U32*)op=*(U32*)ref; op+=4; ref+=4; }
		if (cpy>oend-4)
		{
			if (cpy > oend) goto _output_error;
			while(op<cpy-3) { *(U32*)op=*(U32*)ref; op+=4; ref+=4; }
			while(op<cpy) *op++=*ref++;
			if (op>=oend) break;    // Check EOF
			continue;
		}
		do { *(U32*)op = *(U32*)ref; op+=4; ref+=4; } while (op<cpy) ;
		op=cpy;		// correction
	}

	// end of decoding
	return (int) (((char*)op)-dest);

	// write overflow error detected
_output_error:
	return (int) (-(((char*)ip)-source));
}


