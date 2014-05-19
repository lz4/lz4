/*
   LZ4 - Fast LZ compression algorithm
   Header File
   Copyright (C) 2011-2014, Yann Collet.
   BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

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

   You can contact the author at :
   - LZ4 source repository : http://code.google.com/p/lz4/
   - LZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
*/
#pragma once

#if defined (__cplusplus)
extern "C" {
#endif


/**************************************
   Version
**************************************/
#define LZ4_VERSION_MAJOR    1    /* for major interface/format changes  */
#define LZ4_VERSION_MINOR    2    /* for minor interface/format changes  */
#define LZ4_VERSION_RELEASE  0    /* for tweaks, bug-fixes, or development */


/**************************************
   Tuning parameter
**************************************/
/*
 * LZ4_MEMORY_USAGE :
 * Memory usage formula : N->2^N Bytes (examples : 10 -> 1KB; 12 -> 4KB ; 16 -> 64KB; 20 -> 1MB; etc.)
 * Increasing memory usage improves compression ratio
 * Reduced memory usage can improve speed, due to cache effect
 * Default value is 14, for 16KB, which nicely fits into Intel x86 L1 cache
 */
#define LZ4_MEMORY_USAGE 14


/**************************************
   Simple Functions
**************************************/

int LZ4_compress        (const char* source, char* dest, int inputSize);
int LZ4_decompress_safe (const char* source, char* dest, int compressedSize, int maxOutputSize);

/*
LZ4_compress() :
    Compresses 'inputSize' bytes from 'source' into 'dest'.
    Destination buffer must be already allocated,
    and must be sized to handle worst cases situations (input data not compressible)
    Worst case size evaluation is provided by function LZ4_compressBound()
    inputSize : Max supported value is LZ4_MAX_INPUT_VALUE
    return : the number of bytes written in buffer dest
             or 0 if the compression fails

LZ4_decompress_safe() :
    compressedSize : is obviously the source size
    maxOutputSize : is the size of the destination buffer, which must be already allocated.
    return : the number of bytes decoded in the destination buffer (necessarily <= maxOutputSize)
             If the destination buffer is not large enough, decoding will stop and output an error code (<0).
             If the source stream is detected malformed, the function will stop decoding and return a negative result.
             This function is protected against buffer overflow exploits :
             it never writes outside of output buffer, and never reads outside of input buffer.
             Therefore, it is protected against malicious data packets.
*/


/**************************************
   Advanced Functions
**************************************/
#define LZ4_MAX_INPUT_SIZE        0x7E000000   /* 2 113 929 216 bytes */
#define LZ4_COMPRESSBOUND(isize)  ((unsigned int)(isize) > (unsigned int)LZ4_MAX_INPUT_SIZE ? 0 : (isize) + ((isize)/255) + 16)

/*
LZ4_compressBound() :
    Provides the maximum size that LZ4 may output in a "worst case" scenario (input data not compressible)
    primarily useful for memory allocation of output buffer.
    macro is also provided when result needs to be evaluated at compilation (such as stack memory allocation).

    isize  : is the input size. Max supported value is LZ4_MAX_INPUT_SIZE
    return : maximum output size in a "worst case" scenario
             or 0, if input size is too large ( > LZ4_MAX_INPUT_SIZE)
*/
int LZ4_compressBound(int isize);


/*
LZ4_compress_limitedOutput() :
    Compress 'inputSize' bytes from 'source' into an output buffer 'dest' of maximum size 'maxOutputSize'.
    If it cannot achieve it, compression will stop, and result of the function will be zero.
    This function never writes outside of provided output buffer.

    inputSize  : Max supported value is LZ4_MAX_INPUT_VALUE
    maxOutputSize : is the size of the destination buffer (which must be already allocated)
    return : the number of bytes written in buffer 'dest'
             or 0 if the compression fails
*/
int LZ4_compress_limitedOutput (const char* source, char* dest, int inputSize, int maxOutputSize);


/*
LZ4_decompress_fast() :
    originalSize : is the original and therefore uncompressed size
    return : the number of bytes read from the source buffer (in other words, the compressed size)
             If the source stream is malformed, the function will stop decoding and return a negative result.
             Destination buffer must be already allocated. Its size must be a minimum of 'originalSize' bytes.
    note : This function is a bit faster than LZ4_decompress_safe()
           It provides fast decompression and fully respect memory boundaries for properly formed compressed data.
           It does not provide full protection against intentionnally modified data stream.
           Use this function in a trusted environment (data to decode comes from a trusted source).
*/
int LZ4_decompress_fast (const char* source, char* dest, int originalSize);


/*
LZ4_decompress_safe_partial() :
    This function decompress a compressed block of size 'compressedSize' at position 'source'
    into output buffer 'dest' of size 'maxOutputSize'.
    The function tries to stop decompressing operation as soon as 'targetOutputSize' has been reached,
    reducing decompression time.
    return : the number of bytes decoded in the destination buffer (necessarily <= maxOutputSize)
       Note : this number can be < 'targetOutputSize' should the compressed block to decode be smaller.
             Always control how many bytes were decoded.
             If the source stream is detected malformed, the function will stop decoding and return a negative result.
             This function never writes outside of output buffer, and never reads outside of input buffer. It is therefore protected against malicious data packets
*/
int LZ4_decompress_safe_partial (const char* source, char* dest, int compressedSize, int targetOutputSize, int maxOutputSize);


/*
The following functions are provided should you prefer to allocate table memory using your own allocation methods.
int LZ4_sizeofState();
provides the size to allocate for compression tables.

Tables must be aligned on 4-bytes boundaries, otherwise compression will fail (return code 0).

The allocated memory can be provided to the compressions functions using 'void* state' parameter.
LZ4_compress_withState() and LZ4_compress_limitedOutput_withState() are equivalent to previously described functions.
They just use the externally allocated memory area instead of allocating their own one (on stack, or on heap).
*/
int LZ4_sizeofState(void);
int LZ4_compress_withState               (void* state, const char* source, char* dest, int inputSize);
int LZ4_compress_limitedOutput_withState (void* state, const char* source, char* dest, int inputSize, int maxOutputSize);


/**************************************
   Experimental Streaming Functions
**************************************/

#define LZ4_DICTSIZE_U32 ((1 << (LZ4_MEMORY_USAGE-2)) + 6)
#define LZ4_DICTSIZE     (LZ4_DICTSIZE_U32 * sizeof(unsigned int))
/*
 * LZ4_dict_t
 * information structure to track an LZ4 stream
 * set it to zero (memset()) before first use/
 */
typedef struct { unsigned int table[LZ4_DICTSIZE_U32]; } LZ4_dict_t;

/*
 * LZ4_compress_usingDict
 * Compress data block 'source', using blocks compressed before (with the same function) to improve compression ratio
 * Previous data blocks are assumed to still be present at their previous location.
 */
int LZ4_compress_usingDict (LZ4_dict_t* LZ4_dict, const char* source, char* dest, int inputSize);
//int LZ4_compress_limitedOutput_usingDict (LZ4_dict_t* LZ4_dict, const char* source, char* dest, int inputSize, int maxOutputSize);

/*
 * LZ4_setDictPos
 * If previous data blocks cannot be guaranteed to remain at their previous location in memory
 * save them into a safe place, and
 * use this function to indicate where to find them.
 * Return : 1 if OK, 0 if error
 */
int LZ4_setDictPos (LZ4_dict_t* LZ4_dict, const char* dictionary, int dictSize);

/*
 * LZ4_loadDict
 * Use this function to load a static dictionary into LZ4_dict.
 * It will be used to improve compression of next chained block.
 * Return : 1 if OK, 0 if error
 */
int LZ4_loadDict   (LZ4_dict_t* LZ4_dict, const char* dictionary, int dictSize);


/*
*_usingDict() :
    These decoding functions work the same as their "normal" versions,
    but can also use up to 64KB of dictionary data (dictStart, dictSize)
    to decode chained blocks.
*/
int LZ4_decompress_safe_usingDict (const char* source, char* dest, int compressedSize, int maxOutputSize, const char* dictStart, int dictSize);
int LZ4_decompress_fast_usingDict (const char* source, char* dest, int originalSize, const char* dictStart, int dictSize);

/*
*_withPrefix64k() :
    These decoding functions work the same as their "normal" versions,
    but can also use up to 64KB of data in front of 'char* dest'
    to decode chained blocks.
    The last 64KB of previous block must be present there.
*/
int LZ4_decompress_safe_withPrefix64k (const char* source, char* dest, int compressedSize, int maxOutputSize);
int LZ4_decompress_fast_withPrefix64k (const char* source, char* dest, int originalSize);


/**************************************
   Obsolete Functions
**************************************/
/*
These functions are deprecated and should no longer be used.
They are provided here for compatibility with existing user programs.
*/
int   LZ4_uncompress (const char* source, char* dest, int outputSize);
int   LZ4_uncompress_unknownOutputSize (const char* source, char* dest, int isize, int maxOutputSize);

/* Obsolete streaming functions */
void* LZ4_create (const char* inputBuffer);
int   LZ4_sizeofStreamState(void);
int   LZ4_resetStreamState(void* state, const char* inputBuffer);
int   LZ4_compress_continue (void* LZ4_Data, const char* source, char* dest, int inputSize);
int   LZ4_compress_limitedOutput_continue (void* LZ4_Data, const char* source, char* dest, int inputSize, int maxOutputSize);
char* LZ4_slideInputBuffer (void* LZ4_Data);
int   LZ4_free (void* LZ4_Data);


#if defined (__cplusplus)
}
#endif
