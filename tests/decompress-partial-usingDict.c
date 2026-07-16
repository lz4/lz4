#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "lz4.h"
#include "lz4file.h"

/* LZ4_USER_MEMORY_FUNCTIONS makes the library route its allocations through
 * LZ4_malloc/LZ4_calloc/LZ4_free (which the library only *declares*; the user
 * must *define* them globally). This is portable (unlike --wrap, supported only
 * by GNU ld). We use it to track allocations and simulate out-of-memory when
 * opening a file for writing, verifying LZ4F_writeOpen() does not leak. */
size_t allocLeakNet = 0;
int allocLeakFailAfter = -1;  /* -1: never fail */
int allocLeakCount = 0;

void* LZ4_malloc(size_t s) {
    void* p;
    if ((allocLeakFailAfter >= 0) && (++allocLeakCount > allocLeakFailAfter)) {
        return NULL;
    }
    p = malloc(s);
    if (p != NULL) { allocLeakNet++; }
    return p;
}

void* LZ4_calloc(size_t n, size_t s) {
    void* p;
    if ((allocLeakFailAfter >= 0) && (++allocLeakCount > allocLeakFailAfter)) {
        return NULL;
    }
    p = calloc(n, s);
    if (p != NULL) { allocLeakNet++; }
    return p;
}

void LZ4_free(void* p) {
    if (p != NULL) { allocLeakNet--; }
    free(p);
}

/* Open a real temp file, write the source, close it, and verify that the
 * open/write/close cycle leaves no leaked allocations (net == 0). */
static int checkWriteOpenLeak(const char* src, int srcLen) {
    LZ4_writeFile_t* wf = NULL;
    FILE* f;
    size_t r;

    f = tmpfile();
    if (f == NULL) {
        printf("checkWriteOpenLeak: cannot create temp file, skipping\n");
        return 0;
    }

    allocLeakCount = 0;
    allocLeakNet = 0;
    allocLeakFailAfter = -1;  /* no injection: exercise the normal path */
    r = LZ4F_writeOpen(&wf, f, NULL);
    if (LZ4F_isError(r)) {
        printf("LZ4F_writeOpen failed on normal path: %s\n", LZ4F_getErrorName(r));
        fclose(f);
        return -1;
    }
    r = LZ4F_write(wf, src, (size_t)srcLen);
    if (LZ4F_isError(r)) {
        printf("LZ4F_write failed: %s\n", LZ4F_getErrorName(r));
        LZ4F_writeClose(wf);
        fclose(f);
        return -1;
    }
    r = LZ4F_writeClose(wf);
    fclose(f);
    if (LZ4F_isError(r)) {
        printf("LZ4F_writeClose failed: %s\n", LZ4F_getErrorName(r));
        return -1;
    }
    if (allocLeakNet != 0) {
        printf("LZ4F_writeOpen/write/close leaked %d allocation(s)\n", (int)allocLeakNet);
        return -1;
    }
    return 0;
}

/* Simulate an out-of-memory condition during LZ4F_writeOpen() by making the
 * library's failAfter-th allocation fail. The file is a real temp file, so a
 * successful open can never crash. A non-zero net after the failed open means
 * LZ4F_writeOpen() leaked the allocations it had already made. */
static int checkWriteOpenOOMLeak(int failAfter) {
    LZ4_writeFile_t* wf = NULL;
    FILE* f;
    size_t r;

    f = tmpfile();
    if (f == NULL) {
        printf("checkWriteOpenOOMLeak: cannot create temp file, skipping\n");
        return 0;
    }

    allocLeakCount = 0;
    allocLeakNet = 0;
    allocLeakFailAfter = failAfter;  /* fail the (failAfter+1)-th allocation */
    r = LZ4F_writeOpen(&wf, f, NULL);
    allocLeakFailAfter = -1; /* stop failing for the rest of the program */

    /* If no allocation went through our allocator, LZ4_USER_MEMORY_FUNCTIONS
     * did not reach the library objects (e.g. cache reuse without the flag),
     * so the OOM injection cannot take effect. Skip rather than fail. */
    if (allocLeakCount == 0) {
        fclose(f);
        return 0;
    }

    if (!LZ4F_isError(r)) {
        printf("LZ4F_writeOpen should have failed under OOM (failAfter=%d)\n", failAfter);
        if (wf != NULL) { LZ4F_writeClose(wf); }
        fclose(f);
        return -1;
    }
    if (allocLeakNet != 0) {
        printf("LZ4F_writeOpen leaked %d allocation(s) on OOM (failAfter=%d)\n",
               (int)allocLeakNet, failAfter);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

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

int main(void)
{
  int srcLen = (int)strlen(source);
  size_t const smallSize = 1024;
  size_t const largeSize = 64 * 1024 - 1;
  char cmpBuffer[BUFFER_SIZE];
  char* buffer;
  char* outBuffer;
  char* dict;
  char* largeDict;
  char* smallDict;
  int i;
  int cmpSize;

  if (checkWriteOpenLeak(source, srcLen) != 0) {
    return -1;
  }
  if (checkWriteOpenOOMLeak(0) != 0) {
    return -1;
  }
  if (checkWriteOpenOOMLeak(2) != 0) {
    return -1;
  }

  buffer = (char*)malloc(BUFFER_SIZE + largeSize);
  outBuffer = buffer + largeSize;
  dict = (char*)malloc(largeSize);
  largeDict = dict;
  smallDict = dict + largeSize - smallSize;

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
