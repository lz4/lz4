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
// Memory usage formula : N->2^(N+2) Bytes (examples : 17 -> 512KB ; 12 -> 16KB)
#define HASH_LOG 12


//**************************************
// Basic Types
//**************************************
#if defined(_MSC_VER) || defined(_WIN32) || defined(__WIN32__)
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
#define INCOMPRESSIBLE 128

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
	BYTE* hashTable[HASHTABLESIZE];
};


//**************************************
// Macros
//**************************************
#define HASH_FUNCTION(i)	((i * 2654435761U) >> ((MINMATCH*8)-HASH_LOG))
#define HASH_VALUE(p)		HASH_FUNCTION(*(U32*)p)



//****************************
// Compression CODE
//****************************

int LZ4_compress(char* source, 
				 char* dest,
				 int isize)
{
	void* ctx = malloc(sizeof(struct refTables));
	int result = LZ4_compressCtx(&ctx, source, dest, isize);
	free(ctx);

	return result;
}



int LZ4_compressCtx(void** ctx,
				 char* source, 
				 char* dest,
				 int isize)
{	
	struct refTables *srt = (struct refTables *) (*ctx);
	BYTE**  HashTable;

	BYTE	*ip = (BYTE*) source,      /* input pointer */ 
			*anchor = ip,
			*incompressible = anchor + INCOMPRESSIBLE,
			*iend = ip + isize,
			*ilimit = iend - MINMATCH;

	BYTE	*op = (BYTE*) dest,  /* output pointer */
			*ref,
			*orun, *l_end;
	
	int		len, length;
	U32		step=1;


	// Init 
	if (*ctx == NULL) 
	{
		srt = (struct refTables *) malloc ( sizeof(struct refTables) );
		*ctx = (void*) srt;
	}
	HashTable = srt->hashTable;
	memset(HashTable, 0, sizeof(srt->hashTable));

	// Main Loop
	while (ip < ilimit)
	{
		ref = HashTable[HASH_VALUE(ip)];
		HashTable[HASH_VALUE(ip)] = ip;

		// Min Match
		if ((ref < ip - MAX_DISTANCE) || (*(U32*)ref != *(U32*)ip))
		{ 
			if (ip>incompressible) { incompressible += INCOMPRESSIBLE << (step >> 1); step++; }
			ip+=step; 
			continue; 
		}
		step=1;

		// Catch up
		while ((ip>anchor) && (ip[-1]==ref[-1])) { ip--; ref--; }  

		// Encode Literal length
		length = ip - anchor;
		orun = op++;
		if (length>=(int)RUN_MASK) { *orun=(RUN_MASK<<ML_BITS); len = length-RUN_MASK; for(; len > 254 ; len-=255) *op++ = 255; *op++ = (BYTE)len; } 
		else *orun = (length<<ML_BITS);

		// Copy Literals
		l_end = op + length;
		while (op<l_end)  { *(U32*)op = *(U32*)anchor; op+=4; anchor+=4; }
		op = l_end;

		// Encode Offset
		*(U16*)op = (ip-ref); op+=2;

		// Start Counting
		ip+=MINMATCH; ref+=MINMATCH;   // MinMatch verified
		anchor = ip;
		while (ip<(iend-3))
		{
			if (*(U32*)ref == *(U32*)ip) { ip+=4; ref+=4; continue; }
			if (*(U16*)ref == *(U16*)ip) { ip+=2; ref+=2; }
			if (*ref == *ip) ip++;
			goto _endCount;
		}
		if ((ip<(iend-1)) && (*(U16*)ref == *(U16*)ip)) { ip+=2; ref+=2; }
		if ((ip<iend) && (*ref == *ip)) ip++;
_endCount:
		len = (ip - anchor);
		
		// Encode MatchLength
		if (len>=(int)ML_MASK) { *orun+=ML_MASK; len-=ML_MASK; for(; len > 509 ; len-=510) { *op++ = 255; *op++ = 255; } if (len > 254) { len-=255; *op++ = 255; } *op++ = (BYTE)len; } 
		else *orun += len;			

		// Prepare next loop
		anchor = ip; 
		incompressible = anchor + INCOMPRESSIBLE;
	}

	// Encode Last Literals
	len = length = iend - anchor;
	if (length)
	{
		orun=op++;
		if (len>=(int)RUN_MASK) { *orun=(RUN_MASK<<ML_BITS); len-=RUN_MASK; for(; len > 254 ; len-=255) *op++ = 255; *op++ = (BYTE) len; } 
		else *orun = (len<<ML_BITS);
		for(;length>0;length-=4) { *(U32*)op = *(U32*)anchor; op+=4; anchor+=4; }
		op += length;    // correction
	}

	// End
	return (int) (((char*)op)-dest);
}



//****************************
// Decompression CODE
//****************************
int LZ4_uncompress(char* source, 
				 char* dest,
				 int osize)
{	
	// Local Variables
	BYTE	*ip = (BYTE*) source;

	BYTE	*op = (BYTE*) dest, 
			*olimit = op + osize - 4,
			*ref, *cpy,
			runcode;
	
	U32		dec[4]={0, 3, 2, 3};
	int		length;


	// Main Loop
	while (1)
	{
		// get runlength
		runcode = *ip++;
		if ((length=(runcode>>ML_BITS)) == RUN_MASK)  { for (;*ip==255;length+=255) {ip++;} length += *ip++; } 

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
		ip-=(op-ref); op=ref;	// correction

		// get offset
		ref -= *(U16*)ip; ip+=2;

		// get matchlength
		if ((length=(runcode&ML_MASK)) == ML_MASK) { for (;*ip==255;length+=255) {ip++;} length += *ip++; } 
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
	BYTE	*ip = (BYTE*) source,
			*iend = ip + isize;

	BYTE	*op = (BYTE*) dest, 
			*oend = op + maxOutputSize,
			*ref, *cpy,
			runcode;
	
	U32		dec[4]={0, 3, 2, 3};
	int		len, length;


	// Main Loop
	while (ip<iend)
	{
		// get runlength
		runcode = *ip++;
		if ((length=(runcode>>ML_BITS)) == RUN_MASK)  { for (;(len=*ip++)==255;length+=255){} length += len; } 

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
		if ((length=(runcode&ML_MASK)) == ML_MASK) { for (;(len=*ip++)==255;length+=255){} length += len; }
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


//****************************
// Deprecated functions
//****************************
int LZ4_decode ( char* source, 
				 char* dest,
				 int isize)
{	
	// Local Variables
	BYTE	*ip = (BYTE*)source,      
			*iend = ip + isize;

	BYTE	*op = (BYTE*)dest, 
			*ref, *cpy,
			runcode;
	
	U32		dec[4]={0, 3, 2, 3};
	int		len, length;


	// Main Loop
	while (ip < iend)
	{
		// get runlength
		runcode = *ip++;
		if ((length=(runcode>>ML_BITS)) == RUN_MASK)  { for (;(len=*ip++)==255;length+=255){} length += len; } 

		// copy literals
		ref=op+length;
		while (op<ref) { *(U32*)op = *(U32*)ip; op+=4; ip+=4; }
		ip-=(op-ref); op=ref;	// correction
		if (ip>=iend) break;    // Check EOF

		// get offset
		ref -= *(U16*)ip; ip+=2;

		// get matchlength
		if ((length=(runcode&ML_MASK)) == ML_MASK) { for (;(len=*ip++)==255;length+=255){} length += len; } 
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
		while(op<cpy) { *(U32*)op=*(U32*)ref; op+=4; ref+=4; }
		op=cpy;		// correction
	}

	// end of decoding
	return (int) (((char*)op)-dest);
}
