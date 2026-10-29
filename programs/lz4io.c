/*
  LZ4io.c - LZ4 File/Stream Interface
  Copyright (c) Yann Collet. All rights reserved.

  GPL v2 License

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

  You can contact the author at :
  - LZ4 source repository : https://github.com/lz4/lz4
  - LZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
*/
/*
  Note : this is stand-alone program.
  It is not part of LZ4 compression library, it is a user code of the LZ4 library.
  - The license of LZ4 library is BSD.
  - The license of xxHash library is BSD.
  - The license of this source file is GPLv2.
*/


/*-************************************
*  Compiler options
**************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  pragma warning(disable : 4127)    /* disable: C4127: conditional expression is constant */
#endif
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#  define _DEFAULT_SOURCE          /* madvise(), MADV_* : see LZ4IO_releaseInput() */
#endif
#if defined(__MINGW32__) && !defined(_POSIX_SOURCE)
#  define _POSIX_SOURCE 1          /* disable %llu warnings with MinGW on Windows */
#endif


/*****************************
*  Includes
*****************************/
#include "platform.h"  /* Large File Support, SET_BINARY_MODE, SET_SPARSE_FILE_MODE, PLATFORM_POSIX_VERSION, __64BIT__ */
#include "timefn.h"    /* TIME_ */
#include "util.h"      /* UTIL_getFileStat, UTIL_setFileStat */
#include <stdio.h>     /* fprintf, fopen, fread, stdin, stdout, fflush, getchar */
#include <stdlib.h>    /* malloc, free */
#include <string.h>    /* strerror, strcmp, strlen */
#include <time.h>      /* clock_t, for cpu-time */
#include <sys/types.h> /* stat64 */
#include <sys/stat.h>  /* stat64 */
#include "lz4conf.h"   /* compile-time constants */
#include "lz4io.h"
#include "threadpool.h" /* TPool_* */
#define LZ4_STATIC_LINKING_ONLY  /* LZ4_ACCELERATION_MAX */
#include "lz4.h"       /* required for legacy format */
#include "lz4hc.h"     /* required for legacy format */
#define LZ4F_STATIC_LINKING_ONLY
#include "lz4frame.h"  /* LZ4F_* */
#include "xxhash.h"    /* XXH32 : content checksum of a memory-mapped source */

/* memory-mapped input (see LZ4IO_setMmap()) : POSIX 64-bit systems */
#ifndef LZ4IO_MMAP_SUPPORT
#  if (PLATFORM_POSIX_VERSION >= 1) && defined(__64BIT__)
#    define LZ4IO_MMAP_SUPPORT 1
#  else
#    define LZ4IO_MMAP_SUPPORT 0
#  endif
#endif
#if LZ4IO_MMAP_SUPPORT
#  include <sys/mman.h>  /* mmap, munmap, madvise */
#  include <signal.h>    /* sigaction, SIGBUS */
#  include <unistd.h>    /* write, _exit */
#endif
#if PLATFORM_POSIX_VERSION >= 1
#  include <fcntl.h>     /* open, O_* */
#  include <unistd.h>    /* close */
#endif


/*****************************
*  Constants
*****************************/
#define KB *(1 <<10)
#define MB *(1 <<20)
#define GB *(1U<<30)

#define _1BIT  0x01
#define _2BITS 0x03
#define _3BITS 0x07
#define _4BITS 0x0F
#define _8BITS 0xFF

#define MAGICNUMBER_SIZE    4
#define LZ4IO_MAGICNUMBER   0x184D2204
#define LZ4IO_SKIPPABLE0    0x184D2A50
#define LZ4IO_SKIPPABLEMASK 0xFFFFFFF0
#define LEGACY_MAGICNUMBER  0x184C2102
#define MOZLZ4_MAGIC        "mozLz40"   /* Firefox format, see LZ4IO_decodeMozLZ4() */
#define MOZLZ4_MAGIC_SIZE   sizeof(MOZLZ4_MAGIC)   /* 8 bytes, including the terminating '\0' */

#define CACHELINE 64
#define LEGACY_BLOCKSIZE   (8 MB)
#define MIN_STREAM_BUFSIZE (192 KB)
#define LZ4IO_BLOCKSIZEID_DEFAULT 7
#define LZ4_MAX_DICT_SIZE (64 KB)

#undef MIN
#define MIN(a,b)  ((a)<(b)?(a):(b))
#undef MAX
#define MAX(a,b)  ((a)>(b)?(a):(b))

/**************************************
*  Time and Display
**************************************/
#define DISPLAY(...)         fprintf(stderr, __VA_ARGS__)
#define DISPLAYOUT(...)      fprintf(stdout, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...) if (g_displayLevel>=l) { DISPLAY(__VA_ARGS__); if (g_displayLevel>=4) fflush(stderr); }
static int g_displayLevel = 0;   /* 0 : no display  ; 1: errors  ; 2 : + result + interaction + warnings ; 3 : + progression; 4 : + information */

#define DISPLAYUPDATE(l, ...) if (g_displayLevel>=l) { \
            if ( (TIME_clockSpan_ns(g_time) > refreshRate)  \
              || (g_displayLevel>=4) ) {               \
                g_time = TIME_getTime();               \
                DISPLAY(__VA_ARGS__);                  \
                if (g_displayLevel>=4) fflush(stderr); \
        }   }
static const Duration_ns refreshRate = 200000000;
static TIME_t g_time = { 0 };

/* Gives the cpu time,
 * i.e. the sum of kernel time + user time spent in all active threads since the beginning of the program.
 * When compared with time duration, it gives a sense of cpu load.
 * On Windows: uses GetProcessTimes() counters.
 * On posix: relies on the fact that `clock()` measures cpu time.
 */
static double cpuTime_sec(void)
{
#ifdef _WIN32
    FILETIME creationTime, exitTime, kernelTime, userTime;
    GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime);
    return ( ((double)kernelTime.dwLowDateTime + (double)userTime.dwLowDateTime)
           + ((double)kernelTime.dwHighDateTime + (double)userTime.dwHighDateTime) * (double)(1ULL << 32) )
           * 100. / 1000000000.;
#else
    return (double)clock() / (double)CLOCKS_PER_SEC;
#endif
}

static double cpuTimeSpan_sec(double startTime)
{
    return cpuTime_sec() - startTime;
}

static void LZ4IO_finalTimeDisplay(TIME_t timeStart, double cpuTimeStart, unsigned long long size)
{
#if LZ4IO_MULTITHREAD
    if (!TIME_support_MT_measurements()) {
        DISPLAYLEVEL(5, "time measurements not compatible with multithreading \n");
    } else
#endif
    {
        Duration_ns const duration_ns = TIME_clockSpan_ns(timeStart);
        double const seconds = (double)(duration_ns + !duration_ns) / (double)1000000000.;
        double const cpuLoad_s = cpuTimeSpan_sec(cpuTimeStart);
        DISPLAYLEVEL(3,"Done in %.2f s ==> %.2f MiB/s  (cpu load : %.0f%%)\n", seconds,
                        (double)size / seconds / 1024. / 1024.,
                        (cpuLoad_s / seconds) * 100.);
    }
}

/**************************************
*  Exceptions
***************************************/
#ifndef DEBUG
#  define DEBUG 0
#endif
#define DEBUGOUTPUT(...) if (DEBUG) DISPLAY(__VA_ARGS__);
#define END_PROCESS(error, ...)                                   \
{                                                                 \
    DEBUGOUTPUT("Error in %s, line %i : \n", __FILE__, __LINE__); \
    DISPLAYLEVEL(1, "Error %i : ", error);                        \
    DISPLAYLEVEL(1, __VA_ARGS__);                                 \
    DISPLAYLEVEL(1, " \n");                                       \
    fflush(NULL);                                                 \
    assert(TPool_allJobsAwaited());  /* no job running */         \
    exit(error);                                                  \
}
/* JOB_ERROR() : error met within a job, see TPool_setJobError() */
#define JOB_ERROR(error, ...)                                     \
{                                                                 \
    DISPLAYLEVEL(1, "Error %i : ", error);                        \
    DISPLAYLEVEL(1, __VA_ARGS__);                                 \
    DISPLAYLEVEL(1, " \n");                                       \
    TPool_setJobError(error);                                     \
}
#define END_PROCESS_OR_JOB_ERROR(fromJob, error, ...)             \
{                                                                 \
    if (fromJob) JOB_ERROR(error, __VA_ARGS__)                    \
    else END_PROCESS(error, __VA_ARGS__)                          \
}

#define LZ4IO_STATIC_ASSERT(c)   { enum { LZ4IO_static_assert = 1/(int)(!!(c)) }; }   /* use after variable declarations */


/* ************************************************** */
/* ****************** Init functions ******************** */
/* ************************************************** */

/* nb of worker threads, for frame and legacy formats : capped as within liblz4 (LZ4F_getMaxNbWorkers()) ;
 * 1 when liblz4 doesn't support multi-threading (see lz4 -V) */
static int LZ4IO_capNbWorkers(int nbWorkers)
{
    unsigned const libMax = LZ4F_getMaxNbWorkers();
    unsigned const maxWorkers = libMax ? libMax : 1;
    if (nbWorkers < 1) return 1;
    return ((unsigned)nbWorkers > maxWorkers) ? (int)maxWorkers : nbWorkers;
}

int LZ4IO_defaultNbWorkers(void)
{
#if LZ4IO_MULTITHREAD
    int const nbCores = UTIL_countCores();
    int const spared = 1 + ((unsigned)nbCores >> 3);
    if (nbCores <= spared) return 1;
    return nbCores - spared;
#else
    return 1;
#endif
}

/* ************************************************** */
/* ****************** Parameters ******************** */
/* ************************************************** */

struct LZ4IO_prefs_s {
    int passThrough;
    int overwrite;
    int testMode;
    int blockSizeId;
    size_t blockSize;
    int blockChecksum;
    int streamChecksum;
    int blockIndependence;
    int sparseFileSupport;
    int contentSizeFlag;
    int useDictionary;
    unsigned favorDecSpeed;
    const char* dictionaryFilename;
    int removeSrcFile;
    int nbWorkers;
    int autoWorkers;   /* nbWorkers determined automatically (otherwise : requested, -T#) */
    int singleThread;  /* frames compressed in liblz4's single-threaded mode (nbWorkers == 0) */
    U64 memoryBudget;  /* 0 : default (LZ4IO_MEMORY_BUDGET) */
    int useMmap;
};

void LZ4IO_freePreferences(LZ4IO_prefs_t* prefs)
{
    free(prefs);
}

LZ4IO_prefs_t* LZ4IO_defaultPreferences(void)
{
    LZ4IO_prefs_t* const prefs = (LZ4IO_prefs_t*)malloc(sizeof(*prefs));
    if (!prefs) END_PROCESS(11, "Can't even allocate LZ4IO preferences");
    prefs->passThrough = 0;
    prefs->overwrite = 1;
    prefs->testMode = 0;
    LZ4IO_setBlockSizeID(prefs, LZ4IO_BLOCKSIZEID_DEFAULT);
    prefs->blockChecksum = 0;
    prefs->streamChecksum = 1;
    prefs->blockIndependence = 1;
    prefs->sparseFileSupport = 1;
    prefs->contentSizeFlag = 0;
    prefs->useDictionary = 0;
    prefs->favorDecSpeed = 0;
    prefs->dictionaryFilename = NULL;
    prefs->removeSrcFile = 0;
    prefs->nbWorkers = LZ4IO_capNbWorkers(LZ4IO_defaultNbWorkers());
    prefs->autoWorkers = 1;
    prefs->singleThread = 0;
    prefs->memoryBudget = 0;
    prefs->useMmap = 1;
    return prefs;
}

int LZ4IO_setNbWorkers(LZ4IO_prefs_t* const prefs, int nbWorkers)
{
    prefs->autoWorkers = (nbWorkers == 0);
    if (nbWorkers == 0) nbWorkers = LZ4IO_defaultNbWorkers();
    prefs->nbWorkers = LZ4IO_capNbWorkers(nbWorkers);
    return prefs->nbWorkers;
}

int LZ4IO_setSingleThread(LZ4IO_prefs_t* const prefs, int enable)
{
    prefs->singleThread = (enable != 0);
    return prefs->singleThread;
}

unsigned long long LZ4IO_setMemoryBudget(LZ4IO_prefs_t* const prefs, unsigned long long budget)
{
    prefs->memoryBudget = budget;
    return budget;
}

int LZ4IO_setDictionaryFilename(LZ4IO_prefs_t* const prefs, const char* dictionaryFilename)
{
    prefs->dictionaryFilename = dictionaryFilename;
    prefs->useDictionary = dictionaryFilename != NULL;
    return prefs->useDictionary;
}

/* Default setting : passThrough = 0; return : passThrough mode (0/1) */
int LZ4IO_setPassThrough(LZ4IO_prefs_t* const prefs, int yes)
{
   prefs->passThrough = (yes!=0);
   return prefs->passThrough;
}

/* Default setting : overwrite = 1; return : overwrite mode (0/1) */
int LZ4IO_setOverwrite(LZ4IO_prefs_t* const prefs, int yes)
{
   prefs->overwrite = (yes!=0);
   return prefs->overwrite;
}

/* Default setting : testMode = 0; return : testMode (0/1) */
int LZ4IO_setTestMode(LZ4IO_prefs_t* const prefs, int yes)
{
   prefs->testMode = (yes!=0);
   return prefs->testMode;
}

/* blockSizeID : valid values : 4-5-6-7 */
size_t LZ4IO_setBlockSizeID(LZ4IO_prefs_t* const prefs, unsigned bsid)
{
    static const size_t blockSizeTable[] = { 64 KB, 256 KB, 1 MB, 4 MB };
    static const unsigned minBlockSizeID = 4;
    static const unsigned maxBlockSizeID = 7;
    if ((bsid < minBlockSizeID) || (bsid > maxBlockSizeID)) return 0;
    prefs->blockSizeId = (int)bsid;
    prefs->blockSize = blockSizeTable[(unsigned)prefs->blockSizeId-minBlockSizeID];
    return prefs->blockSize;
}

size_t LZ4IO_setBlockSize(LZ4IO_prefs_t* const prefs, size_t blockSize)
{
    static const size_t minBlockSize = 32;
    static const size_t maxBlockSize = 4 MB;
    unsigned bsid = 0;
    if (blockSize < minBlockSize) blockSize = minBlockSize;
    if (blockSize > maxBlockSize) blockSize = maxBlockSize;
    prefs->blockSize = blockSize;
    blockSize--;
    /* find which of { 64k, 256k, 1MB, 4MB } is closest to blockSize */
    while (blockSize >>= 2)
        bsid++;
    if (bsid < 7) bsid = 7;
    prefs->blockSizeId = (int)(bsid-3);
    return prefs->blockSize;
}

/* Default setting : 1 == independent blocks */
int LZ4IO_setBlockMode(LZ4IO_prefs_t* const prefs, LZ4IO_blockMode_t blockMode)
{
    prefs->blockIndependence = (blockMode == LZ4IO_blockIndependent);
    return prefs->blockIndependence;
}

/* Default setting : 0 == no block checksum */
int LZ4IO_setBlockChecksumMode(LZ4IO_prefs_t* const prefs, int enable)
{
    prefs->blockChecksum = (enable != 0);
    return prefs->blockChecksum;
}

/* Default setting : 1 == checksum enabled */
int LZ4IO_setStreamChecksumMode(LZ4IO_prefs_t* const prefs, int enable)
{
    prefs->streamChecksum = (enable != 0);
    return prefs->streamChecksum;
}

/* Default setting : 0 (no notification) */
int LZ4IO_setNotificationLevel(int level)
{
    g_displayLevel = level;
    return g_displayLevel;
}

/* Default setting : 1 (auto: enabled on file, disabled on stdout) */
int LZ4IO_setSparseFile(LZ4IO_prefs_t* const prefs, int enable)
{
    prefs->sparseFileSupport = 2*(enable!=0);  /* 2==force enable */
    return prefs->sparseFileSupport;
}

/* Default setting : 1 (enabled, when supported) */
int LZ4IO_setMmap(LZ4IO_prefs_t* const prefs, int enable)
{
    prefs->useMmap = (enable != 0);
    return prefs->useMmap;
}

/* Default setting : 0 (disabled) */
int LZ4IO_setContentSize(LZ4IO_prefs_t* const prefs, int enable)
{
    prefs->contentSizeFlag = (enable!=0);
    return prefs->contentSizeFlag;
}

/* Default setting : 0 (disabled) */
void LZ4IO_favorDecSpeed(LZ4IO_prefs_t* const prefs, int favor)
{
    prefs->favorDecSpeed = (favor!=0);
}

void LZ4IO_setRemoveSrcFile(LZ4IO_prefs_t* const prefs, unsigned flag)
{
  prefs->removeSrcFile = (flag>0);
}


/* ************************************************************************ **
** ********************** String functions ********************* **
** ************************************************************************ */

static int LZ4IO_isDevNull(const char* s)
{
    return UTIL_sameString(s, nulmark);
}

static int LZ4IO_isStdin(const char* s)
{
    return UTIL_sameString(s, stdinmark);
}

static int LZ4IO_isStdout(const char* s)
{
    return UTIL_sameString(s, stdoutmark);
}

/* LZ4IO_mustRemoveSrcFile() :
 * `--rm` only applies to a regular source file :
 * not in test mode, which removes nothing (see lz4.1.md),
 * and not when reading from stdin, which is not a file. */
static int LZ4IO_mustRemoveSrcFile(const LZ4IO_prefs_t* prefs, const char* srcFileName)
{
    return prefs->removeSrcFile
        && !prefs->testMode
        && !LZ4IO_isStdin(srcFileName);
}


/* ************************************************************************ **
** ********************** LZ4 File / Pipe compression ********************* **
** ************************************************************************ */

static int LZ4IO_isSkippableMagicNumber(unsigned int magic) {
    return (magic & LZ4IO_SKIPPABLEMASK) == LZ4IO_SKIPPABLE0;
}


/** LZ4IO_openSrcFile() :
 * condition : `srcFileName` must be non-NULL.
 * @result : FILE* to `dstFileName`, or NULL if it fails */
static FILE* LZ4IO_openSrcFile(const char* srcFileName)
{
    FILE* f;

    if (LZ4IO_isStdin(srcFileName)) {
        DISPLAYLEVEL(4,"Using stdin for input \n");
        f = stdin;
        SET_BINARY_MODE(stdin);
        return f;
    }

    if (UTIL_isDirectory(srcFileName)) {
        DISPLAYLEVEL(1, "lz4: %s is a directory -- ignored \n", srcFileName);
        return NULL;
    }

    f = fopen(srcFileName, "rb");
    if (f==NULL) DISPLAYLEVEL(1, "%s: %s \n", srcFileName, strerror(errno));
    return f;
}

/* @return : 1 if @dstFileName designates the file @srcFileName (same name, link, ...) :
 *           opening it for writing would truncate the source before it is read */
static int LZ4IO_isSameFile(const char* srcFileName, const char* dstFileName)
{
#if defined(_WIN32)
    return UTIL_sameString(srcFileName, dstFileName);   /* no inode numbers */
#else
    stat_t srcStat, dstStat;
    return UTIL_getFileStat(srcFileName, &srcStat)
        && UTIL_getFileStat(dstFileName, &dstStat)
        && (srcStat.st_dev == dstStat.st_dev) && (srcStat.st_ino == dstStat.st_ino);
#endif
}

/** FIO_openDstFile() :
 *  prefs is writable, because sparseFileSupport might be updated.
 *  condition : `dstFileName` must be non-NULL.
 * @srcFileName : source whose attributes are copied once `dstFileName` is complete, or NULL
 *                it can't be the destination itself : it would be truncated
 * @result : FILE* to `dstFileName`, or NULL if it fails */
static FILE*
LZ4IO_openDstFile(const char* dstFileName, const LZ4IO_prefs_t* const prefs, const char* srcFileName)
{
    FILE* f;
    assert(dstFileName != NULL);

    if (LZ4IO_isStdout(dstFileName)) {
        DISPLAYLEVEL(4, "Using stdout for output \n");
        f = stdout;
        SET_BINARY_MODE(stdout);
        if (prefs->sparseFileSupport==1) {
            DISPLAYLEVEL(4, "Sparse File Support automatically disabled on stdout ;"
                            " to force-enable it, add --sparse command \n");
        }
    } else {
        if (srcFileName != NULL && !LZ4IO_isStdin(srcFileName) && LZ4IO_isSameFile(srcFileName, dstFileName)) {
            DISPLAYLEVEL(1, "%s: input and output are the same file; not overwritten \n", dstFileName);
            return NULL;
        }
        if (!prefs->overwrite && !LZ4IO_isDevNull(dstFileName)) {
            /* Check if destination file already exists */
            FILE* const testf = fopen( dstFileName, "rb" );
            if (testf != NULL) {  /* dest exists, prompt for overwrite authorization */
                fclose(testf);
                if (g_displayLevel <= 1) {  /* No interaction possible */
                    DISPLAY("%s already exists; not overwritten  \n", dstFileName);
                    return NULL;
                }
                DISPLAY("%s already exists; do you want to overwrite (y/N) ? ", dstFileName);
                {   int ch = getchar();
                    if ((ch!='Y') && (ch!='y')) {
                        DISPLAY("    not overwritten  \n");
                        return NULL;
                    }
                    while ((ch!=EOF) && (ch!='\n')) ch = getchar();  /* flush rest of input line */
        }   }   }
#if PLATFORM_POSIX_VERSION >= 1
        /* a new file stays private until the source's permissions are copied, once it is complete */
        if (srcFileName != NULL && !LZ4IO_isStdin(srcFileName) && !LZ4IO_isDevNull(dstFileName)
          && UTIL_isRegFile(srcFileName)) {
            int const fd = open(dstFileName, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
            f = (fd < 0) ? NULL : fdopen(fd, "wb");
            if (f == NULL && fd >= 0) close(fd);
        } else
#endif
        f = fopen( dstFileName, "wb" );
        if (f==NULL) DISPLAYLEVEL(1, "%s: %s\n", dstFileName, strerror(errno));
    }

    /* sparse file */
    {   int const sparseMode = (prefs->sparseFileSupport - (f==stdout)) > 0;
        if (f && sparseMode) { SET_SPARSE_FILE_MODE(f); }
    }

    return f;
}


/***************************************
*   MT I/O
***************************************/

/* LZ4IO_waitJobs() :
 * waits for all jobs of both pools, then exits with the first error met by a job, if any */
static void LZ4IO_waitJobs(TPool* tPool, TPool* wPool)
{
    TPool_jobsCompleted(tPool);
    TPool_jobsCompleted(wPool);
    if (TPool_jobError()) {
        fflush(NULL);
        exit(TPool_jobError());
    }
}

typedef struct {
    void* buf;
    size_t size;
    unsigned long long rank;
} BufferDesc;

typedef struct {
    unsigned long long expectedRank;
    BufferDesc* buffers;
    size_t capacity;
    size_t blockSize;
    unsigned long long totalCSize;
} WriteRegister;

static void WR_destroy(WriteRegister* wr)
{
    free(wr->buffers);
}

#define WR_INITIAL_BUFFER_POOL_SIZE 16
/* Note: WR_init() can fail (allocation)
 * check that wr->buffers!= NULL for success */
static WriteRegister WR_init(size_t blockSize)
{
    WriteRegister wr = { 0, NULL, WR_INITIAL_BUFFER_POOL_SIZE, 0, 0 };
    wr.buffers = (BufferDesc*)calloc(1, WR_INITIAL_BUFFER_POOL_SIZE * sizeof(BufferDesc));
    wr.blockSize = blockSize;
    return wr;
}

static void WR_addBufDesc(WriteRegister* wr, const BufferDesc* bd)
{
    if (wr->buffers[wr->capacity-1].buf != NULL) {
        /* buffer capacity is full : extend it */
        size_t const oldCapacity = wr->capacity;
        size_t const addedCapacity = MIN(oldCapacity, 256);
        size_t const newCapacity = oldCapacity + addedCapacity;
        size_t const newSize = newCapacity * sizeof(BufferDesc);
        void* const newBuf = realloc(wr->buffers, newSize);
        if (newBuf == NULL) {
            END_PROCESS(39, "cannot extend register of buffers")
        }
        wr->buffers = (BufferDesc*)newBuf;
        memset(wr->buffers + oldCapacity, 0, addedCapacity * sizeof(BufferDesc));
        wr->buffers[oldCapacity] = bd[0];
        wr->capacity = newCapacity;
    } else {
        /* at least one position (the last one) is free, i.e. buffer==NULL */
        size_t n;
        for (n=0; n<wr->capacity; n++) {
            if (wr->buffers[n].buf == NULL) {
                wr->buffers[n] = bd[0];
                break;
            }
        }
        assert(n != wr->capacity);
    }
}

static int WR_isPresent(WriteRegister* wr, unsigned long long id)
{
    size_t n;
    for (n=0; n<wr->capacity; n++) {
        if (wr->buffers[n].buf == NULL) {
            /* no more buffers stored */
            return 0;
        }
        if (wr->buffers[n].rank == id)
            return 1;
    }
    return 0;
}

/* Note: requires @id to exist! */
static BufferDesc WR_getBufID(WriteRegister* wr, unsigned long long id)
{
    size_t n;
    for (n=0; n<wr->capacity; n++) {
        if (wr->buffers[n].buf == NULL) {
            /* no more buffers stored */
            break;
        }
        if (wr->buffers[n].rank == id)
            return wr->buffers[n];
    }
    END_PROCESS(41, "buffer ID not found");
}

static void WR_removeBuffID(WriteRegister* wr, unsigned long long id)
{
    size_t n;
    for (n=0; n<wr->capacity; n++) {
        if (wr->buffers[n].buf == NULL) {
            /* no more buffers stored */
            return;
        }
        if (wr->buffers[n].rank == id) {
            free(wr->buffers[n].buf);
            break;
        }
    }
    /* overwrite buffer descriptor, scale others down*/
    n++;
    for (; n < wr->capacity; n++) {
        wr->buffers[n-1] = wr->buffers[n];
        if (wr->buffers[n].buf == NULL)
            return;
    }
    {   BufferDesc const nullBd = { NULL, 0, 0 };
        wr->buffers[wr->capacity-1] = nullBd;
    }
}

typedef struct {
    WriteRegister* wr;
    void* cBuf;
    size_t cSize;
    unsigned long long blockNb;
    FILE* out;
} WriteJobDesc;

static void LZ4IO_writeBuffer(BufferDesc bufDesc, FILE* out)
{
    size_t const size = bufDesc.size;
    if (TPool_jobError()) return;   /* nothing more to write */
    if (fwrite(bufDesc.buf, 1, size, out) != size) {
        JOB_ERROR(38, "Write error : cannot write compressed block");
    }
}

static void LZ4IO_checkWriteOrder(void* arg)
{
    WriteJobDesc* const wjd = (WriteJobDesc*)arg;
    size_t const cSize = wjd->cSize;
    WriteRegister* const wr = wjd->wr;

    if (wjd->blockNb != wr->expectedRank) {
        /* incorrect order : let's store this buffer for later write */
        BufferDesc bd;
        bd.buf = wjd->cBuf;
        bd.size = wjd->cSize;
        bd.rank = wjd->blockNb;
        WR_addBufDesc(wr, &bd);
        free(wjd);  /* because wjd is pod */
        return;
    }

    /* expected block ID : let's write this block */
    {   BufferDesc bd;
        bd.buf = wjd->cBuf;
        bd.size = wjd->cSize;
        bd.rank = wjd->blockNb;
        LZ4IO_writeBuffer(bd, wjd->out);
    }
    wr->expectedRank++;
    wr->totalCSize += cSize;
    free(wjd->cBuf);
    /* and check for more blocks, previously saved */
    while (WR_isPresent(wr, wr->expectedRank)) {
        BufferDesc const bd = WR_getBufID(wr, wr->expectedRank);
        LZ4IO_writeBuffer(bd, wjd->out);
        wr->totalCSize += bd.size;
        WR_removeBuffID(wr, wr->expectedRank);
        wr->expectedRank++;
    }
    free(wjd);  /* because wjd is pod */
    {   unsigned long long const processedSize = (unsigned long long)(wr->expectedRank-1) * wr->blockSize;
        DISPLAYUPDATE(2, "\rRead : %u MiB   ==> %.2f%%   ",
                (unsigned)(processedSize >> 20),
                (double)wr->totalCSize / (double)processedSize * 100.);
    }
}

typedef size_t (*compress_f)(
    const void* parameters,
    void* dst,
    size_t dstCapacity,
    const void* src,
    size_t srcSize);

typedef struct {
    TPool* wpool;
    void* buffer;
    size_t inSize;
    unsigned long long blockNb;
    compress_f compress;
    const void* compressParameters;
    FILE* fout;
    WriteRegister* wr;
    size_t maxCBlockSize;
} CompressJobDesc;

static void LZ4IO_compressChunk(void* arg)
{
    CompressJobDesc* const cjd = (CompressJobDesc*)arg;
    size_t const outCapacity = cjd->maxCBlockSize;
    void* const out_buff = malloc(outCapacity);
    if (!out_buff) {
        JOB_ERROR(33, "Allocation error : can't allocate output buffer to compress new chunk");
        return;
    }
    {   size_t const cSize = cjd->compress(cjd->compressParameters, out_buff, outCapacity, cjd->buffer, cjd->inSize);

        /* check for write */
        {   WriteJobDesc* const wjd = (WriteJobDesc*)malloc(sizeof(*wjd));
            if (wjd == NULL) {
                JOB_ERROR(35, "Allocation error : can't describe new write job");
                free(out_buff);
                return;
            }
            wjd->cBuf = out_buff;
            wjd->cSize = (size_t)cSize;
            wjd->blockNb = cjd->blockNb;
            wjd->out = cjd->fout;
            wjd->wr = cjd->wr;
            TPool_submitJob(cjd->wpool, LZ4IO_checkWriteOrder, wjd);
    }   }
}

static void LZ4IO_compressAndFreeChunk(void* arg)
{
    CompressJobDesc* const cjd = (CompressJobDesc*)arg;
    if (!TPool_jobError()) LZ4IO_compressChunk(arg);
    /* clean up */
    free(cjd->buffer);
    free(cjd); /* because cjd is pod */
}

/* one ReadTracker per file to compress */
typedef struct {
    TPool* tPool;
    TPool* wpool;
    FILE* fin;
    size_t chunkSize;
    unsigned long long totalReadSize;
    unsigned long long blockNb;
    compress_f compress;
    const void* compressParameters;
    FILE* fout;
    WriteRegister* wr;
    size_t maxCBlockSize;
} ReadTracker;

static void LZ4IO_readAndProcess(void* arg)
{
    ReadTracker* const rjd = (ReadTracker*)arg;
    size_t const chunkSize = rjd->chunkSize;
    void* const buffer = malloc(chunkSize);
    if (!buffer) {
        JOB_ERROR(31, "Allocation error : can't allocate buffer to read new chunk");
        return;
    }
    {   size_t const inSize = fread(buffer, (size_t)1, chunkSize, rjd->fin);
        if (inSize > chunkSize) {
            JOB_ERROR(32, "Read error (read %u > %u [chunk size])", (unsigned)inSize, (unsigned)chunkSize);
            free(buffer);
            return;
        }
        rjd->totalReadSize += inSize;
        /* special case: nothing left: stop read operation */
        if (inSize == 0) {
            free(buffer);
            return;
        }
        /* process read input */
        {   CompressJobDesc* const cjd = (CompressJobDesc*)malloc(sizeof(*cjd));
            if (cjd==NULL) {
                JOB_ERROR(33, "Allocation error : can't describe new compression job");
                free(buffer);
                return;
            }
            cjd->wpool = rjd->wpool;
            cjd->buffer = buffer; /* transfer ownership */
            cjd->inSize = inSize;
            cjd->blockNb = rjd->blockNb;
            cjd->compress = rjd->compress;
            cjd->compressParameters = rjd->compressParameters;
            cjd->fout = rjd->fout;
            cjd->wr = rjd->wr;
            cjd->maxCBlockSize = rjd->maxCBlockSize;
            TPool_submitJob(rjd->tPool, LZ4IO_compressAndFreeChunk, cjd);
            if ((inSize == chunkSize) && !TPool_jobError()) {
                /* likely more => read another chunk */
                rjd->blockNb++;
                TPool_submitJob(rjd->tPool, LZ4IO_readAndProcess, rjd);
    }   }   }
}


/***************************************
*   Legacy Compression
***************************************/

/* Size in bytes of a legacy block header in little-endian format */
#define LZ4IO_LEGACY_BLOCK_HEADER_SIZE 4
#define LZ4IO_LEGACY_BLOCK_SIZE_MAX  (8 MB)

/* unoptimized version; solves endianness & alignment issues */
static void LZ4IO_writeLE32 (void* p, unsigned value32)
{
    unsigned char* const dstPtr = (unsigned char*)p;
    dstPtr[0] = (unsigned char)value32;
    dstPtr[1] = (unsigned char)(value32 >> 8);
    dstPtr[2] = (unsigned char)(value32 >> 16);
    dstPtr[3] = (unsigned char)(value32 >> 24);
}


typedef struct {
    int cLevel;
} CompressLegacyState;

/* same as LZ4F_levelToAcceleration() : capped at LZ4_ACCELERATION_MAX, without overflow */
static int LZ4IO_levelToAcceleration(int cLevel)
{
    if (cLevel >= 0) return 1;
    if (cLevel < 1 - LZ4_ACCELERATION_MAX) return LZ4_ACCELERATION_MAX;
    return -cLevel + 1;
}

static size_t LZ4IO_compressBlockLegacy_fast(
    const void* params,
    void* dst,
    size_t dstCapacity,
    const void* src,
    size_t srcSize
)
{
    const CompressLegacyState* const clevel = (const CompressLegacyState*)params;
    int const acceleration = LZ4IO_levelToAcceleration(clevel->cLevel);
    int const cSize = LZ4_compress_fast((const char*)src, (char*)dst + LZ4IO_LEGACY_BLOCK_HEADER_SIZE, (int)srcSize, (int)dstCapacity, acceleration);
    if (cSize < 0)
        END_PROCESS(51, "fast compression failed");
    LZ4IO_writeLE32(dst, (unsigned)cSize);
    return (size_t) cSize + LZ4IO_LEGACY_BLOCK_HEADER_SIZE;
}

static size_t LZ4IO_compressBlockLegacy_HC(
    const void* params,
    void* dst,
    size_t dstCapacity,
    const void* src,
    size_t srcSize
)
{
    const CompressLegacyState* const cs = (const CompressLegacyState*)params;
    int const clevel = cs->cLevel;
    int const cSize = LZ4_compress_HC((const char*)src, (char*)dst + LZ4IO_LEGACY_BLOCK_HEADER_SIZE, (int)srcSize, (int)dstCapacity, clevel);
    if (cSize < 0)
        END_PROCESS(52, "HC compression failed");
    LZ4IO_writeLE32(dst, (unsigned)cSize);
    return (size_t) cSize + LZ4IO_LEGACY_BLOCK_HEADER_SIZE;
}

/* LZ4IO_finishCompressedFile() :
 * Once the compressed file is complete : close files,
 * copy owner, permissions and modification time of the source file,
 * remove it with --rm, and display final status.
 * A source modified (@srcModified) or renamed or replaced (@srcReplaced) during compression is kept,
 * and a replaced one doesn't give its attributes */
static void
LZ4IO_finishCompressedFile(FILE* srcFile, FILE* dstFile,
                           const char* srcFileName, const char* dstFileName,
                           unsigned long long srcSize, unsigned long long cSize,
                           int srcModified, int srcReplaced,
                           const LZ4IO_prefs_t* io_prefs)
{
    fclose(srcFile);
    if (!LZ4IO_isStdout(dstFileName)) fclose(dstFile);  /* do not close stdout */

    /* Copy owner, file permissions and modification time */
    {   stat_t statbuf;
        if (!LZ4IO_isStdin(srcFileName)
         && !LZ4IO_isStdout(dstFileName)
         && !LZ4IO_isDevNull(dstFileName)
         && !srcReplaced
         && UTIL_getFileStat(srcFileName, &statbuf)) {
            UTIL_setFileStat(dstFileName, &statbuf);
    }   }

    if (LZ4IO_mustRemoveSrcFile(io_prefs, srcFileName) && !srcModified && !srcReplaced) {  /* remove source file : --rm */
        if (remove(srcFileName))
            END_PROCESS(50, "Remove error : %s: %s", srcFileName, strerror(errno));
    }

    /* Final Status */
    DISPLAYLEVEL(2, "\r%79s\r", "");
    DISPLAYLEVEL(2, "Compressed %llu bytes into %llu bytes ==> %.2f%%\n",
                    srcSize, cSize,
                    (double)cSize / (double)(srcSize + !srcSize /* avoid division by zero */ ) * 100.);
}

/* LZ4IO_compressLegacy_internal :
 * Implementation of LZ4IO_compressFilename_Legacy.
 * @return: 0 if success, !0 if error
 */
static int LZ4IO_compressLegacy_internal(unsigned long long* readSize,
                                  const char* input_filename,
                                  const char* output_filename,
                                  int compressionlevel,
                                  const LZ4IO_prefs_t* prefs)
{
    int clResult = 0;
    compress_f const compressionFunction = (compressionlevel < LZ4HC_CLEVEL_MIN) ? LZ4IO_compressBlockLegacy_fast : LZ4IO_compressBlockLegacy_HC;
    FILE* const finput = LZ4IO_openSrcFile(input_filename);
    FILE* foutput = NULL;
    TPool* const tPool = TPool_create(prefs->nbWorkers, 4);
    TPool* const wPool = TPool_create(1, 4);
    WriteRegister wr = WR_init(LEGACY_BLOCKSIZE);

    /* Init & checks */
    *readSize = 0;
    if (finput == NULL) {
        /* read file error : recoverable */
        clResult = 1;
        goto _cfl_clean;
    }
    foutput = LZ4IO_openDstFile(output_filename, prefs, input_filename);
    if (foutput == NULL) {
        /* write file error : recoverable */
        fclose(finput);
        clResult = 1;
        goto _cfl_clean;
    }
    if (tPool == NULL || wPool == NULL)
        END_PROCESS(21, "threadpool creation error ");
    if (wr.buffers == NULL)
        END_PROCESS(22, "can't allocate write register");


    /* Write Archive Header */
    {   char outHeader[MAGICNUMBER_SIZE];
        LZ4IO_writeLE32(outHeader, LEGACY_MAGICNUMBER);
        if (fwrite(outHeader, 1, MAGICNUMBER_SIZE, foutput) != MAGICNUMBER_SIZE)
            END_PROCESS(23, "Write error : cannot write header");
    }
    wr.totalCSize = MAGICNUMBER_SIZE;

    {   CompressLegacyState cls;
        ReadTracker rjd;
        cls.cLevel = compressionlevel;
        rjd.tPool = tPool;
        rjd.wpool = wPool;
        rjd.fin = finput;
        rjd.chunkSize = LEGACY_BLOCKSIZE;
        rjd.totalReadSize = 0;
        rjd.blockNb = 0;
        rjd.compress = compressionFunction;
        rjd.compressParameters = &cls;
        rjd.fout = foutput;
        rjd.wr = &wr;
        rjd.maxCBlockSize = (size_t)LZ4_compressBound(LEGACY_BLOCKSIZE) + LZ4IO_LEGACY_BLOCK_HEADER_SIZE;
        /* Ignite the job chain */
        TPool_submitJob(tPool, LZ4IO_readAndProcess, &rjd);
        LZ4IO_waitJobs(tPool, wPool);

        LZ4IO_finishCompressedFile(finput, foutput, input_filename, output_filename,
                                   rjd.totalReadSize, wr.totalCSize, 0, 0, prefs);
        *readSize = rjd.totalReadSize;
    }

    /* Free */
_cfl_clean:
    WR_destroy(&wr);
    TPool_free(wPool);
    TPool_free(tPool);

    return clResult;
}

/* LZ4IO_compressFilename_Legacy :
 * This function is intentionally "hidden" (not published in .h)
 * It generates compressed streams using the old 'legacy' format
 * @return: 0 if success, !0 if error
 */
int LZ4IO_compressFilename_Legacy(const char* input_filename,
                                  const char* output_filename,
                                  int compressionlevel,
                                  const LZ4IO_prefs_t* prefs)
{
    TIME_t const timeStart = TIME_getTime();
    double const cpuStart = cpuTime_sec();
    unsigned long long processed = 0;
    int r = LZ4IO_compressLegacy_internal(&processed, input_filename, output_filename, compressionlevel, prefs);
    LZ4IO_finalTimeDisplay(timeStart, cpuStart, processed);
    return r;
}

/* LZ4IO_reserveFileName() :
 * ensures that *bufferPtr can store a file name of @nameLength characters
 * @return : *bufferPtr, or NULL if allocation fails */
static char* LZ4IO_reserveFileName(char** bufferPtr, size_t* capacityPtr, size_t nameLength)
{
    if (*capacityPtr <= nameLength) {
        free(*bufferPtr);
        *capacityPtr = nameLength + 20;
        *bufferPtr = (char*)malloc(*capacityPtr);
        if (*bufferPtr == NULL) *capacityPtr = 0;
    }
    return *bufferPtr;
}

/* LZ4IO_compressedFileName() :
 * @return : name of the compressed file : @srcFileName followed by @suffix,
 *           or stdoutmark when @suffix is stdout,
 *           or NULL if allocation fails */
static const char*
LZ4IO_compressedFileName(char** bufferPtr, size_t* capacityPtr, const char* srcFileName, const char* suffix)
{
    size_t const srcLength = strlen(srcFileName);
    size_t const suffixLength = strlen(suffix);
    if (LZ4IO_isStdout(suffix)) return stdoutmark;
    if (LZ4IO_reserveFileName(bufferPtr, capacityPtr, srcLength + suffixLength) == NULL) return NULL;
    memcpy(*bufferPtr, srcFileName, srcLength);
    memcpy(*bufferPtr + srcLength, suffix, suffixLength + 1);
    return *bufferPtr;
}

/* LZ4IO_compressMultipleFilenames_Legacy :
 * This function is intentionally "hidden" (not published in .h)
 * It generates multiple compressed streams using the old 'legacy' format */
int LZ4IO_compressMultipleFilenames_Legacy(
                            const char** inFileNamesTable, int ifntSize,
                            const char* suffix,
                            int compressionLevel, const LZ4IO_prefs_t* prefs)
{
    TIME_t const timeStart = TIME_getTime();
    double const cpuStart = cpuTime_sec();
    unsigned long long totalProcessed = 0;
    int i;
    int missed_files = 0;
    char* nameBuffer = NULL;
    size_t nameCapacity = 0;

    /* loop on each file */
    for (i=0; i<ifntSize; i++) {
        unsigned long long processed = 0;
        const char* const dstFileName = LZ4IO_compressedFileName(&nameBuffer, &nameCapacity, inFileNamesTable[i], suffix);
        if (dstFileName == NULL) return ifntSize;   /* not enough memory */
        missed_files += LZ4IO_compressLegacy_internal(&processed,
                                inFileNamesTable[i], dstFileName,
                                compressionLevel, prefs);
        totalProcessed += processed;
    }

    /* Close & Free */
    LZ4IO_finalTimeDisplay(timeStart, cpuStart, totalProcessed);
    free(nameBuffer);

    return missed_files;
}

/*********************************************
*  Compression using Frame format
*********************************************/
typedef struct {
    void*  srcBuffer;
    size_t srcBufferSize;
    void*  dstBuffer;
    size_t dstBufferSize;
    LZ4F_compressionContext_t ctx;
    LZ4F_preferences_t preparedPrefs;
    LZ4F_CDict* cdict;
    unsigned nbWorkers;   /* automatic nb of workers employed so far : only grows (see LZ4IO_compressFilename_extRess()) */
} cRess_t;

static void LZ4IO_freeCResources(cRess_t ress)
{
    free(ress.srcBuffer);
    free(ress.dstBuffer);

    LZ4F_freeCDict(ress.cdict);
    ress.cdict = NULL;

    { LZ4F_errorCode_t const errorCode = LZ4F_freeCompressionContext(ress.ctx);
      if (LZ4F_isError(errorCode)) END_PROCESS(35, "Error : can't free LZ4F context resource : %s", LZ4F_getErrorName(errorCode)); }
}

static void* LZ4IO_createDict(size_t* dictSize, const char* dictFilename)
{
    size_t readSize;
    size_t dictEnd = 0;
    size_t dictLen = 0;
    size_t dictStart;
    size_t circularBufSize = LZ4_MAX_DICT_SIZE;
    char* circularBuf = (char*)malloc(circularBufSize);
    char* dictBuf;
    FILE* dictFile;

    if (!dictFilename)
        END_PROCESS(26, "Dictionary error : no filename provided");
    if (!circularBuf)
        END_PROCESS(25, "Allocation error : not enough memory for circular buffer");

    dictFile = LZ4IO_openSrcFile(dictFilename);
    if (!dictFile)
        END_PROCESS(27, "Dictionary error : could not open dictionary file");

    /* opportunistically seek to the part of the file we care about.
     * If this fails it's not a problem since we'll just read everything anyways. */
    if (!LZ4IO_isStdin(dictFilename)) {
        (void)UTIL_fseek(dictFile, -LZ4_MAX_DICT_SIZE, SEEK_END);
    }

    do {
        readSize = fread(circularBuf + dictEnd, 1, circularBufSize - dictEnd, dictFile);
        dictEnd = (dictEnd + readSize) % circularBufSize;
        dictLen += readSize;
    } while (readSize>0);

    if (dictLen > LZ4_MAX_DICT_SIZE) {
        dictLen = LZ4_MAX_DICT_SIZE;
    }

    *dictSize = dictLen;

    dictStart = (circularBufSize + dictEnd - dictLen) % circularBufSize;

    if (dictStart == 0) {
        /* We're in the simple case where the dict starts at the beginning of our circular buffer. */
        dictBuf = circularBuf;
        circularBuf = NULL;
    } else {
        /* Otherwise, we will alloc a new buffer and copy our dict into that. */
        dictBuf = (char *)malloc(dictLen ? dictLen : 1);
        if (!dictBuf) END_PROCESS(28, "Allocation error : not enough memory");

        memcpy(dictBuf, circularBuf + dictStart, circularBufSize - dictStart);
        memcpy(dictBuf + circularBufSize - dictStart, circularBuf, dictLen - (circularBufSize - dictStart));
    }

    fclose(dictFile);
    free(circularBuf);

    return dictBuf;
}

static LZ4F_CDict* LZ4IO_createCDict(const LZ4IO_prefs_t* io_prefs)
{
    size_t dictionarySize;
    void* dictionaryBuffer;
    LZ4F_CDict* cdict;
    if (!io_prefs->useDictionary) return NULL;
    dictionaryBuffer = LZ4IO_createDict(&dictionarySize, io_prefs->dictionaryFilename);
    if (!dictionaryBuffer) END_PROCESS(29, "Dictionary error : could not create dictionary");
    cdict = LZ4F_createCDict(dictionaryBuffer, dictionarySize);
    free(dictionaryBuffer);
    return cdict;
}

/* LZ4IO_customBlockSize() :
 * @return 1 if blocks must be cut at a size different from LZ4F's standard ones */
static int LZ4IO_customBlockSize(const LZ4IO_prefs_t* io_prefs)
{
    return io_prefs->blockSize != LZ4F_getBlockSize((LZ4F_blockSizeID_t)io_prefs->blockSizeId);
}

size_t LZ4IO_framePreferences(const LZ4IO_prefs_t* io_prefs, LZ4F_preferences_t* framePrefs)
{
    memset(framePrefs, 0, sizeof(*framePrefs));
    framePrefs->frameInfo.blockMode = (LZ4F_blockMode_t)io_prefs->blockIndependence;
    framePrefs->frameInfo.blockSizeID = (LZ4F_blockSizeID_t)io_prefs->blockSizeId;
    framePrefs->frameInfo.blockChecksumFlag = (LZ4F_blockChecksum_t)io_prefs->blockChecksum;
    framePrefs->frameInfo.contentChecksumFlag = (LZ4F_contentChecksum_t)io_prefs->streamChecksum;
    framePrefs->frameInfo.contentSize = (unsigned long long)(io_prefs->contentSizeFlag != 0);
    framePrefs->favorDecSpeed = io_prefs->favorDecSpeed;
    if (!LZ4IO_customBlockSize(io_prefs)) return 0;
    framePrefs->autoFlush = 1;
    return io_prefs->blockSize;
}

/* Memory budget of frame compression : limits the automatic nb of workers,
 * and explicit ones on 32-bit systems, where address space is scarce */
#ifndef LZ4IO_MEMORY_BUDGET
#  define LZ4IO_MEMORY_BUDGET ((sizeof(size_t) < 8) ? ((U64)1 << 30) : ((U64)4 << 30))
#endif

/* size of liblz4 compression jobs (see LZ4F_mtJobSize()) : 1 MB, or a single block when larger */
static size_t LZ4IO_jobSize(const LZ4F_preferences_t* prefs)
{
    size_t const bs = LZ4F_getBlockSize(prefs->frameInfo.blockSizeID);
    size_t const blockSize = LZ4F_isError(bs) ? 4 MB : bs;
    if (blockSize >= 1 MB) return blockSize;
    return ((1 MB) / blockSize) * blockSize;
}

/* estimated memory of frame compression with @nbWorkers :
 * liblz4's job slots, input buffers and LZ4 states, plus the CLI's own buffers */
static U64 LZ4IO_compressionMemory(const LZ4F_preferences_t* prefs, unsigned nbWorkers, size_t srcBufferSize)
{
    LZ4F_preferences_t p = *prefs;
    size_t const jobSize = LZ4IO_jobSize(prefs);
    U64 const outCapacity = (U64)jobSize + (jobSize / (64 KB) + 1) * 8;   /* + block headers and checksums */
    p.nbWorkers = nbWorkers;
    return (2 * (U64)nbWorkers + 1) * outCapacity                 /* job slots */
         + ((U64)nbWorkers + 1) * (64 KB + jobSize)               /* input buffers */
         + ((U64)nbWorkers + 1) * (U64)LZ4_sizeofStateHC()        /* one LZ4 state per thread (worst case : HC) */
         + LZ4F_compressBound(srcBufferSize, &p) + srcBufferSize; /* CLI buffers */
}

unsigned long long LZ4IO_defaultMemoryBudget(void) { return LZ4IO_MEMORY_BUDGET; }

/* @return : largest nb of workers, up to @nbWorkers, within @budget (at least 1) */
static unsigned LZ4IO_budgetNbWorkers(const LZ4F_preferences_t* prefs, unsigned nbWorkers, size_t srcBufferSize, U64 budget)
{
    unsigned lo = 1, hi = MAX(nbWorkers, 1);   /* memory grows with nbWorkers */
    while (lo < hi) {
        unsigned const mid = lo + (hi - lo + 1) / 2;
        if (LZ4IO_compressionMemory(prefs, mid, srcBufferSize) <= budget) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

static cRess_t LZ4IO_createCResources(const LZ4IO_prefs_t* io_prefs)
{
    const size_t chunkSize = 4 MB;
    cRess_t ress;
    memset(&ress, 0, sizeof(ress));

    /* set compression advanced parameters */
    LZ4IO_framePreferences(io_prefs, &ress.preparedPrefs);
    ress.preparedPrefs.frameInfo.contentSize = 0;   /* set for each file */
    if (LZ4IO_customBlockSize(io_prefs)) {
        /* LZ4F cuts blocks at standard sizes only :
         * custom sizes require autoFlush, and one LZ4F_compressUpdate() invocation per block,
         * so compression is single-threaded */
#if LZ4IO_MULTITHREAD
        DISPLAYLEVEL(3, "Note : custom block size (%u bytes) : single-threaded compression (-T# ignored) \n",
                        (unsigned)io_prefs->blockSize);
#endif
    } else if (io_prefs->singleThread) {
        DISPLAYLEVEL(3, "Single-threaded compression (frames not cut into jobs) \n");
    } else {
        /* frames are cut into jobs, compressed by liblz4's worker threads (when available) :
         * output doesn't depend on nbWorkers, nor on multi-threading support */
        unsigned const requested = (unsigned)io_prefs->nbWorkers;   /* >= 1, capped (LZ4IO_capNbWorkers()) */
        U64 const budget = io_prefs->memoryBudget ? io_prefs->memoryBudget : LZ4IO_MEMORY_BUDGET;
        unsigned const nbWorkers = LZ4IO_budgetNbWorkers(&ress.preparedPrefs, requested, chunkSize, budget);
        if (nbWorkers < requested) {
            if (io_prefs->autoWorkers) {
                DISPLAYLEVEL(3, "Memory budget (%u MB) : %u threads instead of %u \n",
                                (unsigned)(budget >> 20), nbWorkers, requested);
            } else {   /* requested nb of threads (-T#) not honored */
                DISPLAYLEVEL(2, "Warning : memory budget (%u MB) : %u threads instead of %u (see --memory) \n",
                                (unsigned)(budget >> 20), nbWorkers, requested);
        }   }
        if (LZ4IO_compressionMemory(&ress.preparedPrefs, nbWorkers, chunkSize) > budget)
            DISPLAYLEVEL(2, "Warning : memory budget (%u MB) too small : compression requires ~%u MB \n",
                            (unsigned)(budget >> 20),
                            (unsigned)(LZ4IO_compressionMemory(&ress.preparedPrefs, nbWorkers, chunkSize) >> 20));
#if LZ4IO_MULTITHREAD
        if (LZ4F_getMaxNbWorkers()) {
            DISPLAYLEVEL(3, "Using up to %u threads for compression \n", nbWorkers);
        } else {
            DISPLAYLEVEL(3, "Note : liblz4 built without multithreading support : single-threaded compression \n");
        }
#endif
        ress.preparedPrefs.nbWorkers = nbWorkers;
    }

    /* Allocate compression state */
    {   LZ4F_errorCode_t const errorCode = LZ4F_createCompressionContext(&(ress.ctx), LZ4F_VERSION);
        if (LZ4F_isError(errorCode))
            END_PROCESS(30, "Allocation error : can't create LZ4F context : %s", LZ4F_getErrorName(errorCode));
    }
    assert(ress.ctx != NULL);

    /* Allocate Buffers */
    ress.srcBuffer = malloc(chunkSize);
    ress.srcBufferSize = chunkSize;
    assert(io_prefs->blockSize > 0);
    /* single-block frames, or streaming by chunks (output can lag behind input) */
    ress.dstBufferSize = MAX(LZ4F_compressFrameBound(io_prefs->blockSize, &ress.preparedPrefs),
                             LZ4F_compressBound(chunkSize, &ress.preparedPrefs));
    ress.dstBuffer = malloc(ress.dstBufferSize);
    if (!ress.srcBuffer || !ress.dstBuffer)
        END_PROCESS(31, "Allocation error : can't allocate buffers");

    ress.cdict = LZ4IO_createCDict(io_prefs);

    return ress;
}

/* Input of frame compression : read by chunks into a buffer,
 * or memory-mapped (regular files), so that compression jobs read it in place (LZ4F_srcStableUntilFlush) */
typedef struct {
    FILE* file;
    void* buffer;
    char* map;          /* NULL : not memory-mapped */
    size_t mapSize;
    size_t pos;         /* consumed (memory-mapped input) */
    size_t released;    /* memory-mapped input released (memory hint) */
#if LZ4IO_MMAP_SUPPORT
    struct sigaction prevBusAction;   /* restored once unmapped */
#endif
} LZ4IO_cInput_t;

#if LZ4IO_MMAP_SUPPORT
/* Reading a memory-mapped file truncated meanwhile, or failing (I/O error), raises SIGBUS,
 * in the thread accessing it (main thread, or liblz4 worker thread) :
 * it's reported as an error, instead of a crash. */
#  define LZ4IO_MMAP_ERROR 53
static char g_mmapErrorMsg[512];
static size_t g_mmapErrorMsgSize = 0;

static volatile sig_atomic_t g_mmapFault = 0;

/* several threads can fault simultaneously : only the first one reports */
static int LZ4IO_firstMmapFault(void)
{
#  if defined(__GNUC__)
    return __sync_lock_test_and_set(&g_mmapFault, 1) == 0;
#  else
    int const first = !g_mmapFault;   /* best effort : the message may be repeated */
    g_mmapFault = 1;
    return first;
#  endif
}

static void LZ4IO_mmapFaultHandler(int sig)
{
    /* async-signal-safe functions only */
    (void)sig;
    if (!LZ4IO_firstMmapFault()) {
        for (;;) pause();   /* the first faulting thread terminates the process */
    }
    if (g_mmapErrorMsgSize) {
        ssize_t const r = write(STDERR_FILENO, g_mmapErrorMsg, g_mmapErrorMsgSize);
        (void)r;
    }
    _exit(LZ4IO_MMAP_ERROR);
}
#endif

/* maps a regular file ; leaves @in unmapped otherwise, or on failure */
static void LZ4IO_mapInput(LZ4IO_cInput_t* in, const char* srcFileName)
{
#if LZ4IO_MMAP_SUPPORT
    int const fd = fileno(in->file);
    struct stat st;
    struct sigaction busAction;
    void* map;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_size <= 0)) return;
    if ((unsigned long long)st.st_size > (size_t)-1) return;

    /* message prepared beforehand : the handler can't format it */
    {   int const n = snprintf(g_mmapErrorMsg, sizeof(g_mmapErrorMsg),
                        "Error %i : %s : file truncated or unreadable during compression (memory-mapped) ; retry with --no-mmap \n",
                        LZ4IO_MMAP_ERROR, srcFileName);
        g_mmapErrorMsgSize = ((g_displayLevel >= 1) && (n > 0)) ? MIN((size_t)n, sizeof(g_mmapErrorMsg) - 1) : 0;
    }
    memset(&busAction, 0, sizeof(busAction));
    busAction.sa_handler = LZ4IO_mmapFaultHandler;
    sigemptyset(&busAction.sa_mask);
    if (sigaction(SIGBUS, &busAction, &in->prevBusAction)) return;   /* no handler : no mapping */

    map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        (void)sigaction(SIGBUS, &in->prevBusAction, NULL);
        return;
    }
#  ifdef MADV_SEQUENTIAL
    (void)madvise(map, (size_t)st.st_size, MADV_SEQUENTIAL);
#  endif
    in->map = (char*)map;
    in->mapSize = (size_t)st.st_size;
#else
    (void)in; (void)srcFileName;
#endif
}

static void LZ4IO_unmapInput(LZ4IO_cInput_t* in)
{
#if LZ4IO_MMAP_SUPPORT
    if (in->map) {
        munmap(in->map, in->mapSize);
        (void)sigaction(SIGBUS, &in->prevBusAction, NULL);
    }
#endif
    in->map = NULL;
}

/* @return : nb of bytes available at *ptr, up to @size ; 0 means end of input */
static size_t LZ4IO_readInput(LZ4IO_cInput_t* in, size_t size, const void** ptr)
{
    if (in->map) {
        size_t const n = MIN(size, in->mapSize - in->pos);
        *ptr = in->map + in->pos;
        in->pos += n;
        return n;
    }
    *ptr = in->buffer;
    return fread(in->buffer, (size_t)1, size, in->file);
}

/* releases memory-mapped input more than @lag bytes behind :
 * only a memory hint, since released pages are read again from the file if still needed (ex: by a job in flight) */
static void LZ4IO_releaseInput(LZ4IO_cInput_t* in, size_t lag)
{
#if LZ4IO_MMAP_SUPPORT && defined(__linux__) && defined(MADV_DONTNEED)
    if (in->map && (in->pos > lag)) {
        size_t const end = ((in->pos - lag) / (1 MB)) * (1 MB);   /* page aligned */
        if (end > in->released) {
            (void)madvise(in->map + in->released, end - in->released, MADV_DONTNEED);
            in->released = end;
        }
    }
#else
    (void)in; (void)lag;
#endif
}

/* A source file modified during its compression is detected by comparing its size and times
 * before and after (like tar : "file changed as we read it").
 * Input read once : the frame holds the content read, ex: a log file growing meanwhile (warning).
 * Memory-mapped input is read several times : the frame is consistent only if the mapped content is unchanged
 * (ex: file only grown), verified with the frame's content checksum (warning) ; otherwise, error. */
typedef struct {
    int valid;      /* regular file */
    stat_t st;
} LZ4IO_srcState_t;

static LZ4IO_srcState_t LZ4IO_getSrcState(FILE* srcFile)
{
    LZ4IO_srcState_t s;
    memset(&s, 0, sizeof(s));
    s.valid = UTIL_getFDStat(UTIL_fileno(srcFile), &s.st);
    return s;
}

static int LZ4IO_srcModified(const LZ4IO_srcState_t* before, FILE* srcFile)
{
    LZ4IO_srcState_t const after = LZ4IO_getSrcState(srcFile);
    if (!before->valid) return 0;   /* not a regular file : can't tell */
    if (!after.valid) return 1;
    if ( (before->st.st_size != after.st.st_size)
      || (before->st.st_mtime != after.st.st_mtime)
      || (before->st.st_ctime != after.st.st_ctime) )
        return 1;
#if !defined(_WIN32) && (PLATFORM_POSIX_VERSION >= 200809L)
    if ( (before->st.st_mtim.tv_nsec != after.st.st_mtim.tv_nsec)
      || (before->st.st_ctim.tv_nsec != after.st.st_ctim.tv_nsec) )
        return 1;
#endif
    return 0;
}

/* @return : 1 if @srcFileName doesn't designate the compressed file anymore (ex: log rotation),
 *           so that --rm would remove another file */
static int LZ4IO_srcReplaced(const LZ4IO_srcState_t* s, const char* srcFileName)
{
#if defined(_WIN32)
    /* no inode numbers, and st_dev differs between _stat() and _fstat() :
     * but an open file can't be renamed nor removed there */
    (void)s; (void)srcFileName;
    return 0;
#else
    stat_t st;
    if (!s->valid || LZ4IO_isStdin(srcFileName)) return 0;
    if (!UTIL_getFileStat(srcFileName, &st)) return 1;   /* removed, or not a regular file anymore */
    return (st.st_dev != s->st.st_dev) || (st.st_ino != s->st.st_ino);
#endif
}

/* @return : 1 if the mapped content still matches the frame's content checksum (stored in its last 4 bytes),
 *           0 if it differs, or can't be verified (no checksum, or file shrunk : mapped content can't be read) */
static int LZ4IO_mappedContentUnchanged(const LZ4IO_cInput_t* in, FILE* srcFile,
                                        int contentChecksum, const void* frameEnd)
{
    stat_t st;
    if (!contentChecksum || (frameEnd == NULL)) return 0;
    if (!UTIL_getFDStat(UTIL_fileno(srcFile), &st) || ((unsigned long long)st.st_size < in->mapSize)) return 0;
    {   const unsigned char* const c = (const unsigned char*)frameEnd - 4;
        unsigned const frameChecksum = (unsigned)c[0] | ((unsigned)c[1] << 8) | ((unsigned)c[2] << 16) | ((unsigned)c[3] << 24);
        return XXH32(in->map, in->mapSize, 0) == frameChecksum;
    }
}

/* keeps the last 4 bytes of the frame (content checksum, see LZ4IO_mappedContentUnchanged()) */
static void LZ4IO_keepFrameTail(unsigned char tail[4], const void* out, size_t size)
{
    const unsigned char* const o = (const unsigned char*)out;
    if (size >= 4) {
        memcpy(tail, o + size - 4, 4);
    } else {
        memmove(tail, tail + size, 4 - size);
        memcpy(tail + 4 - size, o, size);
    }
}

/* LZ4IO_compressFrame() :
 * Compress @input into @dstFile as one frame.
 * @return : compressed size, *srcSizePtr : size of input, and @frameTail : last 4 bytes of the frame */
static unsigned long long
LZ4IO_compressFrame(unsigned long long* srcSizePtr, unsigned char frameTail[4],
                    const cRess_t* ress, LZ4IO_cInput_t* input, FILE* dstFile,
                    const char* srcFileName, LZ4F_preferences_t prefs,
                    const LZ4IO_prefs_t* const io_prefs)
{
    unsigned long long filesize = 0;
    unsigned long long compressedfilesize = 0;
    void* const dstBuffer = ress->dstBuffer;
    const size_t dstBufferSize = ress->dstBufferSize;
    const size_t blockSize = io_prefs->blockSize;
    /* custom block sizes : one block per invocation (see LZ4IO_createCResources()) */
    const size_t chunkSize = LZ4IO_customBlockSize(io_prefs) ? blockSize : ress->srcBufferSize;
    /* memory-mapped input is released (only a memory hint) behind input still read by liblz4 :
     * up to 2*nbWorkers+1 jobs in flight (see LZ4F_MT_NBSLOTS() in lz4frame.c), and the 64 KB of history before them */
    size_t const releaseLag = (2 * (size_t)prefs.nbWorkers + 1) * LZ4IO_jobSize(&prefs) + 64 KB;
    size_t readSize;
    LZ4F_compressionContext_t ctx = ress->ctx;   /* just a pointer */
    LZ4F_compressOptions_t cOpts;
    const void* chunk;

    memset(&cOpts, 0, sizeof(cOpts));
    /* mapping remains until the end of frame : worker threads read it in place.
     * Not in single-threaded mode, where output of linked blocks depends on stableSrc : same output as input read with fread() */
    cOpts.stableSrc = (input->map && prefs.nbWorkers) ? LZ4F_srcStableUntilFlush : LZ4F_srcUnstable;

    /* read first chunk */
    readSize  = LZ4IO_readInput(input, chunkSize, &chunk);
    if (ferror(input->file)) END_PROCESS(40, "Error reading %s ", srcFileName);
    filesize += readSize;

    /* single-block file */
    if (readSize < blockSize) {
        /* Compress in single pass */
        size_t const cSize = LZ4F_compressFrame_usingCDict(ctx, dstBuffer, dstBufferSize, chunk, readSize, ress->cdict, &prefs);
        if (LZ4F_isError(cSize))
            END_PROCESS(41, "Compression failed : %s", LZ4F_getErrorName(cSize));
        LZ4IO_keepFrameTail(frameTail, dstBuffer, cSize);
        compressedfilesize = cSize;
        DISPLAYUPDATE(2, "\rRead : %u MiB   ==> %.2f%%   ",
                      (unsigned)(filesize>>20), (double)compressedfilesize/(double)(filesize+!filesize)*100);   /* avoid division by zero */

        /* Write Block */
        if (fwrite(dstBuffer, 1, cSize, dstFile) != cSize) {
            END_PROCESS(42, "Write error : failed writing single-block compressed frame");
    }   }

    else

    /* multiple-blocks file */
    {
        /* Write Frame Header */
        size_t const headerSize = LZ4F_compressBegin_usingCDict(ctx, dstBuffer, dstBufferSize, ress->cdict, &prefs);
        if (LZ4F_isError(headerSize))
            END_PROCESS(43, "File header generation failed : %s", LZ4F_getErrorName(headerSize));
        if (fwrite(dstBuffer, 1, headerSize, dstFile) != headerSize)
            END_PROCESS(44, "Write error : cannot write header");
        compressedfilesize += headerSize;

        /* Main Loop - one chunk at a time */
        while (readSize>0) {
            size_t const outSize = LZ4F_compressUpdate(ctx, dstBuffer, dstBufferSize, chunk, readSize, &cOpts);
            if (LZ4F_isError(outSize))
                END_PROCESS(45, "Compression failed : %s", LZ4F_getErrorName(outSize));
            compressedfilesize += outSize;
            DISPLAYUPDATE(2, "\rRead : %u MiB   ==> %.2f%%   ",
                        (unsigned)(filesize>>20),
                        (double)compressedfilesize / (double)filesize * 100.);

            /* Write Block */
            if (fwrite(dstBuffer, 1, outSize, dstFile) != outSize)
                END_PROCESS(46, "Write error : cannot write compressed block");

            /* Read next chunk */
            LZ4IO_releaseInput(input, releaseLag);
            readSize  = LZ4IO_readInput(input, chunkSize, &chunk);
            filesize += readSize;
        }
        if (ferror(input->file)) END_PROCESS(47, "Error reading %s ", srcFileName);

        /* End of Frame : jobs in flight, then end mark and checksum, in several pieces when they fill dstBuffer */
        {   size_t endSize;
            do {
                endSize = LZ4F_compressEnd(ctx, dstBuffer, dstBufferSize, NULL);
                if (LZ4F_isError(endSize))
                    END_PROCESS(48, "End of frame error : %s", LZ4F_getErrorName(endSize));
                if (fwrite(dstBuffer, 1, endSize, dstFile) != endSize)
                    END_PROCESS(49, "Write error : cannot write end of frame");
                compressedfilesize += endSize;
                LZ4IO_keepFrameTail(frameTail, dstBuffer, endSize);
            } while (endSize == dstBufferSize);
        }
    }

    *srcSizePtr = filesize;
    return compressedfilesize;
}

/*
 * LZ4IO_compressFilename_extRess()
 * result : 0 : compression completed correctly
 *          1 : missing or pb opening srcFileName
 */
static int
LZ4IO_compressFilename_extRess(unsigned long long* inStreamSize,
                               cRess_t* ress,
                               const char* srcFileName, const char* dstFileName,
                               int compressionLevel,
                               const LZ4IO_prefs_t* const io_prefs)
{
    unsigned long long srcSize, cSize;
    LZ4F_preferences_t prefs;
    LZ4IO_cInput_t input;
    LZ4IO_srcState_t srcState;
    int srcModified, srcReplaced;
    unsigned char frameTail[4];    /* last bytes of the frame (content checksum) */
    FILE* dstFile;
    FILE* const srcFile = LZ4IO_openSrcFile(srcFileName);
    *inStreamSize = 0;
    if (srcFile == NULL) return 1;
    srcState = LZ4IO_getSrcState(srcFile);
    dstFile = LZ4IO_openDstFile(dstFileName, io_prefs, srcFileName);
    if (dstFile == NULL) { fclose(srcFile); return 1; }

    /* Input : memory-mapped when possible (frames cut into jobs only, so that output doesn't depend on it) */
    memset(&input, 0, sizeof(input));
    input.file = srcFile;
    input.buffer = ress->srcBuffer;
    if (io_prefs->useMmap && !LZ4IO_customBlockSize(io_prefs) && !LZ4IO_isStdin(srcFileName))
        LZ4IO_mapInput(&input, srcFileName);

    /* Adjust compression parameters */
    prefs = ress->preparedPrefs;
    prefs.compressionLevel = compressionLevel;
    if (io_prefs->contentSizeFlag) {
      U64 const fileSize = UTIL_getOpenFileSize(srcFile);
      prefs.frameInfo.contentSize = fileSize;   /* == 0 if input == stdin */
      if (fileSize==0)
          DISPLAYLEVEL(3, "Warning : cannot determine input content size \n");
    }
    /* no more workers than the jobs of the file (output doesn't depend on nbWorkers),
     * but never decreasing from one file to the next, so that worker threads and buffers are kept */
    if (prefs.nbWorkers > 1) {
        U64 const fileSize = UTIL_getOpenFileSize(srcFile);   /* 0 : unknown */
        U64 const jobSize = LZ4IO_jobSize(&prefs);
        U64 const nbJobs = MAX((fileSize + jobSize - 1) / jobSize, ress->nbWorkers);
        if (fileSize) {
            if (nbJobs < prefs.nbWorkers) prefs.nbWorkers = (unsigned)nbJobs;
            ress->nbWorkers = prefs.nbWorkers;
    }   }
    /* with independent blocks, a file fitting into a single job is compressed by the calling thread,
     * without job buffers : same output, and worker threads remain available for next files.
     * Not with linked blocks : single-threaded output can then depend on stableSrc (memory-mapped input) */
    if (prefs.nbWorkers && (prefs.frameInfo.blockMode == LZ4F_blockIndependent)) {
        U64 const fileSize = UTIL_getOpenFileSize(srcFile);   /* 0 : unknown (ex: stdin) */
        if (fileSize && (fileSize <= LZ4IO_jobSize(&prefs))) prefs.nbWorkers = 0;
    }

    cSize = LZ4IO_compressFrame(&srcSize, frameTail, ress, &input, dstFile, srcFileName, prefs, io_prefs);

    /* source modified, renamed or replaced during compression : kept, even with --rm */
    srcModified = LZ4IO_srcModified(&srcState, srcFile);
    srcReplaced = LZ4IO_srcReplaced(&srcState, srcFileName);
    {   int const inconsistent = srcModified && (input.map != NULL)
                && !LZ4IO_mappedContentUnchanged(&input, srcFile,
                        prefs.frameInfo.contentChecksumFlag == LZ4F_contentChecksumEnabled, frameTail + 4);
        LZ4IO_unmapInput(&input);
        if (inconsistent) {   /* inconsistent output is removed */
            fclose(srcFile);
            if (!LZ4IO_isStdout(dstFileName)) fclose(dstFile);
            if (!LZ4IO_isStdout(dstFileName) && !LZ4IO_isDevNull(dstFileName)) (void)remove(dstFileName);
            END_PROCESS(54, "%s : file modified during compression (memory-mapped) : output removed ; see --no-mmap", srcFileName);
        }
        if (srcModified || srcReplaced)   /* --rm not honored : also displayed in modes lowering notifications (stdout, multiple files) */
            DISPLAYLEVEL((io_prefs->removeSrcFile ? 1 : 2), "\rWarning : %s : file %s during compression : output holds the content read%s \n",
                            srcFileName, srcReplaced ? "renamed or replaced" : "modified",
                            io_prefs->removeSrcFile ? " ; source kept" : "");
    }

    LZ4IO_finishCompressedFile(srcFile, dstFile, srcFileName, dstFileName, srcSize, cSize, srcModified, srcReplaced, io_prefs);
    *inStreamSize = srcSize;
    return 0;
}

int LZ4IO_compressFilename(const char* srcFileName, const char* dstFileName, int compressionLevel, const LZ4IO_prefs_t* prefs)
{
    TIME_t const timeStart = TIME_getTime();
    double const cpuStart = cpuTime_sec();
    cRess_t ress = LZ4IO_createCResources(prefs);
    unsigned long long processed;

    int const result = LZ4IO_compressFilename_extRess(&processed, &ress, srcFileName, dstFileName, compressionLevel, prefs);

    /* Free resources */
    LZ4IO_freeCResources(ress);

    /* Final Status */
    LZ4IO_finalTimeDisplay(timeStart, cpuStart, processed);

    return result;
}

int LZ4IO_compressMultipleFilenames(
                              const char** inFileNamesTable, int ifntSize,
                              const char* suffix,
                              int compressionLevel,
                              const LZ4IO_prefs_t* prefs)
{
    int i;
    int missed_files = 0;
    char* nameBuffer = NULL;
    size_t nameCapacity = 0;
    cRess_t ress = LZ4IO_createCResources(prefs);
    unsigned long long totalProcessed = 0;
    TIME_t timeStart = TIME_getTime();
    double const cpuStart = cpuTime_sec();

    /* loop on each file */
    for (i=0; i<ifntSize; i++) {
        unsigned long long processed;
        const char* const dstFileName = LZ4IO_compressedFileName(&nameBuffer, &nameCapacity, inFileNamesTable[i], suffix);
        if (dstFileName == NULL) {   /* not enough memory */
            LZ4IO_freeCResources(ress);
            return ifntSize;
        }
        missed_files += LZ4IO_compressFilename_extRess(&processed, &ress,
                                inFileNamesTable[i], dstFileName,
                                compressionLevel, prefs);
        totalProcessed += processed;
    }

    /* Close & Free */
    LZ4IO_freeCResources(ress);
    free(nameBuffer);
    LZ4IO_finalTimeDisplay(timeStart, cpuStart, totalProcessed);

    return missed_files;
}


/* ********************************************************************* */
/* ********************** LZ4 file-stream Decompression **************** */
/* ********************************************************************* */

/* It's presumed that @p points to a memory space of size >= 4 */
static unsigned LZ4IO_readLE32 (const void* p)
{
    const unsigned char* const srcPtr = (const unsigned char*)p;
    unsigned value32 = srcPtr[0];
    value32 += (unsigned)srcPtr[1] <<  8;
    value32 += (unsigned)srcPtr[2] << 16;
    value32 += (unsigned)srcPtr[3] << 24;
    return value32;
}


static unsigned
LZ4IO_fwriteSparse(FILE* file,
                   const void* buffer, size_t bufferSize,
                   int sparseFileSupport,
                   unsigned storedSkips,
                   int fromJob)
{
    const size_t sizeT = sizeof(size_t);
    const size_t maskT = sizeT -1 ;
    const size_t* const bufferT = (const size_t*)buffer;   /* Buffer is supposed malloc'ed, hence aligned on size_t */
    const size_t* ptrT = bufferT;
    size_t bufferSizeT = bufferSize / sizeT;
    const size_t* const bufferTEnd = bufferT + bufferSizeT;
    const size_t segmentSizeT = (32 KB) / sizeT;
    int const sparseMode = (sparseFileSupport - (file==stdout)) > 0;

    if (!sparseMode) {  /* normal write */
        size_t const sizeCheck = fwrite(buffer, 1, bufferSize, file);
        if (sizeCheck != bufferSize) END_PROCESS_OR_JOB_ERROR(fromJob, 70, "Write error : cannot write decoded block");
        return 0;
    }

    /* avoid int overflow */
    if (storedSkips > 1 GB) {
        int const seekResult = UTIL_fseek(file, 1 GB, SEEK_CUR);
        if (seekResult != 0) { END_PROCESS_OR_JOB_ERROR(fromJob, 71, "1 GB skip error (sparse file support)"); return 0; }
        storedSkips -= 1 GB;
    }

    while (ptrT < bufferTEnd) {
        size_t seg0SizeT = segmentSizeT;
        size_t nb0T;

        /* count leading zeros */
        if (seg0SizeT > bufferSizeT) seg0SizeT = bufferSizeT;
        bufferSizeT -= seg0SizeT;
        for (nb0T=0; (nb0T < seg0SizeT) && (ptrT[nb0T] == 0); nb0T++) ;
        storedSkips += (unsigned)(nb0T * sizeT);

        if (nb0T != seg0SizeT) {   /* not all 0s */
            errno = 0;
            {   int const seekResult = UTIL_fseek(file, storedSkips, SEEK_CUR);
                if (seekResult) { END_PROCESS_OR_JOB_ERROR(fromJob, 72, "Sparse skip error(%d): %s ; try --no-sparse", (int)errno, strerror(errno)); return 0; }
            }
            storedSkips = 0;
            seg0SizeT -= nb0T;
            ptrT += nb0T;
            {   size_t const sizeCheck = fwrite(ptrT, sizeT, seg0SizeT, file);
                if (sizeCheck != seg0SizeT) { END_PROCESS_OR_JOB_ERROR(fromJob, 73, "Write error : cannot write decoded block"); return 0; }
        }   }
        ptrT += seg0SizeT;
    }

    if (bufferSize & maskT) {  /* size not multiple of sizeT : implies end of block */
        const char* const restStart = (const char*)bufferTEnd;
        const char* restPtr = restStart;
        size_t const restSize =  bufferSize & maskT;
        const char* const restEnd = restStart + restSize;
        for (; (restPtr < restEnd) && (*restPtr == 0); restPtr++) ;
        storedSkips += (unsigned) (restPtr - restStart);
        if (restPtr != restEnd) {
            int const seekResult = UTIL_fseek(file, storedSkips, SEEK_CUR);
            if (seekResult) { END_PROCESS_OR_JOB_ERROR(fromJob, 74, "Sparse skip error ; try --no-sparse"); return 0; }
            storedSkips = 0;
            {   size_t const sizeCheck = fwrite(restPtr, 1, (size_t)(restEnd - restPtr), file);
                if (sizeCheck != (size_t)(restEnd - restPtr)) { END_PROCESS_OR_JOB_ERROR(fromJob, 75, "Write error : cannot write decoded end of block"); return 0; }
        }   }
    }

    return storedSkips;
}

static void LZ4IO_fwriteSparseEnd(FILE* file, unsigned storedSkips)
{
    if (storedSkips>0) {   /* implies sparseFileSupport>0 */
        const char lastZeroByte[1] = { 0 };
        if (UTIL_fseek(file, storedSkips-1, SEEK_CUR) != 0)
            END_PROCESS(69, "Final skip error (sparse file)\n");
        if (fwrite(lastZeroByte, 1, 1, file) != 1)
            END_PROCESS(69, "Write error : cannot write last zero\n");
    }
}


static unsigned g_magicRead = 0;   /* out-parameter of LZ4IO_decodeLegacyStream() */

#if LZ4IO_MULTITHREAD

typedef struct {
    void* buffer;
    size_t size;
    FILE* f;
    int sparseEnable;
    unsigned* storedSkips;
    unsigned long long* totalSize;
} ChunkToWrite;

static void LZ4IO_writeDecodedChunk(void* arg)
{
    ChunkToWrite* const ctw = (ChunkToWrite*)arg;
    assert(ctw != NULL);

    if (!TPool_jobError()) {   /* otherwise, nothing more to write */
        /* note: works because only 1 thread */
        *ctw->storedSkips = LZ4IO_fwriteSparse(ctw->f, ctw->buffer, ctw->size, ctw->sparseEnable, *ctw->storedSkips, 1);
        *ctw->totalSize += (unsigned long long)ctw->size;
        DISPLAYUPDATE(2, "\rDecompressed : %u MiB  ", (unsigned)(ctw->totalSize[0] >>20));
    }

    /* clean up */
    free(ctw);
}

typedef struct {
    void* inBuffer;
    size_t inSize;
    void* outBuffer;
    unsigned long long* totalSize;
    TPool* wPool;
    FILE* foutput;
    int sparseEnable;
    unsigned* storedSkips;
} LegacyBlockInput;

static void LZ4IO_decompressBlockLegacy(void* arg)
{
    int decodedSize;
    LegacyBlockInput* const lbi = (LegacyBlockInput*)arg;

    if (TPool_jobError()) { free(lbi); return; }   /* stop decoding */
    decodedSize = LZ4_decompress_safe((const char*)lbi->inBuffer, (char*)lbi->outBuffer, (int)lbi->inSize, LEGACY_BLOCKSIZE);
    if (decodedSize < 0) {
        JOB_ERROR(64, "Decoding Failed ! Corrupted input detected !");
        free(lbi);
        return;
    }

    /* push to write thread */
    {   ChunkToWrite* const ctw = (ChunkToWrite*)malloc(sizeof(*ctw));
        if (ctw==NULL) {
            JOB_ERROR(33, "Allocation error : can't describe new write job");
            free(lbi);
            return;
        }
        ctw->buffer = lbi->outBuffer;
        ctw->size = (size_t)decodedSize;
        ctw->f = lbi->foutput;
        ctw->sparseEnable = lbi->sparseEnable;
        ctw->storedSkips = lbi->storedSkips;
        ctw->totalSize = lbi->totalSize;
        TPool_submitJob(lbi->wPool, LZ4IO_writeDecodedChunk, ctw);
    }

    /* clean up */
    free(lbi);
}

static unsigned long long
LZ4IO_decodeLegacyStream(FILE* finput, FILE* foutput, const LZ4IO_prefs_t* prefs)
{
    unsigned long long streamSize = 0;
    unsigned storedSkips = 0;

    TPool* const tPool = TPool_create(1, 1);
    TPool* const wPool = TPool_create(1, 1);
#define NB_BUFFSETS 4 /* 1 being read, 1 being processed, 1 being written, 1 being queued */
    void* inBuffs[NB_BUFFSETS];
    void* outBuffs[NB_BUFFSETS];
    int bSetNb;

    if (tPool == NULL || wPool == NULL)
        END_PROCESS(21, "threadpool creation error ");

    /* allocate buffers up front */
    for (bSetNb=0; bSetNb<NB_BUFFSETS; bSetNb++) {
        inBuffs[bSetNb] = malloc((size_t)LZ4_compressBound(LEGACY_BLOCKSIZE));
        outBuffs[bSetNb] = malloc(LEGACY_BLOCKSIZE);
        if (!inBuffs[bSetNb] || !outBuffs[bSetNb])
            END_PROCESS(31, "Allocation error : can't allocate buffer for legacy decoding");
    }

    /* Main Loop */
    for (bSetNb = 0;;bSetNb = (bSetNb+1) % NB_BUFFSETS) {
        char header[LZ4IO_LEGACY_BLOCK_HEADER_SIZE];
        unsigned int blockSize;

        if (TPool_jobError()) break;

        /* Block Size */
        {   size_t const sizeCheck = fread(header, 1, LZ4IO_LEGACY_BLOCK_HEADER_SIZE, finput);
            if (sizeCheck == 0) break;                   /* Nothing to read : file read is completed */
            if (sizeCheck != LZ4IO_LEGACY_BLOCK_HEADER_SIZE) {
                LZ4IO_waitJobs(tPool, wPool);
                END_PROCESS(61, "Error: cannot read block size in Legacy format");
        }   }
        blockSize = LZ4IO_readLE32(header);       /* Convert to Little Endian */
        if (blockSize == 0) break;                /* end marker (Linux kernel) */
        if (blockSize > LZ4_COMPRESSBOUND(LEGACY_BLOCKSIZE)) {
            /* Cannot read next block : maybe new stream ? */
            g_magicRead = blockSize;
            break;
        }

        /* Read Block */
        {   size_t const sizeCheck = fread(inBuffs[bSetNb], 1, blockSize, finput);
            if (sizeCheck != blockSize) {
                LZ4IO_waitJobs(tPool, wPool);
                END_PROCESS(63, "Read error : cannot access compressed block !");
            }
            /* push to decoding thread */
            {   LegacyBlockInput* const lbi = (LegacyBlockInput*)malloc(sizeof(*lbi));
                if (lbi==NULL) {
                    LZ4IO_waitJobs(tPool, wPool);
                    END_PROCESS(64, "Allocation error : not enough memory to allocate job descriptor");
                }
                lbi->inBuffer = inBuffs[bSetNb];
                lbi->inSize = blockSize;
                lbi->outBuffer = outBuffs[bSetNb];
                lbi->wPool = wPool;
                lbi->totalSize = &streamSize;
                lbi->foutput = foutput;
                lbi->sparseEnable = prefs->sparseFileSupport;
                lbi->storedSkips = &storedSkips;
                TPool_submitJob(tPool, LZ4IO_decompressBlockLegacy, lbi);
            }
        }
    }
    LZ4IO_waitJobs(tPool, wPool);
    if (ferror(finput)) END_PROCESS(65, "Read error : ferror");

    /* flush last zeroes */
    LZ4IO_fwriteSparseEnd(foutput, storedSkips);

    /* Free */
    TPool_free(wPool);
    TPool_free(tPool);
    for (bSetNb=0; bSetNb<NB_BUFFSETS; bSetNb++) {
        free(inBuffs[bSetNb]);
        free(outBuffs[bSetNb]);
    }

    return streamSize;
}

#else

static unsigned long long
LZ4IO_decodeLegacyStream(FILE* finput, FILE* foutput, const LZ4IO_prefs_t* prefs)
{
    unsigned long long streamSize = 0;
    unsigned storedSkips = 0;

    /* Allocate Memory */
    char* const in_buff  = (char*)malloc((size_t)LZ4_compressBound(LEGACY_BLOCKSIZE));
    char* const out_buff = (char*)malloc(LEGACY_BLOCKSIZE);
    if (!in_buff || !out_buff) END_PROCESS(61, "Allocation error : not enough memory");

    /* Main Loop */
    while (1) {
        unsigned int blockSize;

        /* Block Size */
        {   size_t const sizeCheck = fread(in_buff, 1, LZ4IO_LEGACY_BLOCK_HEADER_SIZE, finput);
            if (sizeCheck == 0) break;                   /* Nothing to read : file read is completed */
            if (sizeCheck != LZ4IO_LEGACY_BLOCK_HEADER_SIZE)
                END_PROCESS(62, "Error: cannot read block size in Legacy format");
        }
        blockSize = LZ4IO_readLE32(in_buff);       /* Convert to Little Endian */
        if (blockSize == 0) break;                 /* end marker (Linux kernel) */
        if (blockSize > LZ4_COMPRESSBOUND(LEGACY_BLOCKSIZE)) {
            /* Cannot read next block : maybe new stream ? */
            g_magicRead = blockSize;
            break;
        }

        /* Read Block */
        { size_t const sizeCheck = fread(in_buff, 1, blockSize, finput);
          if (sizeCheck != blockSize) END_PROCESS(63, "Read error : cannot access compressed block !"); }

        /* Decode Block */
        {   int const decodeSize = LZ4_decompress_safe(in_buff, out_buff, (int)blockSize, LEGACY_BLOCKSIZE);
            if (decodeSize < 0) END_PROCESS(64, "Decoding Failed ! Corrupted input detected !");
            streamSize += (unsigned long long)decodeSize;
            /* Write Block */
            storedSkips = LZ4IO_fwriteSparse(foutput, out_buff, (size_t)decodeSize, prefs->sparseFileSupport, storedSkips, 0); /* success or die */
    }   }
    if (ferror(finput)) END_PROCESS(65, "Read error : ferror");

    LZ4IO_fwriteSparseEnd(foutput, storedSkips);

    /* Free */
    free(in_buff);
    free(out_buff);

    return streamSize;
}
#endif

/* mozlz4 : format of Firefox's .jsonlz4 and .mozlz4 files (decoding only).
 * MOZLZ4_MAGIC, followed by the decompressed size (4 bytes, little endian),
 * then a single LZ4 block, extending to the end of input.
 * Like Firefox, the decompressed size is only used as decoding capacity. */
static unsigned long long
LZ4IO_decodeMozLZ4(FILE* finput, FILE* foutput, const LZ4IO_prefs_t* prefs)
{
    unsigned char sizeStore[4];
    unsigned dstCapacity;
    size_t srcCapacity, srcSize;
    char* src;
    char* dst;
    int decodedSize;

    if (fread(sizeStore, 1, sizeof(sizeStore), finput) != sizeof(sizeStore))
        END_PROCESS(62, "Error: cannot read decompressed size in mozlz4 format");
    dstCapacity = LZ4IO_readLE32(sizeStore);
    if (dstCapacity > (unsigned)LZ4_MAX_INPUT_SIZE)   /* can't be compressed into a single block */
        END_PROCESS(62, "Error: mozlz4 decompressed size too large (%u)", dstCapacity);
    srcCapacity = (size_t)LZ4_COMPRESSBOUND(dstCapacity);   /* no valid block is larger */

    src = (char*)malloc(srcCapacity);
    dst = (char*)malloc(MAX(dstCapacity, 1));   /* malloc(0) may return NULL */
    if (!src || !dst) END_PROCESS(61, "Allocation error : not enough memory");

    srcSize = fread(src, 1, srcCapacity, finput);
    if (ferror(finput)) END_PROCESS(65, "Read error : ferror");

    if (srcSize == 0 && dstCapacity == 0)
        decodedSize = 0;   /* empty content : Firefox writes no block */
    else
        decodedSize = LZ4_decompress_safe(src, dst, (int)srcSize, (int)dstCapacity);
    if (decodedSize < 0) END_PROCESS(64, "Decoding Failed ! Corrupted input detected !");
    {   unsigned const storedSkips = LZ4IO_fwriteSparse(foutput, dst, (size_t)decodedSize, prefs->sparseFileSupport, 0, 0); /* success or die */
        LZ4IO_fwriteSparseEnd(foutput, storedSkips);
    }

    free(src);
    free(dst);
    return (unsigned long long)decodedSize;
}


typedef struct {
    void*  srcBuffer;
    size_t srcBufferSize;
    void*  dstBuffer;
    size_t dstBufferSize;
    FILE*  dstFile;
    LZ4F_decompressionContext_t dCtx;
    void*  dictBuffer;
    size_t dictBufferSize;
} dRess_t;

static void LZ4IO_loadDDict(dRess_t* ress, const LZ4IO_prefs_t* const prefs)
{
    if (!prefs->useDictionary) {
        ress->dictBuffer = NULL;
        ress->dictBufferSize = 0;
        return;
    }

    ress->dictBuffer = LZ4IO_createDict(&ress->dictBufferSize, prefs->dictionaryFilename);
    if (!ress->dictBuffer) END_PROCESS(25, "Dictionary error : could not create dictionary");
}

static const size_t LZ4IO_dBufferSize = 64 KB;
static dRess_t LZ4IO_createDResources(const LZ4IO_prefs_t* const prefs)
{
    dRess_t ress;

    /* init */
    LZ4F_errorCode_t const errorCode = LZ4F_createDecompressionContext(&ress.dCtx, LZ4F_VERSION);
    if (LZ4F_isError(errorCode)) END_PROCESS(60, "Can't create LZ4F context : %s", LZ4F_getErrorName(errorCode));

    /* Allocate Memory */
    ress.srcBufferSize = LZ4IO_dBufferSize;
    ress.srcBuffer = malloc(ress.srcBufferSize);
    ress.dstBufferSize = LZ4IO_dBufferSize;
    ress.dstBuffer = malloc(ress.dstBufferSize);
    if (!ress.srcBuffer || !ress.dstBuffer) END_PROCESS(61, "Allocation error : not enough memory");

    LZ4IO_loadDDict(&ress, prefs);

    ress.dstFile = NULL;
    return ress;
}

static void LZ4IO_freeDResources(dRess_t ress)
{
    LZ4F_errorCode_t errorCode = LZ4F_freeDecompressionContext(ress.dCtx);
    if (LZ4F_isError(errorCode)) END_PROCESS(69, "Error : can't free LZ4F context resource : %s", LZ4F_getErrorName(errorCode));
    free(ress.srcBuffer);
    free(ress.dstBuffer);
    free(ress.dictBuffer);
}


#if LZ4IO_MULTITHREAD

#define INBUFF_SIZE (4 MB)
#define OUTBUFF_SIZE (1 * INBUFF_SIZE)
#define OUTBUFF_QUEUE 1
#define PBUFFERS_NB (1 /* being decompressed */ + OUTBUFF_QUEUE + 1 /* being written to io */)

typedef struct {
    void* ptr;
    size_t capacity;
    size_t size;
} Buffer;

/* BufferPool:
 * Based on ayncio property :
 * all buffers are allocated and released in order,
 * maximum nb of buffers limited by queues */

typedef struct {
    Buffer buffers[PBUFFERS_NB];
    unsigned long long availNext;   /* monotonic : used modulo PBUFFERS_NB, must not signed-overflow */
    unsigned long long usedIdx;
} BufferPool;

static void LZ4IO_freeBufferPool(BufferPool* bp)
{
    int i;
    if (bp==NULL) return;
    for (i=0; i<PBUFFERS_NB; i++)
        free(bp->buffers[i].ptr);
    free(bp);
}

static BufferPool* LZ4IO_createBufferPool(size_t bufSize)
{
    BufferPool* const bp = (BufferPool*)calloc(1, sizeof(*bp));
    int i;
    if (bp==NULL) return NULL;
    for (i=0; i<PBUFFERS_NB; i++) {
        bp->buffers[i].ptr = malloc(bufSize);
        if (bp->buffers[i].ptr == NULL) {
            LZ4IO_freeBufferPool(bp);
            return NULL;
        }
         bp->buffers[i].capacity = bufSize;
         bp->buffers[i].size = 0;
    }
    bp->availNext = 0;
    bp->usedIdx = 0;
    return bp;
}

/* Note: Thread Sanitizer can be detected with below macro
 * but it's not guaranteed (doesn't seem to work with clang) */
#ifdef __SANITIZE_THREAD__
# undef LZ4IO_NO_TSAN_ONLY
#endif

static Buffer BufPool_getBuffer(BufferPool* bp)
{
    assert(bp != NULL);
#ifdef LZ4IO_NO_TSAN_ONLY
    /* The following assert() are susceptible to race conditions */
    assert(bp->availNext >= bp->usedIdx);
    assert(bp->availNext < bp->usedIdx + PBUFFERS_NB);
#endif
    {   unsigned long long const id = bp->availNext++ % PBUFFERS_NB;
        assert(bp->buffers[id].size == 0);
        return bp->buffers[id];
}   }

void BufPool_releaseBuffer(BufferPool* bp, Buffer buf)
{
    assert(bp != NULL);
#ifdef LZ4IO_NO_TSAN_ONLY
    /* The following assert() is susceptible to race conditions */
    assert(bp->usedIdx < bp->availNext);
#endif
    {   unsigned long long const id = bp->usedIdx++ % PBUFFERS_NB;
        assert(bp->buffers[id].ptr == buf.ptr);
        bp->buffers[id].size = 0;
}   }

typedef struct {
    Buffer bufOut;
    FILE* fOut;
    BufferPool* bp;
    int sparseEnable;
    unsigned* storedSkips;
    unsigned long long* totalSize;
} LZ4FChunkToWrite;

static void LZ4IO_writeDecodedLZ4FChunk(void* arg)
{
    LZ4FChunkToWrite* const ctw = (LZ4FChunkToWrite*)arg;
    assert(ctw != NULL);

    if (!TPool_jobError()) {   /* otherwise, nothing more to write */
        /* note: works because only 1 thread */
        *ctw->storedSkips = LZ4IO_fwriteSparse(ctw->fOut, ctw->bufOut.ptr, ctw->bufOut.size, ctw->sparseEnable, *ctw->storedSkips, 1);
        *ctw->totalSize += (unsigned long long)ctw->bufOut.size; /* note: works because only 1 thread */
        DISPLAYUPDATE(2, "\rDecompressed : %u MiB  ", (unsigned)(ctw->totalSize[0] >> 20));
    }

    /* clean up */
    BufPool_releaseBuffer(ctw->bp, ctw->bufOut);
    free(ctw);
}

typedef struct {
    LZ4F_dctx* dctx;
    const void* inBuffer;
    size_t inSize;
    const void* dictBuffer;
    size_t dictBufferSize;
    BufferPool* bp;
    unsigned long long* totalSize;
    LZ4F_errorCode_t* lastStatus;
    TPool* wPool;
    FILE* foutput;
    int sparseEnable;
    unsigned* storedSkips;
} LZ4FChunk;

static void LZ4IO_decompressLZ4FChunk(void* arg)
{
    LZ4FChunk* const lz4fc = (LZ4FChunk*)arg;
    const char* inPtr = (const char*)lz4fc->inBuffer;
    size_t pos = 0;

    while ((pos < lz4fc->inSize) && !TPool_jobError()) {  /* still to read */
        size_t remainingInSize = lz4fc->inSize - pos;
        Buffer b = BufPool_getBuffer(lz4fc->bp);
        if (b.capacity != OUTBUFF_SIZE) {
            JOB_ERROR(33, "Could not allocate output buffer!");
            break;
        }
        assert(b.size == 0);
        b.size = b.capacity;
        {   size_t nextToLoad = LZ4F_decompress_usingDict(lz4fc->dctx,
                                b.ptr, &b.size,
                                inPtr + pos, &remainingInSize,
                                lz4fc->dictBuffer, lz4fc->dictBufferSize,
                                NULL);
            if (LZ4F_isError(nextToLoad)) {
                JOB_ERROR(34, "Decompression error : %s", LZ4F_getErrorName(nextToLoad));
                break;
            }
            *lz4fc->lastStatus = nextToLoad;
        }
        assert(remainingInSize <= lz4fc->inSize - pos);
        pos += remainingInSize;
        assert(b.size <= b.capacity);

        /* push to write thread */
        {   LZ4FChunkToWrite* const ctw = (LZ4FChunkToWrite*)malloc(sizeof(*ctw));
            if (ctw==NULL) {
                JOB_ERROR(35, "Allocation error : can't describe new write job");
                break;
            }
            ctw->bufOut = b;
            ctw->fOut = lz4fc->foutput;
            ctw->bp = lz4fc->bp;
            ctw->sparseEnable = lz4fc->sparseEnable;
            ctw->storedSkips = lz4fc->storedSkips;
            ctw->totalSize = lz4fc->totalSize;
            TPool_submitJob(lz4fc->wPool, LZ4IO_writeDecodedLZ4FChunk, ctw);
        }
    }

    /* clean up */
    free(lz4fc);
}

static unsigned long long
LZ4IO_decompressLZ4F(dRess_t ress,
                     FILE* const srcFile, FILE* const dstFile,
                     const LZ4IO_prefs_t* const prefs)
{
    unsigned long long filesize = 0;
    LZ4F_errorCode_t nextToLoad;
    LZ4F_errorCode_t lastStatus = 1;
    unsigned storedSkips = 0;
    LZ4F_decompressOptions_t const dOpt_skipCrc = { 0, 1, 0, 0 };
    const LZ4F_decompressOptions_t* const dOptPtr =
        ((prefs->blockChecksum==0) && (prefs->streamChecksum==0)) ?
        &dOpt_skipCrc : NULL;
    TPool* const tPool = TPool_create(1, 1);
    TPool* const wPool = TPool_create(1, 1);
    BufferPool* const bp = LZ4IO_createBufferPool(OUTBUFF_SIZE);
#define NB_BUFFSETS 4 /* 1 being read, 1 being processed, 1 being written, 1 being queued */
    void* inBuffs[NB_BUFFSETS];
    int bSetNb;

    /* checks */
    if (tPool == NULL || wPool == NULL || bp==NULL)
        END_PROCESS(22, "threadpool creation error ");

    /* allocate buffers up front */
    for (bSetNb=0; bSetNb<NB_BUFFSETS; bSetNb++) {
        inBuffs[bSetNb] = malloc((size_t)INBUFF_SIZE);
        if (!inBuffs[bSetNb])
            END_PROCESS(23, "Allocation error : can't allocate buffer for legacy decoding");
    }

    /* Init feed with magic number (already consumed from FILE* sFile) */
    {   size_t inSize = MAGICNUMBER_SIZE;
        size_t outSize= 0;
        LZ4IO_writeLE32(ress.srcBuffer, LZ4IO_MAGICNUMBER);
        nextToLoad = LZ4F_decompress_usingDict(ress.dCtx,
                            ress.dstBuffer, &outSize,
                            ress.srcBuffer, &inSize,
                            ress.dictBuffer, ress.dictBufferSize,
                            dOptPtr);  /* set it once, it's enough */
        if (LZ4F_isError(nextToLoad))
            END_PROCESS(23, "Header error : %s", LZ4F_getErrorName(nextToLoad));
    }

    /* Main Loop */
    assert(nextToLoad);
    for (bSetNb = 0; ; bSetNb = (bSetNb+1) % NB_BUFFSETS) {
        size_t readSize;

        if (TPool_jobError()) break;

        /* Read input */
        readSize = fread(inBuffs[bSetNb], 1, INBUFF_SIZE, srcFile);
        if (ferror(srcFile)) {
            LZ4IO_waitJobs(tPool, wPool);
            END_PROCESS(26, "Read error");
        }

        /* push to decoding thread */
        {   LZ4FChunk* const lbi = (LZ4FChunk*)malloc(sizeof(*lbi));
            if (lbi==NULL) {
                LZ4IO_waitJobs(tPool, wPool);
                END_PROCESS(25, "Allocation error : not enough memory to allocate job descriptor");
            }
            lbi->dctx = ress.dCtx;
            lbi->inBuffer = inBuffs[bSetNb];
            lbi->inSize = readSize;
            lbi->dictBuffer = ress.dictBuffer;
            lbi->dictBufferSize = ress.dictBufferSize;
            lbi->bp = bp;
            lbi->wPool = wPool;
            lbi->totalSize = &filesize;
            lbi->lastStatus = &lastStatus;
            lbi->foutput = dstFile;
            lbi->sparseEnable = prefs->sparseFileSupport;
            lbi->storedSkips = &storedSkips;
            TPool_submitJob(tPool, LZ4IO_decompressLZ4FChunk, lbi);
        }
        if (readSize < INBUFF_SIZE) break;   /* likely reached end of stream */
    }
    LZ4IO_waitJobs(tPool, wPool);
    assert(feof(srcFile));

    /* flush */
    if (lastStatus != 0) {
        END_PROCESS(26, "LZ4F frame decoding could not complete: invalid frame");
    }
    if (!prefs->testMode) LZ4IO_fwriteSparseEnd(dstFile, storedSkips);

    /* Clean */
    for (bSetNb=0; bSetNb<NB_BUFFSETS; bSetNb++) {
        free(inBuffs[bSetNb]);
    }
    LZ4IO_freeBufferPool(bp);
    TPool_free(wPool);
    TPool_free(tPool);

    return filesize;
}

#else

static unsigned long long
LZ4IO_decompressLZ4F(dRess_t ress,
                     FILE* const srcFile, FILE* const dstFile,
                     const LZ4IO_prefs_t* const prefs)
{
    unsigned long long filesize = 0;
    LZ4F_errorCode_t nextToLoad;
    unsigned storedSkips = 0;
    LZ4F_decompressOptions_t const dOpt_skipCrc = { 0, 1, 0, 0 };
    const LZ4F_decompressOptions_t* const dOptPtr =
        ((prefs->blockChecksum==0) && (prefs->streamChecksum==0)) ?
        &dOpt_skipCrc : NULL;

    /* Init feed with magic number (already consumed from FILE* sFile) */
    {   size_t inSize = MAGICNUMBER_SIZE;
        size_t outSize= 0;
        LZ4IO_writeLE32(ress.srcBuffer, LZ4IO_MAGICNUMBER);
        nextToLoad = LZ4F_decompress_usingDict(ress.dCtx,
                            ress.dstBuffer, &outSize,
                            ress.srcBuffer, &inSize,
                            ress.dictBuffer, ress.dictBufferSize,
                            dOptPtr);  /* set it once, it's enough */
        if (LZ4F_isError(nextToLoad))
            END_PROCESS(62, "Header error : %s", LZ4F_getErrorName(nextToLoad));
    }

    /* Main Loop */
    for (;nextToLoad;) {
        size_t readSize;
        size_t pos = 0;
        size_t decodedBytes = ress.dstBufferSize;

        /* Read input */
        if (nextToLoad > ress.srcBufferSize) nextToLoad = ress.srcBufferSize;
        readSize = fread(ress.srcBuffer, 1, nextToLoad, srcFile);
        if (!readSize) break;   /* reached end of file or stream */

        while ((pos < readSize) || (decodedBytes == ress.dstBufferSize)) {  /* still to read, or still to flush */
            /* Decode Input (at least partially) */
            size_t remaining = readSize - pos;
            decodedBytes = ress.dstBufferSize;
            nextToLoad = LZ4F_decompress_usingDict(ress.dCtx,
                                    ress.dstBuffer, &decodedBytes,
                                    (char*)(ress.srcBuffer)+pos, &remaining,
                                    ress.dictBuffer, ress.dictBufferSize,
                                    NULL);
            if (LZ4F_isError(nextToLoad))
                END_PROCESS(66, "Decompression error : %s", LZ4F_getErrorName(nextToLoad));
            pos += remaining;

            /* Write Block */
            if (decodedBytes) {
                if (!prefs->testMode)
                    storedSkips = LZ4IO_fwriteSparse(dstFile, ress.dstBuffer, decodedBytes, prefs->sparseFileSupport, storedSkips, 0);
                filesize += decodedBytes;
                DISPLAYUPDATE(2, "\rDecompressed : %u MiB  ", (unsigned)(filesize>>20));
            }

            if (!nextToLoad) break;
        }
    }
    /* can be out because readSize == 0, which could be an fread() error */
    if (ferror(srcFile)) END_PROCESS(67, "Read error");

    if (!prefs->testMode) LZ4IO_fwriteSparseEnd(dstFile, storedSkips);
    if (nextToLoad!=0)
        END_PROCESS(68, "Unfinished stream (nextToLoad=%u)", (unsigned)nextToLoad);

    return filesize;
}

#endif /* LZ4IO_MULTITHREAD */

/* LZ4IO_passThrough:
 * just output the same content as input, no decoding.
 * This is a capability of zcat, and by extension lz4cat
 * MNstore : contain the first MNsize bytes already read from finput
 */
#define PTSIZE  (64 KB)
#define PTSIZET (PTSIZE / sizeof(size_t))
static unsigned long long
LZ4IO_passThrough(FILE* finput, FILE* foutput,
                  const unsigned char* MNstore, size_t MNsize,
                  int sparseFileSupport)
{
	size_t buffer[PTSIZET];
    size_t readBytes = 1;
    unsigned long long total = MNsize;
    unsigned storedSkips = 0;

    if (fwrite(MNstore, 1, MNsize, foutput) != MNsize) {
        END_PROCESS(50, "Pass-through write error");
    }
    while (readBytes) {
        readBytes = fread(buffer, 1, sizeof(buffer), finput);
        total += readBytes;
        storedSkips = LZ4IO_fwriteSparse(foutput, buffer, readBytes, sparseFileSupport, storedSkips, 0);
    }
    if (ferror(finput)) END_PROCESS(51, "Read Error");

    LZ4IO_fwriteSparseEnd(foutput, storedSkips);
    return total;
}

/* when fseek() doesn't work (pipe scenario),
 * read and forget from input.
**/
#define SKIP_BUFF_SIZE (16 KB)
static int skipStream(FILE* f, unsigned offset)
{
    char buf[SKIP_BUFF_SIZE];
    while (offset > 0) {
        size_t const tr = MIN(offset, sizeof(buf));
        size_t const r = fread(buf, 1, tr, f);
        if (r != tr) return 1; /* error reading f */
        offset -= (unsigned)tr;
    }
    assert(offset == 0);
    return 0;
}

/** Safely handle cases when (unsigned)offset > LONG_MAX.
 *  fseek() succeeds beyond end of file, so the last byte is read instead,
 *  to detect when fewer than @offset bytes remain (truncated input). */
static int fseek_u32(FILE *fp, unsigned offset, int where)
{
    const unsigned stepMax = 1U << 30;
    int errorNb = 0;

    if (where != SEEK_CUR) return -1;  /* Only allows SEEK_CUR */
    if (offset == 0) return 0;
    offset--;   /* last byte is read below */
    while (offset > 0) {
        unsigned s = offset;
        if (s > stepMax) s = stepMax;
        errorNb = UTIL_fseek(fp, (long)s, SEEK_CUR);
        if (errorNb==0) { offset -= s; continue; }
        errorNb = skipStream(fp, offset);
        offset = 0;
    }
    if (errorNb) return errorNb;
    return (fgetc(fp) == EOF);
}


#define ENDOFSTREAM ((unsigned long long)-1)
#define DECODING_ERROR ((unsigned long long)-2)
static unsigned long long
selectDecoder(dRess_t ress,
              FILE* finput, FILE* foutput,
              const LZ4IO_prefs_t* const prefs)
{
    unsigned char MNstore[MOZLZ4_MAGIC_SIZE];   /* large enough for any magic number */
    size_t MNsize = MAGICNUMBER_SIZE;           /* nb of bytes read into MNstore */
    unsigned magicNumber;
    static unsigned nbFrames = 0;

    /* init */
    nbFrames++;

    /* Check Archive Header */
    if (g_magicRead) {  /* magic number already read from finput (see legacy frame)*/
        magicNumber = g_magicRead;
        g_magicRead = 0;
    } else {
        size_t const nbReadBytes = fread(MNstore, 1, MAGICNUMBER_SIZE, finput);
        if (nbReadBytes==0) { nbFrames = 0; return ENDOFSTREAM; }   /* EOF */
        if (nbReadBytes != MAGICNUMBER_SIZE)
          END_PROCESS(40, "Unrecognized header : Magic Number unreadable");
        magicNumber = LZ4IO_readLE32(MNstore);   /* Little Endian format */
    }
    if (LZ4IO_isSkippableMagicNumber(magicNumber))
        magicNumber = LZ4IO_SKIPPABLE0;   /* fold skippable magic numbers */
    if (magicNumber == LZ4IO_readLE32(MOZLZ4_MAGIC)) {
        /* first half of mozlz4 magic : check the second half */
        size_t const secondHalfSize = MOZLZ4_MAGIC_SIZE - MAGICNUMBER_SIZE;
        MNsize += fread(MNstore + MAGICNUMBER_SIZE, 1, secondHalfSize, finput);
        if ( MNsize == MOZLZ4_MAGIC_SIZE
          && !memcmp(MNstore + MAGICNUMBER_SIZE, &MOZLZ4_MAGIC[MAGICNUMBER_SIZE], secondHalfSize) ) {
            DISPLAYLEVEL(4, "Detected : mozlz4 format \n");
            return LZ4IO_decodeMozLZ4(finput, foutput, prefs);
        }
        /* not mozlz4 : undecodable, see default case */
    }

    switch(magicNumber)
    {
    case LZ4IO_MAGICNUMBER:
        return LZ4IO_decompressLZ4F(ress, finput, foutput, prefs);
    case LEGACY_MAGICNUMBER:
        DISPLAYLEVEL(4, "Detected : Legacy format \n");
        return LZ4IO_decodeLegacyStream(finput, foutput, prefs);
    case LZ4IO_SKIPPABLE0:
        DISPLAYLEVEL(4, "Skipping detected skippable area \n");
        {   size_t const nbReadBytes = fread(MNstore, 1, 4, finput);
            if (nbReadBytes != 4)
                END_PROCESS(42, "Stream error : skippable size unreadable");
        }
        {   unsigned const size = LZ4IO_readLE32(MNstore);
            int const errorNb = fseek_u32(finput, size, SEEK_CUR);
            if (errorNb != 0)
                END_PROCESS(43, "Stream error : cannot skip skippable area");
        }
        return 0;
    default:
        if (nbFrames == 1) {  /* just started */
            /* Wrong magic number at the beginning of 1st stream */
            if (!prefs->testMode && prefs->overwrite && prefs->passThrough) {
                nbFrames = 0;
                return LZ4IO_passThrough(finput, foutput, MNstore, MNsize, prefs->sparseFileSupport);
            }
            END_PROCESS(44,"Unrecognized header : file cannot be decoded");
        }
        {   long int const position = ftell(finput);  /* only works for files < 2 GB */
            DISPLAYLEVEL(2, "Stream followed by undecodable data ");
            if (position != -1L)
                DISPLAYLEVEL(2, "at position %i ", (int)position);
            DISPLAYLEVEL(2, "\n");
        }
        return DECODING_ERROR;
    }
}


static int
LZ4IO_decompressSrcFile(unsigned long long* outGenSize,
                        dRess_t ress,
                        const char* input_filename, const char* output_filename,
                        const LZ4IO_prefs_t* const prefs)
{
    FILE* const foutput = ress.dstFile;
    unsigned long long filesize = 0;
    int result = 0;

    /* Init */
    FILE* const finput = LZ4IO_openSrcFile(input_filename);
    if (finput==NULL) return 1;
    assert(foutput != NULL);

    /* Loop over multiple streams */
    for ( ; ; ) {  /* endless loop, see break condition */
        unsigned long long const decodedSize =
                        selectDecoder(ress, finput, foutput, prefs);
        if (decodedSize == ENDOFSTREAM) break;
        if (decodedSize == DECODING_ERROR) { result=1; break; }
        filesize += decodedSize;
    }

    /* Close input */
    fclose(finput);
    if (!result && LZ4IO_mustRemoveSrcFile(prefs, input_filename)) {  /* --rm */
        if (remove(input_filename))
            END_PROCESS(45, "Remove error : %s: %s", input_filename, strerror(errno));
    }

    /* Final Status */
    DISPLAYLEVEL(2, "\r%79s\r", "");
    DISPLAYLEVEL(2, "%-30.30s : decoded %llu bytes \n", input_filename, filesize);
    *outGenSize = filesize;
    (void)output_filename;

    return result;
}


static int
LZ4IO_decompressDstFile(unsigned long long* outGenSize,
                        dRess_t ress,
                        const char* input_filename,
                        const char* output_filename,
                        const LZ4IO_prefs_t* const prefs)
{
    int result;
    stat_t statbuf;
    int stat_result = 0;
    FILE* const foutput = LZ4IO_openDstFile(output_filename, prefs, input_filename);
    if (foutput==NULL) return 1;   /* failure */

    if ( !LZ4IO_isStdin(input_filename)
      && UTIL_getFileStat(input_filename, &statbuf))
        stat_result = 1;

    ress.dstFile = foutput;
    result = LZ4IO_decompressSrcFile(outGenSize, ress, input_filename, output_filename, prefs);

    fclose(foutput);

    /* Copy owner, file permissions and modification time */
    if ( stat_result != 0
      && !LZ4IO_isStdout(output_filename)
      && !LZ4IO_isDevNull(output_filename)) {
        UTIL_setFileStat(output_filename, &statbuf);
        /* should return value be read ? or is silent fail good enough ? */
    }

    return result;
}


/* Note : LZ4IO_decompressFilename()
 * can provide total decompression time for the specified fileName.
 * This information is not available with LZ4IO_decompressMultipleFilenames().
 */
int LZ4IO_decompressFilename(const char* input_filename, const char* output_filename, const LZ4IO_prefs_t* prefs)
{
    dRess_t const ress = LZ4IO_createDResources(prefs);
    TIME_t const timeStart = TIME_getTime();
    double const cpuStart = cpuTime_sec();
    unsigned long long processed = 0;

    int const errStat = LZ4IO_decompressDstFile(&processed, ress, input_filename, output_filename, prefs);
    if (!errStat)
        LZ4IO_finalTimeDisplay(timeStart, cpuStart, processed);
    LZ4IO_freeDResources(ress);
    return errStat;
}


int LZ4IO_decompressMultipleFilenames(
                            const char** inFileNamesTable, int ifntSize,
                            const char* suffix,
                            const LZ4IO_prefs_t* prefs)
{
    int i;
    unsigned long long totalProcessed = 0;
    int skippedFiles = 0;
    int missingFiles = 0;
    char* outFileName = NULL;
    size_t ofnSize = 0;
    size_t const suffixSize = strlen(suffix);
    dRess_t ress = LZ4IO_createDResources(prefs);
    TIME_t timeStart = TIME_getTime();
    double const cpuStart = cpuTime_sec();

    if (prefs->blockChecksum==0 && prefs->streamChecksum==0) {
        DISPLAYLEVEL(4, "disabling checksum validation during decoding \n");
    }
    /* stdout and /dev/null (test mode) are destinations shared by all input files */
    if (LZ4IO_isStdout(suffix) || LZ4IO_isDevNull(suffix)) {
        ress.dstFile = LZ4IO_openDstFile(suffix, prefs, NULL);
        if (ress.dstFile == NULL) END_PROCESS(76, "Cannot open destination %s", suffix);
    }

    for (i=0; i<ifntSize; i++) {
        unsigned long long processed = 0;
        size_t const ifnSize = strlen(inFileNamesTable[i]);
        const char* const suffixPtr = inFileNamesTable[i] + ifnSize - suffixSize;
        if (LZ4IO_isStdout(suffix) || LZ4IO_isDevNull(suffix)) {
            missingFiles += LZ4IO_decompressSrcFile(&processed, ress, inFileNamesTable[i], suffix, prefs);
            totalProcessed += processed;
            continue;
        }
        if (ifnSize <= suffixSize  || !UTIL_sameString(suffixPtr, suffix) ) {
            DISPLAYLEVEL(1, "File extension doesn't match expected LZ4_EXTENSION (%4s); will not process file: %s\n", suffix, inFileNamesTable[i]);
            skippedFiles++;
            continue;
        }
        if (LZ4IO_reserveFileName(&outFileName, &ofnSize, ifnSize - suffixSize) == NULL)
            END_PROCESS(71, "Memory allocation error");
        memcpy(outFileName, inFileNamesTable[i], ifnSize - suffixSize);
        outFileName[ifnSize-suffixSize] = '\0';
        missingFiles += LZ4IO_decompressDstFile(&processed, ress, inFileNamesTable[i], outFileName, prefs);
        totalProcessed += processed;
    }

    if (ress.dstFile != NULL && ress.dstFile != stdout) fclose(ress.dstFile);
    LZ4IO_freeDResources(ress);
    free(outFileName);
    LZ4IO_finalTimeDisplay(timeStart, cpuStart, totalProcessed);
    return missingFiles + skippedFiles;
}


/* ********************************************************************* */
/* **********************   LZ4 --list command   *********************** */
/* ********************************************************************* */

typedef enum
{
    lz4Frame = 0,
    legacyFrame,
    skippableFrame
} LZ4IO_frameType_t;

typedef struct {
    LZ4F_frameInfo_t lz4FrameInfo;
    LZ4IO_frameType_t frameType;
} LZ4IO_frameInfo_t;

#define LZ4IO_INIT_FRAMEINFO  { LZ4F_INIT_FRAMEINFO, lz4Frame }

typedef struct {
    const char* fileName;
    unsigned long long fileSize;
    unsigned long long frameCount;
    LZ4IO_frameInfo_t frameSummary;
    unsigned short eqFrameTypes;
    unsigned short eqBlockTypes;
    unsigned short allContentSize;
} LZ4IO_cFileInfo_t;

#define LZ4IO_INIT_CFILEINFO  { NULL, 0ULL, 0, LZ4IO_INIT_FRAMEINFO, 1, 1, 1 }

typedef enum { LZ4IO_LZ4F_OK, LZ4IO_format_not_known, LZ4IO_not_a_file } LZ4IO_infoResult;

static const char * LZ4IO_frameTypeNames[] = {"LZ4Frame", "LegacyFrame", "SkippableFrame" };

/* Read block headers and skip block data
   Return total blocks size for this frame including block headers,
   block checksums and content checksums.
   returns 0 in case it can't successfully skip block data.
   Assumes SEEK_CUR after frame header.
 */
static unsigned long long
LZ4IO_skipBlocksData(FILE* finput,
               const LZ4F_blockChecksum_t blockChecksumFlag,
               const LZ4F_contentChecksum_t contentChecksumFlag)
{
    unsigned char blockInfo[LZ4F_BLOCK_HEADER_SIZE];
    unsigned long long totalBlocksSize = 0;
    for (;;) {
        if (fread(blockInfo, 1, LZ4F_BLOCK_HEADER_SIZE, finput) != LZ4F_BLOCK_HEADER_SIZE) {
            return 0;
        }
        totalBlocksSize += LZ4F_BLOCK_HEADER_SIZE;
        {   const unsigned long nextCBlockSize = LZ4IO_readLE32(&blockInfo) & 0x7FFFFFFFU;
            const unsigned long nextBlock = nextCBlockSize + (blockChecksumFlag * LZ4F_BLOCK_CHECKSUM_SIZE);
            if (nextCBlockSize == 0) {
                /* Reached EndMark */
                if (contentChecksumFlag) {
                    /* Skip content checksum */
                    if (fread(blockInfo, 1, LZ4F_CONTENT_CHECKSUM_SIZE, finput) != LZ4F_CONTENT_CHECKSUM_SIZE) {
                        return 0;
                    }
                    totalBlocksSize += LZ4F_CONTENT_CHECKSUM_SIZE;
                }
                break;
            }
            totalBlocksSize += nextBlock;
            /* skip to the next block */
            assert(nextBlock < LONG_MAX);
            if (UTIL_fseek(finput, (long)nextBlock, SEEK_CUR) != 0) return 0;
    }   }
    return totalBlocksSize;
}

static const unsigned long long legacyFrameUndecodable = (0ULL-1);
/* For legacy frames only.
   Read block headers and skip block data.
   Return total blocks size for this frame including block headers.
   or legacyFrameUndecodable in case it can't successfully skip block data.
   This works as long as legacy block header size = magic number size.
   Assumes SEEK_CUR after frame header.
 */
static unsigned long long LZ4IO_skipLegacyBlocksData(FILE* finput)
{
    unsigned char blockInfo[LZ4IO_LEGACY_BLOCK_HEADER_SIZE];
    unsigned long long totalBlocksSize = 0;
    LZ4IO_STATIC_ASSERT(LZ4IO_LEGACY_BLOCK_HEADER_SIZE == MAGICNUMBER_SIZE);
    for (;;) {
        size_t const bhs = fread(blockInfo, 1, LZ4IO_LEGACY_BLOCK_HEADER_SIZE, finput);
        if (bhs == 0) {
            if (feof(finput)) return totalBlocksSize;
            return legacyFrameUndecodable;
        }
        if (bhs != 4) {
            return legacyFrameUndecodable;
        }
        {   const unsigned int nextCBlockSize = LZ4IO_readLE32(&blockInfo);
            if ( nextCBlockSize == LEGACY_MAGICNUMBER
              || nextCBlockSize == LZ4IO_MAGICNUMBER
              || LZ4IO_isSkippableMagicNumber(nextCBlockSize) ) {
                /* Rewind back. we want cursor at the beginning of next frame */
                if (UTIL_fseek(finput, -LZ4IO_LEGACY_BLOCK_HEADER_SIZE, SEEK_CUR) != 0) {
                    END_PROCESS(37, "impossible to skip backward");
                }
                break;
            }
            if (nextCBlockSize > LZ4IO_LEGACY_BLOCK_SIZE_MAX) {
                DISPLAYLEVEL(4, "Error : block in legacy frame is too large \n");
                return legacyFrameUndecodable;
            }
            totalBlocksSize += LZ4IO_LEGACY_BLOCK_HEADER_SIZE + nextCBlockSize;
            if (nextCBlockSize == 0) break;   /* end marker (Linux kernel) */
            /* skip to the next block
             * note : this won't fail if nextCBlockSize is too large, skipping past the end of finput */
            if (UTIL_fseek(finput, nextCBlockSize, SEEK_CUR) != 0) {
                return legacyFrameUndecodable;
    }   }   }
    return totalBlocksSize;
}

/* LZ4IO_blockTypeID:
 * return human-readable block type, following command line convention
 * buffer : must be a valid memory area of at least 4 bytes */
const char* LZ4IO_blockTypeID(LZ4F_blockSizeID_t sizeID, LZ4F_blockMode_t blockMode, char buffer[4])
{
    buffer[0] = 'B';
    assert(sizeID >= 4); assert(sizeID <= 7);
    buffer[1] = (char)(sizeID + '0');
    buffer[2] = (blockMode == LZ4F_blockIndependent) ? 'I' : 'D';
    buffer[3] = 0;
    return buffer;
}

/* buffer : must be valid memory area of at least 10 bytes */
static const char* LZ4IO_toHuman(long double size, char* buf)
{
    const char units[] = {"\0KMGTPEZY"};
    size_t i = 0;
    for (; size >= 1024; i++) size /= 1024;
    sprintf(buf, "%.2Lf%c", size, units[i]);
    return buf;
}

/* Get filename without path prefix */
static const char* LZ4IO_baseName(const char* input_filename)
{
    const char* b = strrchr(input_filename, '/');
    if (!b) b = strrchr(input_filename, '\\');
    if (!b) return input_filename;
    return b + 1;
}

/* Report frame/s information (--list) in verbose mode (-v).
 * Will populate file info with fileName and frameSummary where applicable.
 * - TODO :
 *  + report nb of blocks, hence max. possible decompressed size (when not reported in header)
 */
static LZ4IO_infoResult
LZ4IO_getCompressedFileInfo(LZ4IO_cFileInfo_t* cfinfo, const char* input_filename, int displayNow)
{
    LZ4IO_infoResult result = LZ4IO_format_not_known;  /* default result (error) */
    unsigned char buffer[LZ4F_HEADER_SIZE_MAX];
    FILE* const finput = LZ4IO_openSrcFile(input_filename);

    if (finput == NULL) return LZ4IO_not_a_file;
    cfinfo->fileSize = UTIL_getOpenFileSize(finput);

    while (!feof(finput)) {
        LZ4IO_frameInfo_t frameInfo = LZ4IO_INIT_FRAMEINFO;
        unsigned magicNumber;
        /* Get MagicNumber */
        {   size_t const nbReadBytes = fread(buffer, 1, MAGICNUMBER_SIZE, finput);
            if (nbReadBytes == 0) { break; } /* EOF */
            result = LZ4IO_format_not_known;  /* default result (error) */
            if (nbReadBytes != MAGICNUMBER_SIZE) {
                END_PROCESS(40, "Unrecognized header : Magic Number unreadable");
        }   }
        magicNumber = LZ4IO_readLE32(buffer);   /* Little Endian format */
        if (LZ4IO_isSkippableMagicNumber(magicNumber))
            magicNumber = LZ4IO_SKIPPABLE0;   /* fold skippable magic numbers */

        switch (magicNumber) {
        case LZ4IO_MAGICNUMBER:
            if (cfinfo->frameSummary.frameType != lz4Frame) cfinfo->eqFrameTypes = 0;
            /* Get frame info */
            {   const size_t readBytes = fread(buffer + MAGICNUMBER_SIZE, 1, LZ4F_HEADER_SIZE_MIN - MAGICNUMBER_SIZE, finput);
                if (ferror(finput)) END_PROCESS(71, "Error reading %s", input_filename);
                if (readBytes != LZ4F_HEADER_SIZE_MIN - MAGICNUMBER_SIZE) break;   /* truncated frame header */
            }
            {   size_t hSize = LZ4F_headerSize(&buffer, LZ4F_HEADER_SIZE_MIN);
                if (LZ4F_isError(hSize)) break;
                if (hSize > LZ4F_HEADER_SIZE_MIN) {
                    /* We've already read LZ4F_HEADER_SIZE_MIN so read any extra until hSize*/
                    const size_t readBytes = fread(buffer + LZ4F_HEADER_SIZE_MIN, 1, hSize - LZ4F_HEADER_SIZE_MIN, finput);
                    if (ferror(finput)) END_PROCESS(72, "Error reading %s", input_filename);
                    if (readBytes != hSize - LZ4F_HEADER_SIZE_MIN) break;   /* truncated frame header */
                }
                /* Create decompression context */
                {   LZ4F_dctx* dctx;
                    if ( LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION)) ) break;
                    {   unsigned const frameInfoError = LZ4F_isError(LZ4F_getFrameInfo(dctx, &frameInfo.lz4FrameInfo, buffer, &hSize));
                        LZ4F_freeDecompressionContext(dctx);
                        if (frameInfoError) break;
                        if ((cfinfo->frameSummary.lz4FrameInfo.blockSizeID != frameInfo.lz4FrameInfo.blockSizeID ||
                                cfinfo->frameSummary.lz4FrameInfo.blockMode != frameInfo.lz4FrameInfo.blockMode)
                                && cfinfo->frameCount != 0)
                            cfinfo->eqBlockTypes = 0;
                        {   const unsigned long long totalBlocksSize = LZ4IO_skipBlocksData(finput,
                                    frameInfo.lz4FrameInfo.blockChecksumFlag,
                                    frameInfo.lz4FrameInfo.contentChecksumFlag);
                            if (totalBlocksSize) {
                                char bTypeBuffer[5];
                                LZ4IO_blockTypeID(frameInfo.lz4FrameInfo.blockSizeID, frameInfo.lz4FrameInfo.blockMode, bTypeBuffer);
                                if (displayNow) DISPLAYOUT("    %6llu %14s %5s %8s",
                                             cfinfo->frameCount + 1,
                                             LZ4IO_frameTypeNames[frameInfo.frameType],
                                             bTypeBuffer,
                                             frameInfo.lz4FrameInfo.contentChecksumFlag ? "XXH32" : "-");
                                if (frameInfo.lz4FrameInfo.contentSize) {
                                    double const ratio = (double)(totalBlocksSize + hSize) / (double)frameInfo.lz4FrameInfo.contentSize * 100;
                                    if (displayNow) DISPLAYOUT(" %20llu %20llu %9.2f%%\n",
                                                    totalBlocksSize + hSize,
                                                    frameInfo.lz4FrameInfo.contentSize,
                                                    ratio);
                                    /* Now we've consumed frameInfo we can use it to store the total contentSize */
                                    frameInfo.lz4FrameInfo.contentSize += cfinfo->frameSummary.lz4FrameInfo.contentSize;
                                }
                                else {
                                    if (displayNow) DISPLAYOUT(" %20llu %20s %9s \n", totalBlocksSize + hSize, "-", "-");
                                    cfinfo->allContentSize = 0;
                                }
                                result = LZ4IO_LZ4F_OK;
            }   }   }   }   }
            break;
        case LEGACY_MAGICNUMBER:
            frameInfo.frameType = legacyFrame;
            if (cfinfo->frameSummary.frameType != legacyFrame && cfinfo->frameCount != 0) cfinfo->eqFrameTypes = 0;
            cfinfo->eqBlockTypes = 0;
            cfinfo->allContentSize = 0;
            {   const unsigned long long totalBlocksSize = LZ4IO_skipLegacyBlocksData(finput);
                if (totalBlocksSize == legacyFrameUndecodable) {
                    DISPLAYLEVEL(1, "Corrupted legacy frame \n");
                    result = LZ4IO_format_not_known;
                    break;
                }
                if (totalBlocksSize) {
                    if (displayNow) DISPLAYOUT("    %6llu %14s %5s %8s %20llu %20s %9s\n",
                                 cfinfo->frameCount + 1,
                                 LZ4IO_frameTypeNames[frameInfo.frameType],
                                 "-", "-",
                                 totalBlocksSize + 4,
                                 "-", "-");
                    result = LZ4IO_LZ4F_OK;
            }   }
            break;
        case LZ4IO_SKIPPABLE0:
            frameInfo.frameType = skippableFrame;
            if (cfinfo->frameSummary.frameType != skippableFrame && cfinfo->frameCount != 0) cfinfo->eqFrameTypes = 0;
            cfinfo->eqBlockTypes = 0;
            cfinfo->allContentSize = 0;
            {   size_t const nbReadBytes = fread(buffer, 1, 4, finput);
                if (nbReadBytes != 4)
                    END_PROCESS(42, "Stream error : skippable size unreadable");
            }
            {   unsigned const size = LZ4IO_readLE32(buffer);
                int const errorNb = fseek_u32(finput, size, SEEK_CUR);
                if (errorNb != 0)
                    END_PROCESS(43, "Stream error : cannot skip skippable area");
                if (displayNow) DISPLAYOUT("    %6llu %14s %5s %8s %20u %20s %9s\n",
                             cfinfo->frameCount + 1,
                             "SkippableFrame",
                             "-", "-", size + 8, "-", "-");

                result = LZ4IO_LZ4F_OK;
            }
            break;
        default:
            {   long int const position = ftell(finput);  /* only works for files < 2 GB */
                DISPLAYLEVEL(3, "Stream followed by undecodable data ");
                if (position != -1L)
                    DISPLAYLEVEL(3, "at position %i ", (int)position);
                result = LZ4IO_format_not_known;
                DISPLAYLEVEL(3, "\n");
            }
        break;
        }
        if (result != LZ4IO_LZ4F_OK) break;
        cfinfo->frameSummary = frameInfo;
        cfinfo->frameCount++;
    }  /* while (!feof(finput)) */
    fclose(finput);
    return result;
}


int LZ4IO_displayCompressedFilesInfo(const char** inFileNames, size_t ifnIdx)
{
    int result = 0;
    size_t idx = 0;
    if (g_displayLevel < 3) {
        DISPLAYOUT("%10s %14s %5s %11s %13s %8s   %s\n",
                "Frames", "Type", "Block", "Compressed", "Uncompressed", "Ratio", "Filename");
    }
    for (; idx < ifnIdx; idx++) {
        /* Get file info */
        LZ4IO_cFileInfo_t cfinfo = LZ4IO_INIT_CFILEINFO;
        cfinfo.fileName = LZ4IO_baseName(inFileNames[idx]);
        if (LZ4IO_isStdin(inFileNames[idx]) ? !UTIL_isRegFD(0) : !UTIL_isRegFile(inFileNames[idx])) {
            DISPLAYLEVEL(1, "lz4: %s is not a regular file \n", inFileNames[idx]);
            return 1;
        }
        if (g_displayLevel >= 3) {
            /* verbose mode */
            DISPLAYOUT("%s(%llu/%llu)\n", cfinfo.fileName, (unsigned long long)idx + 1, (unsigned  long long)ifnIdx);
            DISPLAYOUT("    %6s %14s %5s %8s %20s %20s %9s\n",
                        "Frame", "Type", "Block", "Checksum", "Compressed", "Uncompressed", "Ratio");
        }
        {   LZ4IO_infoResult const op_result = LZ4IO_getCompressedFileInfo(&cfinfo, inFileNames[idx], g_displayLevel >= 3);
            if (op_result != LZ4IO_LZ4F_OK) {
                assert(op_result == LZ4IO_format_not_known);
                DISPLAYLEVEL(1, "lz4: %s: File format not recognized \n", inFileNames[idx]);
                return 1;
        }   }
        if (g_displayLevel >= 3) {
            DISPLAYOUT("\n");
        }
        if (g_displayLevel < 3) {
            /* Display summary */
            char buffers[3][10];
            DISPLAYOUT("%10llu %14s %5s %11s %13s ",
                    cfinfo.frameCount,
                    cfinfo.eqFrameTypes ? LZ4IO_frameTypeNames[cfinfo.frameSummary.frameType] : "-" ,
                    cfinfo.eqBlockTypes ? LZ4IO_blockTypeID(cfinfo.frameSummary.lz4FrameInfo.blockSizeID,
                                                            cfinfo.frameSummary.lz4FrameInfo.blockMode, buffers[0]) : "-",
                    LZ4IO_toHuman((long double)cfinfo.fileSize, buffers[1]),
                    cfinfo.allContentSize ? LZ4IO_toHuman((long double)cfinfo.frameSummary.lz4FrameInfo.contentSize, buffers[2]) : "-");
            if (cfinfo.allContentSize) {
                double const ratio = (double)cfinfo.fileSize / (double)cfinfo.frameSummary.lz4FrameInfo.contentSize * 100;
                DISPLAYOUT("%8.2f%%  %s \n", ratio, cfinfo.fileName);
            } else {
                DISPLAYOUT("%8s   %s\n",
                        "-",
                        cfinfo.fileName);
        }   }  /* if (g_displayLevel < 3) */
    }  /* for (; idx < ifnIdx; idx++) */

    return result;
}
