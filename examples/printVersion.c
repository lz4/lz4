#include <stdio.h>
#include "lz4.h"

int main(int argc, char** argv)
{
    printf("Hello, LZ4 (version = %d)\n", LZ4_versionNumber());
    return 0;
}
