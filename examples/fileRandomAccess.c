/* LZ4file API example : random access within a compressed file
 *
 * This example compresses a file with the lz4file API,
 * then reads it back at various positions, using LZ4F_seek() and LZ4F_tell(),
 * and compares the result with the original file.
 *
 * LZ4F_seek() moves the reading position within decoded data, like fseek().
 * Seeking forward decodes and discards data up to the requested position.
 * Seeking backward restarts decoding from the closest resume point,
 * recorded automatically while reading : block starts, within frames using independent blocks.
 * With linked blocks, decoding would restart from the beginning of the frame instead :
 * hence, files meant for random access should use independent blocks. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lz4file.h>


#define CHUNK_SIZE (16*1024)
#define READ_SIZE 1000

/* compress @inpFilename into @lz4Filename, using settings suitable for random access */
static int compress_file(const char* inpFilename, const char* lz4Filename)
{
    FILE* const inpFp = fopen(inpFilename, "rb");
    FILE* const lz4Fp = fopen(lz4Filename, "wb");
    LZ4_writeFile_t* lz4fWrite = NULL;
    LZ4F_preferences_t prefs = LZ4F_INIT_PREFERENCES;
    char buf[CHUNK_SIZE];
    int result = 1;

    /* independent blocks : decoding can restart from any block start.
     * small blocks : less data to decode before reaching the requested position */
    prefs.frameInfo.blockMode = LZ4F_blockIndependent;
    prefs.frameInfo.blockSizeID = LZ4F_max64KB;

    if (inpFp == NULL || lz4Fp == NULL) {
        printf("error: cannot open files \n");
        goto out;
    }
    if (LZ4F_isError(LZ4F_writeOpen(&lz4fWrite, lz4Fp, &prefs))) {
        printf("LZ4F_writeOpen error \n");
        goto out;
    }
    while (1) {
        size_t const len = fread(buf, 1, sizeof(buf), inpFp);
        if (len == 0) break;
        if (LZ4F_isError(LZ4F_write(lz4fWrite, buf, len))) {
            printf("LZ4F_write error \n");
            goto out;
        }
    }
    result = (ferror(inpFp) != 0);

out:
    if (lz4fWrite != NULL && LZ4F_isError(LZ4F_writeClose(lz4fWrite))) result = 1;
    if (inpFp != NULL) fclose(inpFp);
    if (lz4Fp != NULL) fclose(lz4Fp);
    return result;
}

/* reads up to READ_SIZE bytes at the current position of @lz4fRead,
 * and compares them with the same range of the original file @inpFp.
 * @return 0 if identical */
static int read_and_compare(LZ4_readFile_t* lz4fRead, FILE* inpFp)
{
    char expected[READ_SIZE];
    char decoded[READ_SIZE];
    unsigned long long const pos = LZ4F_tell(lz4fRead);
    size_t expectedSize, decodedSize;

    if (fseek(inpFp, (long)pos, SEEK_SET) != 0) return 1;
    expectedSize = fread(expected, 1, READ_SIZE, inpFp);

    decodedSize = LZ4F_read(lz4fRead, decoded, READ_SIZE);
    if (LZ4F_isError(decodedSize)) {
        printf("LZ4F_read error: %s \n", LZ4F_getErrorName(decodedSize));
        return 1;
    }

    printf("read %4u bytes at position %8llu : ", (unsigned)decodedSize, pos);
    if (decodedSize != expectedSize || memcmp(decoded, expected, decodedSize)) {
        printf("corrupted \n");
        return 1;
    }
    printf("OK \n");
    return 0;
}

int main(int argc, const char** argv)
{
    char lz4Filename[256];
    FILE* inpFp = NULL;
    FILE* lz4Fp = NULL;
    LZ4_readFile_t* lz4fRead = NULL;
    long inpSize;
    int nbReads = 10;
    unsigned seed = 1;
    int n, result = 1;

    if (argc < 2) {
        printf("usage: %s FILE [NB_READS] \n", argv[0]);
        return 1;
    }
    if (argc > 2) nbReads = atoi(argv[2]);
    snprintf(lz4Filename, sizeof(lz4Filename), "%s.lz4", argv[1]);

    printf("compress : %s -> %s \n", argv[1], lz4Filename);
    if (compress_file(argv[1], lz4Filename)) return 1;

    inpFp = fopen(argv[1], "rb");
    lz4Fp = fopen(lz4Filename, "rb");
    if (inpFp == NULL || lz4Fp == NULL) {
        printf("error: cannot open files \n");
        goto out;
    }
    if (fseek(inpFp, 0, SEEK_END) != 0 || (inpSize = ftell(inpFp)) < 0) {
        printf("error: cannot determine size of %s \n", argv[1]);
        goto out;
    }
    if (LZ4F_isError(LZ4F_readOpen(&lz4fRead, lz4Fp))) {
        printf("LZ4F_readOpen error \n");
        goto out;
    }

    /* SEEK_SET : moves to a position, from the beginning of decoded data */
    printf("random reads in %s : \n", lz4Filename);
    for (n = 0; n < nbReads; n++) {
        long pos;
        LZ4F_errorCode_t ret;
        seed = seed * 1103515245u + 12345u;
        pos = (long)((seed >> 1) % ((unsigned long)inpSize + 1));
        ret = LZ4F_seek(lz4fRead, pos, SEEK_SET);
        if (LZ4F_isError(ret)) {
            printf("LZ4F_seek error: %s \n", LZ4F_getErrorName(ret));
            goto out;
        }
        if (read_and_compare(lz4fRead, inpFp)) goto out;
    }

    /* SEEK_CUR : moves relative to current position, here backward */
    {   long long const back = (long long)(LZ4F_tell(lz4fRead) / 2);
        LZ4F_errorCode_t const ret = LZ4F_seek(lz4fRead, -back, SEEK_CUR);
        if (LZ4F_isError(ret)) {
            printf("LZ4F_seek error: %s \n", LZ4F_getErrorName(ret));
            goto out;
        }
        printf("moved back by %lld bytes : \n", back);
        if (read_and_compare(lz4fRead, inpFp)) goto out;
    }

    printf("random access : OK \n");
    result = 0;

out:
    if (lz4fRead != NULL) LZ4F_readClose(lz4fRead);
    if (lz4Fp != NULL) fclose(lz4Fp);
    if (inpFp != NULL) fclose(inpFp);
    return result;
}
