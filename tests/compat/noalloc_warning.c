/*
 * noalloc_warning.c : functions which allocate memory by themselves always fail without allocator
 * (LZ4_STATIC_LINKING_ONLY_DISABLE_MEMORY_ALLOCATION) : invoking them must generate a warning.
 * -DCALL=n selects the invocation (0 : none), see test-disable-memory-allocation in tests/Makefile.
 *
 * Copyright (C) Yann Collet 2026
 * GPL v2 License, see tests/README.md
 */

#include <stddef.h>
#include "lz4hc.h"
#include "lz4frame.h"
#include "lz4file.h"

int invoke(void);
int invoke(void)
{
#if CALL == 1
    return LZ4_createStream() != NULL;
#elif CALL == 2
    return LZ4_createStreamDecode() != NULL;
#elif CALL == 3
    return LZ4_createStreamHC() != NULL;
#elif CALL == 4
    LZ4F_cctx* cctx;
    return LZ4F_isError(LZ4F_createCompressionContext(&cctx, LZ4F_VERSION));
#elif CALL == 5
    LZ4F_dctx* dctx;
    return LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION));
#elif CALL == 6
    return LZ4F_createCDict("", 0) != NULL;
#elif CALL == 7
    LZ4_readFile_t* readFile;
    return LZ4F_isError(LZ4F_readOpen(&readFile, NULL));
#elif CALL == 8
    LZ4_writeFile_t* writeFile;
    return LZ4F_isError(LZ4F_writeOpen(&writeFile, NULL, NULL));
#else
    return 0;
#endif
}
