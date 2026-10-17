#include "stdio.h"
#include "string.h"
#include "lz4.h"

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
  char cmpBuffer[BUFFER_SIZE];
  char outBuffer[BUFFER_SIZE];
  int cmpSize;
  int i;
  int result;

  cmpSize = LZ4_compress_default(source, cmpBuffer, srcLen, BUFFER_SIZE);

  /* Test full size verification with various bounded capacities */
  for (i = cmpSize; i < cmpSize + 10; ++i) {
    result = LZ4_decompress_safe_partial(cmpBuffer, outBuffer, i, srcLen, BUFFER_SIZE);
    if ((result < 0) || (result != srcLen) || memcmp(source, outBuffer, (size_t)srcLen)) {
      printf("test decompress-partial full length error \n");
      return -1;
    }
  }

  /* Test all target output prefix lengths from 1 to srcLen */
  for (i = 1; i <= srcLen; ++i) {
    memset(outBuffer, 0, sizeof(outBuffer));
    result = LZ4_decompress_safe_partial(cmpBuffer, outBuffer, cmpSize, i, BUFFER_SIZE);
    if (result < i) {
      printf("test decompress-partial under-decompressed error at target=%d, got=%d\n", i, result);
      return -1;
    }
    if (memcmp(source, outBuffer, (size_t)i) != 0) {
      printf("test decompress-partial data mismatch error at target=%d\n", i);
      return -1;
    }
  }

  printf("test decompress-partial OK \n");
  return 0;
}
