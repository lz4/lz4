/*
   LZ4 Frame - Auto-framing for LZ4
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
   Framing compression functions
**************************************/

typedef void LZ4F_compressionContext_t;

typedef enum { default=0, max64KB=4, max256KB=5, max1MB=6, max4MB=7} maxBlockSize_t;
typedef enum { default=0, blockLinked, blockIndependent} blockMode_t;
typedef enum { default=0, contentChecksumEnabled, contentNoChecksum} contentChecksum_t;

typedef struct {
  maxBlockSize_t    maxBlockSize;
  blockMode_t       blockMode;
  contentChecksum_t contentChecksum;
} LZ4F_preferences_t;


LZ4F_compressionContext_t* LZ4F_createCompressionContext(void* dstBuffer, size_t dstMaxSize, const LZ4F_preferences_t* preferences);

size_t LZ4F_compressBound(size_t srcSize, const LZ4F_preferences_t* preferences);

size_t LZ4F_compress(void* dstBuffer, size_t dstMaxSize, const void* srcBuffer, size_t srcSize, LZ4F_compressionContext_t* compressionContext);
       
size_t LZ4F_flush(void* dstBuffer, size_t dstMaxSize, LZ4F_compressionContext_t* compressionContext);
       
void   LZ4F_freeCompressionContext(LZ4F_compressionContext_t* compressionContext);




#if defined (__cplusplus)
}
#endif
