/*
 * Regression test for a heap buffer under-allocation in the legacy
 * multi-file compressor (programs/lz4io.c).
 *
 * LZ4IO_compressMultipleFilenames_Legacy() builds the output name as
 * `input + suffix` but sizes the buffer with a fixed `ifnSize + 20`,
 * ignoring suffixSize. Any suffix longer than 19 bytes overflows.
 * Build with AddressSanitizer: with the bug it aborts (overflow); with
 * the fix it compresses successfully (exit 0).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lz4io.h"

/* Internal (not published in lz4io.h) legacy multi-file compressor. */
int LZ4IO_compressMultipleFilenames_Legacy(const char** inFileNamesTable,
                                           int ifntSize,
                                           const char* suffix,
                                           int compressionLevel,
                                           const LZ4IO_prefs_t* prefs);

static int createInput(const char* name)
{
    const char data[] = "LZ4 regression test input data\n";
    FILE* const f = fopen(name, "wb");
    size_t n;
    if (f == NULL)
        return -1;
    n = fwrite(data, 1, sizeof(data) - 1, f);
    fclose(f);
    return (n == sizeof(data) - 1) ? 0 : -1;
}

int main(void)
{
    char suffix[64];
    const char input[] = "tmpsuffix_in";
    LZ4IO_prefs_t* prefs;
    const char* names[1];
    int r;

    memset(suffix, 'x', 31);  /* 31 bytes > 19: triggers the under-allocation */
    suffix[31] = '\0';

    if (createInput(input) != 0) {
        fprintf(stderr, "ERROR: cannot create input file\n");
        return 2;
    }

    prefs = LZ4IO_defaultPreferences();
    names[0] = input;
    r = LZ4IO_compressMultipleFilenames_Legacy(names, 1, suffix, 1, prefs);
    LZ4IO_freePreferences(prefs);

    remove(input);
    {
        char out[sizeof(input) + sizeof(suffix)];
        snprintf(out, sizeof(out), "%s%s", input, suffix);
        remove(out);
    }

    if (r != 0) {
        fprintf(stderr, "ERROR: compress returned %d (unexpected)\n", r);
        return 1;
    }
    return 0;
}
