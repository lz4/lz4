#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "lz4.h"
#include "lz4file.h"

const char source[] =
  "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod\n"
  "tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim\n"
  "veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea\n"
  "commodo consequat. Duis aute irure dolor in reprehenderit in voluptate\n"
  "velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat\n"
  "cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id\n"
  "est laborum.\n"
  "\n"
  "Sed ut perspiciatis unde omnis iste natus error sit voluptatem accusantium\n"
  "doloremque laudantium, totam rem aperiam, eaque ipsa quae ab illo inventore\n"
  "veritatis et quasi architecto beatae vitae dicta sunt explicabo. Nemo enim\n"
  "ipsam voluptatem quia voluptas sit aspernatur aut odit aut fugit, sed quia\n"
  "consequuntur magni dolores eos qui ratione voluptatem sequi nesciunt. Neque\n"
  "porro quisquam est, qui dolorem ipsum quia dolor sit amet, consectetur,\n"
  "adipisci velit, sed quia non numquam eius modi tempora incidunt ut labore\n"
  "et dolore magnam aliquam quaerat voluptatem. Ut enim ad minima veniam, quis\n"
  "nostrum exercitationem ullam corporis suscipit laboriosam, nisi ut aliquid\n"
  "ex ea commodi consequatur? Quis autem vel eum iure reprehenderit qui in ea\n"
  "voluptate velit esse quam nihil molestiae consequatur, vel illum qui\n"
  "dolorem eum fugiat quo voluptas nulla pariatur?\n";

#define BUFFER_SIZE 2048

/* Verify LZ4F_writeOpen() frees its buffers when context creation fails. */
static long allocLeakNet = 0;
static int  allocLeakActive = 0;

extern void* __real_malloc(size_t);
extern void* __real_calloc(size_t, size_t);
extern void* __real_realloc(void*, size_t);
extern void  __real_free(void*);

void* __wrap_malloc(size_t n)
{
    void* p = __real_malloc(n);
    if (allocLeakActive && p) allocLeakNet++;
    return p;
}

void* __wrap_calloc(size_t a, size_t b)
{
    void* p = __real_calloc(a, b);
    if (allocLeakActive && p) allocLeakNet++;
    return p;
}

void* __wrap_realloc(void* p, size_t n)
{
    void* q = __real_realloc(p, n);
    if (allocLeakActive) {
        if (p && (n == 0 || q == NULL)) allocLeakNet--;
        if (q && p == NULL) allocLeakNet++;
    }
    return q;
}

void __wrap_free(void* p)
{
    if (allocLeakActive && p) allocLeakNet--;
    __real_free(p);
}

LZ4F_errorCode_t __wrap_LZ4F_createCompressionContext(LZ4F_cctx** LZ4F_compressionContextPtr,
                                                       unsigned version)
{
    (void)LZ4F_compressionContextPtr;
    (void)version;
    return (LZ4F_errorCode_t)(-1);
}

static int checkWriteOpenOOMLeak(void)
{
    LZ4_writeFile_t* wf = NULL;
    LZ4F_errorCode_t r;
    allocLeakActive = 1;
    r = LZ4F_writeOpen(&wf, (FILE*)0x1, NULL);
    allocLeakActive = 0;
    if (!LZ4F_isError(r)) {
        fprintf(stderr, "ERROR: expected LZ4F_writeOpen() to fail (--wrap not active?)\n");
        return 2;
    }
    if (allocLeakNet != 0) {
        fprintf(stderr, "FAIL: LZ4F_writeOpen() leaked %ld allocations on OOM\n", allocLeakNet);
        return 1;
    }
    return 0;
}

int main(void)
{
  { int const rc = checkWriteOpenOOMLeak(); if (rc) return rc; }

  int srcLen = (int)strlen(source);
  size_t const smallSize = 1024;
  size_t const largeSize = 64 * 1024 - 1;
  char cmpBuffer[BUFFER_SIZE];
  char* const buffer = (char*)malloc(BUFFER_SIZE + largeSize);
  char* outBuffer = buffer + largeSize;
  char* const dict = (char*)malloc(largeSize);
  char* const largeDict = dict;
  char* const smallDict = dict + largeSize - smallSize;
  int i;
  int cmpSize;

  printf("starting test decompress-partial-usingDict : \n");
  assert(buffer != NULL);
  assert(dict != NULL);

  cmpSize = LZ4_compress_default(source, cmpBuffer, srcLen, BUFFER_SIZE);

  for (i = cmpSize; i < cmpSize + 10; ++i) {
    int result = LZ4_decompress_safe_partial_usingDict(cmpBuffer, outBuffer, i, srcLen, BUFFER_SIZE, NULL, 0);
    if ( (result < 0)
      || (result != srcLen)
      || memcmp(source, outBuffer, (size_t)srcLen) ) {
      printf("test decompress-partial-usingDict with no dict error \n");
      free(buffer); free(dict); return -1;
    }
  }

  for (i = cmpSize; i < cmpSize + 10; ++i) {
    int result = LZ4_decompress_safe_partial_usingDict(cmpBuffer, outBuffer, i, srcLen, BUFFER_SIZE, outBuffer - smallSize, smallSize);
    if ( (result < 0)
      || (result != srcLen)
      || memcmp(source, outBuffer, (size_t)srcLen) ) {
      printf("test decompress-partial-usingDict with small prefix error \n");
      free(buffer); free(dict); return -1;
    }
  }

  for (i = cmpSize; i < cmpSize + 10; ++i) {
    int result = LZ4_decompress_safe_partial_usingDict(cmpBuffer, outBuffer, i, srcLen, BUFFER_SIZE, buffer, largeSize);
    if ( (result < 0)
      || (result != srcLen)
      || memcmp(source, outBuffer, (size_t)srcLen) ) {
      printf("test decompress-partial-usingDict with large prefix error \n");
      free(buffer); free(dict); return -1;
    }
  }

  for (i = cmpSize; i < cmpSize + 10; ++i) {
    int result = LZ4_decompress_safe_partial_usingDict(cmpBuffer, outBuffer, i, srcLen, BUFFER_SIZE, smallDict, smallSize);
    if ( (result < 0)
      || (result != srcLen)
      || memcmp(source, outBuffer, (size_t)srcLen) ) {
      printf("test decompress-partial-usingDict with small external dict error \n");
      free(buffer); free(dict); return -1;
    }
  }

  for (i = cmpSize; i < cmpSize + 10; ++i) {
    int result = LZ4_decompress_safe_partial_usingDict(cmpBuffer, outBuffer, i, srcLen, BUFFER_SIZE, largeDict, largeSize);
    if ( (result < 0)
      || (result != srcLen)
      || memcmp(source, outBuffer, (size_t)srcLen) ) {
      printf("test decompress-partial-usingDict with large external dict error \n");
      free(buffer); free(dict); return -1;
    }
  }

  printf("test decompress-partial-usingDict OK \n");
  free(buffer); free(dict);
  return 0;
}
