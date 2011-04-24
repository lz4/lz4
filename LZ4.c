/*
   LZ4 - Fast LZ compression algorithm
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

//**************************************
// Includes
//**************************************
#include <stdlib.h>   // for malloc
#include <string.h>   // for memset
#include "lz4.h"


//**************************************
// Basic Types
//**************************************
#define BYTE	unsigned char
#define U16		unsigned short
#define U32		unsigned long
#define S32		signed long
#define U64		unsigned long long


//**************************************
// Constants
//**************************************
#define MAXTHREADS 32

#define MINMATCH 4
#define INCOMPRESSIBLE 128

#define MAXD_LOG 16
#define MAX_DISTANCE ((1 << MAXD_LOG) - 1)

#define HASH_LOG 17
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
#define HASH_FUNCTION(i)	((i * 2654435761) >> ((MINMATCH*8)-HASH_LOG))
#define HASH_VALUE(p)		HASH_FUNCTION(*(U32*)p)
#define HASH_POINTER(p)		HashTable[HASH_VALUE(p)]



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

	BYTE	*ip = source,      /* input pointer */ 
			*anchor = source,
			*iend = source + isize,
			*ilimit = iend - MINMATCH - 1;

	BYTE	*op = dest,  /* output pointer */
			*ref,
			*orun, *l_end;
	
	int		len, length, sequence, h;
	U32		step=1;
	S32		limit=INCOMPRESSIBLE;


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
		sequence = *(U32*)ip;
		h = HASH_FUNCTION(sequence);
		ref = HashTable[h];
		HashTable[h] = ip;

		// Min Match
		if (( ((ip-ref) >> MAXD_LOG) != 0) || (*(U32*)ref != sequence))
		{ 
			if (ip-anchor>limit) 
			{ 
				limit<<=1; 
				step += 1 + (step>>2); 
			}
			ip+=step; 
			continue; 
		}	

		// catch up
		if (step>1) 
		{ 
			HashTable[h] = ref;
			ip -= (step-1);
			step=1; 
			continue;
		}
		limit=INCOMPRESSIBLE; 

		// Encode Literal length
		len = length = ip - anchor;
		orun=op++;
		if (len>(RUN_MASK-1)) { *orun=(RUN_MASK<<ML_BITS); len-=RUN_MASK; for(; len > 254 ; len-=255) *op++ = 255; *op++ = (BYTE)len; } 
		else *orun = (len<<ML_BITS);

		// Copy Literals
		l_end = op + length;
		while (op<l_end)  { *(U32*)op = *(U32*)anchor; op+=4; anchor+=4; }
		op = l_end;

		// Encode Offset
		*(U16*)op = (ip-ref); op+=2;

		// Start Counting
		ip+=MINMATCH;  ref+=MINMATCH;   // MinMatch verified
		anchor = ip;
		while (*ref == *ip) { ip++; ref++; }   // Ends at *ip!=*ref
		if (ip>iend) ip=iend;
		len = (ip - anchor);
		
		// Encode MatchLength
		if (len>(ML_MASK-1)) { *orun+=ML_MASK; len-=ML_MASK; for(; len > 254 ; len-=255) *op++ = 255; *op++ = (BYTE)len; } 
		else *orun += len;			

		// Prepare next loop
		anchor = ip; 
	}


	// Encode Last Literals
	len = length = iend - anchor;
//	if (length > 0) 
	{
	    orun=op++;
	    if (len>(RUN_MASK-1))
	      { *orun=(RUN_MASK<<ML_BITS); len-=RUN_MASK; for(; len > 254 ; len-=255) *op++ = 255; *op++ = (BYTE) len; } 
		else *orun = (len<<ML_BITS);
		for(;length>0;length-=4) { *(U32*)op = *(U32*)anchor; op+=4; anchor+=4; }
		op += length;    // correction
	}

	// End

	return op-dest;
}



//****************************
// Decompression CODE
//****************************
int LZ4_decode ( char* source, 
				 char* dest,
				 int isize)
{	
	// Local Variables
	BYTE	*ip = source,      
			*iend = source + isize;

	BYTE	*op = dest, 
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
		ref = op+length;
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
		}
		while(op<cpy) { *(U32*)op=*(U32*)ref; op+=4; ref+=4; }
		op=cpy;		// correction
	}

	// end of decoding
	return op-dest;
}



