/*
 * Copyright (c) Yann Collet and LZ4 contributors.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree),
 * meaning you may select, at your option, one of the above-listed licenses.
 */

#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lz4file.h"

int main(void)
{
    FILE* const file = tmpfile();
    LZ4_readFile_t* LZ4F_FILE_SINGLE readFile = NULL;
    LZ4_writeFile_t* LZ4F_FILE_SINGLE writeFile = NULL;
    struct rlimit const noCore = { 0, 0 };
    char input[1] = { 0 };
    int childStatus;
    pid_t child;

    if (file == NULL) return 1;
    if (LZ4F_isError(LZ4F_writeOpen(&writeFile, file, NULL))) return 1;
    if (setrlimit(RLIMIT_CORE, &noCore) != 0) return 1;

    /* The annotated write API must trap before reading past input. */
    child = fork();
    if (child == 0) {
        volatile size_t inputSize = sizeof(input) + 1;
        (void)LZ4F_write(writeFile, input, inputSize);
        _exit(0);
    }
    if (child < 0) return 1;
    if (waitpid(child, &childStatus, 0) != child) return 1;
    if (!WIFSIGNALED(childStatus)) return 1;

    if (LZ4F_write(writeFile, input, sizeof(input)) != sizeof(input)) return 1;
    if (LZ4F_isError(LZ4F_writeClose(writeFile))) return 1;

    rewind(file);
    if (LZ4F_isError(LZ4F_readOpen(&readFile, file))) return 1;

    /* The annotated read API must trap before writing past output. */
    child = fork();
    if (child == 0) {
        char output[1];
        volatile size_t outputSize = sizeof(output) + 1;
        (void)LZ4F_read(readFile, output, outputSize);
        _exit(0);
    }
    if (child < 0) return 1;
    if (waitpid(child, &childStatus, 0) != child) return 1;
    if (!WIFSIGNALED(childStatus)) return 1;

    if (LZ4F_isError(LZ4F_readClose(readFile))) return 1;
    if (fclose(file) != 0) return 1;
    return 0;
}
