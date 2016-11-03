/*
   LZ4 HC - High Compression Mode of LZ4
   Header File
   Copyright (C) 2011-2015, Yann Collet.
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
   - LZ4 source repository : https://github.com/lz4/lz4
   - LZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
*/
#ifndef LZ4_HC_H_19834876238432
#define LZ4_HC_H_19834876238432


#if defined (__cplusplus)
extern "C" {
#endif

/*-***************************
*  Includes
*****************************/
#include <stddef.h>   /* size_t */

/*-***************************************************************
*  Export parameters
*****************************************************************/
/*!
*  LZ4_DLL_EXPORT :
*  Enable exporting of functions when building a Windows DLL
*/
#if defined(_WIN32)
#  if defined(LZ4_DLL_EXPORT) && (LZ4_DLL_EXPORT==1)
#    define LZ4HCLIB_API __declspec(dllexport)
#  else
#    define LZ4HCLIB_API __declspec(dllimport)
#  endif
#else
#  define LZ4HCLIB_API
#endif


#define LZ4HC_MIN_CLEVEL        3
#define LZ4HC_DEFAULT_CLEVEL    9
#define LZ4HC_MAX_CLEVEL        16

/*-************************************
*  Block Compression
**************************************/
/*!
LZ4_compress_HC() :
    Destination buffer `dst` must be already allocated.
    Compression success is guaranteed if `dst` buffer is sized to handle worst circumstances (data not compressible)
    Worst size evaluation is provided by function LZ4_compressBound() (see "lz4.h")
      `srcSize`  : Max supported value is LZ4_MAX_INPUT_SIZE (see "lz4.h")
      `compressionLevel` : Recommended values are between 4 and 9, although any value between 0 and LZ4HC_MAX_CLEVEL will work.
                           0 means "use default value" (see lz4hc.c).
                           Values >LZ4HC_MAX_CLEVEL behave the same as 16.
      @return : the number of bytes written into buffer 'dst'
             or 0 if compression fails.
*/
LZ4HCLIB_API int LZ4_compress_HC (const char* src, char* dst, int srcSize, int maxDstSize, int compressionLevel);


/* Note :
   Decompression functions are provided within "lz4.h" (BSD license)
*/


/*!
LZ4_compress_HC_extStateHC() :
   Use this function if you prefer to manually allocate memory for compression tables.
   To know how much memory must be allocated for the compression tables, use :
      int LZ4_sizeofStateHC();

   Allocated memory must be aligned on 8-bytes boundaries (which a normal malloc() will do properly).

   The allocated memory can then be provided to the compression functions using 'void* state' parameter.
   LZ4_compress_HC_extStateHC() is equivalent to previously described function.
   It just uses externally allocated memory for stateHC.
*/
LZ4HCLIB_API int LZ4_compress_HC_extStateHC(void* state, const char* src, char* dst, int srcSize, int maxDstSize, int compressionLevel);
LZ4HCLIB_API int LZ4_sizeofStateHC(void);


/*-************************************
*  Streaming Compression
**************************************/
#define LZ4_STREAMHCSIZE        262192
#define LZ4_STREAMHCSIZE_SIZET (LZ4_STREAMHCSIZE / sizeof(size_t))
typedef struct { size_t table[LZ4_STREAMHCSIZE_SIZET]; } LZ4_streamHC_t;
/*
  LZ4_streamHC_t
  This structure allows static allocation of LZ4 HC streaming state.
  State must then be initialized using LZ4_resetStreamHC() before first use.

  Static allocation should only be used in combination with static linking.
  If you want to use LZ4 as a DLL, please use construction functions below, which are future-proof.
*/


/*! LZ4_createStreamHC() and LZ4_freeStreamHC() :
  These functions create and release memory for LZ4 HC streaming state.
  Newly created states are already initialized.
  Existing state space can be re-used anytime using LZ4_resetStreamHC().
  If you use LZ4 as a DLL, use these functions instead of static structure allocation,
  to avoid size mismatch between different versions.
*/
LZ4HCLIB_API LZ4_streamHC_t* LZ4_createStreamHC(void);
LZ4HCLIB_API int             LZ4_freeStreamHC (LZ4_streamHC_t* streamHCPtr);

LZ4HCLIB_API void LZ4_resetStreamHC (LZ4_streamHC_t* streamHCPtr, int compressionLevel);
LZ4HCLIB_API int  LZ4_loadDictHC (LZ4_streamHC_t* streamHCPtr, const char* dictionary, int dictSize);

LZ4HCLIB_API int LZ4_compress_HC_continue (LZ4_streamHC_t* streamHCPtr, const char* src, char* dst, int srcSize, int maxDstSize);

LZ4HCLIB_API int LZ4_saveDictHC (LZ4_streamHC_t* streamHCPtr, char* safeBuffer, int maxDictSize);

/*
  These functions compress data in successive blocks of any size, using previous blocks as dictionary.
  One key assumption is that previous blocks (up to 64 KB) remain read-accessible while compressing next blocks.
  There is an exception for ring buffers, which can be smaller than 64 KB.
  Such case is automatically detected and correctly handled by LZ4_compress_HC_continue().

  Before starting compression, state must be properly initialized, using LZ4_resetStreamHC().
  A first "fictional block" can then be designated as initial dictionary, using LZ4_loadDictHC() (Optional).

  Then, use LZ4_compress_HC_continue() to compress each successive block.
  It works like LZ4_compress_HC(), but use previous memory blocks as dictionary to improve compression.
  Previous memory blocks (including initial dictionary when present) must remain accessible and unmodified during compression.
  As a reminder, size 'dst' buffer to handle worst cases, using LZ4_compressBound(), to ensure success of compression operation.

  If, for any reason, previous data blocks can't be preserved unmodified in memory during next compression block,
  you must save it to a safer memory space, using LZ4_saveDictHC().
  Return value of LZ4_saveDictHC() is the size of dictionary effectively saved into 'safeBuffer'.
*/



/*-************************************
*  Deprecated Functions
**************************************/
/* Deprecate Warnings */
/* Should these warnings messages be a problem,
   it is generally possible to disable them,
   with -Wno-deprecated-declarations for gcc
   or _CRT_SECURE_NO_WARNINGS in Visual for example.
   You can also define LZ4_DEPRECATE_WARNING_DEFBLOCK. */
#ifndef LZ4_DEPRECATE_WARNING_DEFBLOCK
#  define LZ4_DEPRECATE_WARNING_DEFBLOCK
#  define LZ4_GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)
#  if (LZ4_GCC_VERSION >= 405) || defined(__clang__)
#    define LZ4_DEPRECATED(message) __attribute__((deprecated(message)))
#  elif (LZ4_GCC_VERSION >= 301)
#    define LZ4_DEPRECATED(message) __attribute__((deprecated))
#  elif defined(_MSC_VER)
#    define LZ4_DEPRECATED(message) __declspec(deprecated(message))
#  else
#    pragma message("WARNING: You need to implement LZ4_DEPRECATED for this compiler")
#    define LZ4_DEPRECATED(message)
#  endif
#endif /* LZ4_DEPRECATE_WARNING_DEFBLOCK */

/* deprecated compression functions */
/* these functions will trigger warning messages in future releases */
LZ4HCLIB_API int LZ4_compressHC                (const char* source, char* dest, int inputSize);
LZ4HCLIB_API int LZ4_compressHC_limitedOutput  (const char* source, char* dest, int inputSize, int maxOutputSize);
LZ4_DEPRECATED("use LZ4_compress_HC() instead") int LZ4_compressHC2 (const char* source, char* dest, int inputSize, int compressionLevel);
LZ4_DEPRECATED("use LZ4_compress_HC() instead") int LZ4_compressHC2_limitedOutput (const char* source, char* dest, int inputSize, int maxOutputSize, int compressionLevel);
LZ4HCLIB_API int LZ4_compressHC_withStateHC               (void* state, const char* source, char* dest, int inputSize);
LZ4HCLIB_API int LZ4_compressHC_limitedOutput_withStateHC (void* state, const char* source, char* dest, int inputSize, int maxOutputSize);
LZ4_DEPRECATED("use LZ4_compress_HC_extStateHC() instead") int LZ4_compressHC2_withStateHC (void* state, const char* source, char* dest, int inputSize, int compressionLevel);
LZ4_DEPRECATED("use LZ4_compress_HC_extStateHC() instead") int LZ4_compressHC2_limitedOutput_withStateHC(void* state, const char* source, char* dest, int inputSize, int maxOutputSize, int compressionLevel);
LZ4HCLIB_API int LZ4_compressHC_continue               (LZ4_streamHC_t* LZ4_streamHCPtr, const char* source, char* dest, int inputSize);
LZ4HCLIB_API int LZ4_compressHC_limitedOutput_continue (LZ4_streamHC_t* LZ4_streamHCPtr, const char* source, char* dest, int inputSize, int maxOutputSize);

/* Deprecated Streaming functions using older model; should no longer be used */
LZ4_DEPRECATED("use LZ4_createStreamHC() instead") void* LZ4_createHC (char* inputBuffer);
LZ4_DEPRECATED("use LZ4_saveDictHC() instead")     char* LZ4_slideInputBufferHC (void* LZ4HC_Data);
LZ4_DEPRECATED("use LZ4_freeStreamHC() instead")   int   LZ4_freeHC (void* LZ4HC_Data);
LZ4_DEPRECATED("use LZ4_compress_HC_continue() instead") int   LZ4_compressHC2_continue (void* LZ4HC_Data, const char* source, char* dest, int inputSize, int compressionLevel);
LZ4_DEPRECATED("use LZ4_compress_HC_continue() instead") int   LZ4_compressHC2_limitedOutput_continue (void* LZ4HC_Data, const char* source, char* dest, int inputSize, int maxOutputSize, int compressionLevel);
LZ4_DEPRECATED("use LZ4_createStreamHC() instead") int   LZ4_sizeofStreamStateHC(void);
LZ4_DEPRECATED("use LZ4_resetStreamHC() instead")  int   LZ4_resetStreamStateHC(void* state, char* inputBuffer);


#if defined (__cplusplus)
}
#endif

#endif /* LZ4_HC_H_19834876238432 */
