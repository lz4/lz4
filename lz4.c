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
#define SKIPSTRENGTH 6
#define STACKLIMIT 13
#define HEAPMODE (HASH_LOG>STACKLIMIT)  // Defines if memory is allocated into the stack (local variable), or into the heap (malloc()).
#define COPYTOKEN 4
#define COPYLENGTH 8
#define LASTLITERALS 5
#define MFLIMIT 12
#define MINLENGTH 13

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
#define LZ4_HASH_FUNCTION(i)	(((i) * 2654435761U) >> ((MINMATCH*8)-HASH_LOG))
#define LZ4_HASH_VALUE(p)		LZ4_HASH_FUNCTION(*(U32*)(p))
#define LZ4_COPYPACKET(s,d)		*(U32*)d = *(U32*)s; d+=4; s+=4; *(U32*)d = *(U32*)s; d+=4; s+=4;
#define LZ4_WILDCOPY(s,d,e)		do { LZ4_COPYPACKET(s,d) } while (d<e);
#define LZ4_BLINDCOPY(s,d,l)	{ BYTE* e=d+l; LZ4_WILDCOPY(s,d,e); d=e; }



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
	const BYTE* const mflimit = iend - MFLIMIT;
#define matchlimit (iend - LASTLITERALS)

	BYTE* op = (BYTE*) dest;
	
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
	HashTable[LZ4_HASH_VALUE(ip)] = ip;
	ip++; forwardH = LZ4_HASH_VALUE(ip);
	
	// Main Loop
    for ( ; ; ) 
	{
		int findMatchAttempts = (1U << skipStrength) + 3;
		const BYTE* forwardIp = ip;
		const BYTE* ref;
		BYTE* token;

		// Find a match
		do {
			U32 h = forwardH;
			int step = findMatchAttempts++ >> skipStrength;
			ip = forwardIp;
			forwardIp = ip + step;

			if (forwardIp > mflimit) { goto _last_literals; }

			forwardH = LZ4_HASH_VALUE(forwardIp);
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
		LZ4_BLINDCOPY(anchor, op, length);


_next_match:
		// Encode Offset
		*(U16*)op = (ip-ref); op+=2;

		// Start Counting
		ip+=MINMATCH; ref+=MINMATCH;   // MinMatch verified
		anchor = ip;
		while (ip<matchlimit-3)
		{
			if (*(U32*)ref == *(U32*)ip) { ip+=4; ref+=4; continue; }
			if (*(U16*)ref == *(U16*)ip) { ip+=2; ref+=2; }
			if (*ref == *ip) ip++;
			goto _endCount;
		}
		if ((ip<(matchlimit-1)) && (*(U16*)ref == *(U16*)ip)) { ip+=2; ref+=2; }
		if ((ip<matchlimit) && (*ref == *ip)) ip++;
_endCount:
		len = (ip - anchor);
		
		// Encode MatchLength
		if (len>=(int)ML_MASK) { *token+=ML_MASK; len-=ML_MASK; for(; len > 509 ; len-=510) { *op++ = 255; *op++ = 255; } if (len > 254) { len-=255; *op++ = 255; } *op++ = (BYTE)len; } 
		else *token += len;	

		// Test end of chunk
		if (ip > mflimit) { anchor = ip;  break; }

		// Test next position
		ref = HashTable[LZ4_HASH_VALUE(ip)];
		HashTable[LZ4_HASH_VALUE(ip)] = ip;
		if ((ref > ip - (MAX_DISTANCE + 1)) && (*(U32*)ref == *(U32*)ip)) { token = op++; *token=0; goto _next_match; }

		// Prepare next loop
		anchor = ip++; 
		forwardH = LZ4_HASH_VALUE(ip);
	}

_last_literals:
	// Encode Last Literals
	{
		int lastRun = iend - anchor;
		if (lastRun>=(int)RUN_MASK) { *op++=(RUN_MASK<<ML_BITS); lastRun-=RUN_MASK; for(; lastRun > 254 ; lastRun-=255) *op++ = 255; *op++ = (BYTE) lastRun; } 
		else *op++ = (lastRun<<ML_BITS);
		memcpy(op, anchor, iend - anchor);
		op += iend-anchor;
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

	BYTE* op = (BYTE*) dest;
	BYTE* const oend = op + osize;
	BYTE* const olimit = op + osize - COPYLENGTH;

	int	dec[4]={0, 3, 2, 3};


	// Main Loop
	while (1)
	{
		int	length;
		BYTE token;
		BYTE* ref;
		BYTE* cpy;

		// get runlength
		token = *ip++;
		if ((length=(token>>ML_BITS)) == RUN_MASK)  { for (;*ip==255;length+=255) {ip++;} length += *ip++; } 

		// copy literals
		ref = op+length;
		if (ref > olimit) 
		{ 
			if (ref > oend) goto _output_error;
			memcpy(op, ip, length);
			break;    // Necessarily EOF
		}
		LZ4_WILDCOPY(ip, op, ref);
		ip -= (op-ref); op = ref;	// correction

		// get offset
		ref -= *(U16*)ip; ip+=2;

		// get matchlength
		if ((length=(token&ML_MASK)) == ML_MASK) { for (;*ip==255;length+=255) {ip++;} length += *ip++; } 

		// copy repeated sequence
		if (op-ref<COPYTOKEN)
		{
			*op++ = *ref++;
			*op++ = *ref++;
			*op++ = *ref++;
			*op++ = *ref++;
			ref -= dec[op-ref];
			*(U32*)op=*(U32*)ref; 
		} else { *(U32*)op=*(U32*)ref; op+=4; ref+=4; }
		cpy = op + length;
		if (cpy > olimit)
		{
			if (cpy > oend) goto _output_error;	
			LZ4_WILDCOPY(ref, op, olimit);
			while(op<cpy) *op++=*ref++;
			op=cpy;
			if (op == oend) break;    // Check EOF (should never happen, since last 5 bytes are supposed to be literals)
			continue;
		}
		LZ4_WILDCOPY(ref, op, cpy);
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


