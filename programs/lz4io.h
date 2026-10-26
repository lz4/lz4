/*
  LZ4io.h - LZ4 File/Stream Interface
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

#ifndef LZ4IO_H_237902873
#define LZ4IO_H_237902873

/*---   Dependency   ---*/
#include <stddef.h>   /* size_t */
#include "lz4frame.h"  /* LZ4F_preferences_t */


/* ************************************************** */
/* Special input/output values                        */
/* ************************************************** */
#define stdinmark  "stdin"
#define stdoutmark "stdout"
#define NULL_OUTPUT "null"
#ifdef _WIN32
#define nulmark "nul"
#else
#define nulmark "/dev/null"
#endif

/* ************************************************** */
/* ****************** Type Definitions ************** */
/* ************************************************** */

typedef struct LZ4IO_prefs_s LZ4IO_prefs_t;

LZ4IO_prefs_t* LZ4IO_defaultPreferences(void);
void LZ4IO_freePreferences(LZ4IO_prefs_t* prefs);


/* *************************************************** */
/* ****************** Processing ********************* */
/* *************************************************** */

/* if output_filename == stdoutmark, writes to stdout */
int LZ4IO_compressFilename(const char* input_filename, const char* output_filename, int compressionlevel, const LZ4IO_prefs_t* prefs);
int LZ4IO_decompressFilename(const char* input_filename, const char* output_filename, const LZ4IO_prefs_t* prefs);

/* if suffix == stdoutmark, writes to stdout */
int LZ4IO_compressMultipleFilenames(const char** inFileNamesTable, int ifntSize, const char* suffix, int compressionlevel, const LZ4IO_prefs_t* prefs);
int LZ4IO_decompressMultipleFilenames(const char** inFileNamesTable, int ifntSize, const char* suffix, const LZ4IO_prefs_t* prefs);


/* ************************************************** */
/* ****************** Parameters ******************** */
/* ************************************************** */

/* nbWorkers : 0 == automatic : nb of cores ; >= 1 : explicit.
 *  Frame format : within memory budget (see LZ4IO_setMemoryBudget()),
 *  and no more than the nb of jobs of each file (when its size is known).
 *  Capped as within liblz4 (LZ4F_getMaxNbWorkers()), for frame and legacy formats : 1 without its multi-threading support.
 * @return : nb of workers applied */
int LZ4IO_setNbWorkers(LZ4IO_prefs_t* const prefs, int nbWorkers);

/* Default setting : 0 == frames cut into jobs, compressed by nbWorkers worker threads and the main thread.
 * 1 == frames compressed by the main thread alone, in liblz4's single-threaded mode (nbWorkers == 0) :
 * with linked blocks, output can differ (slightly) */
int LZ4IO_setSingleThread(LZ4IO_prefs_t* const prefs, int enable);
int LZ4IO_defaultNbWorkers(void);

/* memory budget of multi-threaded compression, in bytes : limits the nb of workers ;
 * 0 : default budget (LZ4IO_defaultMemoryBudget()) */
unsigned long long LZ4IO_setMemoryBudget(LZ4IO_prefs_t* const prefs, unsigned long long budget);
unsigned long long LZ4IO_defaultMemoryBudget(void);   /* 4 GB, 1 GB on 32-bit systems */

int LZ4IO_setDictionaryFilename(LZ4IO_prefs_t* const prefs, const char* dictionaryFilename);

/* Default setting : passThrough = 0;
   return : passThrough mode (0/1) */
int LZ4IO_setPassThrough(LZ4IO_prefs_t* const prefs, int yes);

/* Default setting : overwrite = 1;
   return : overwrite mode (0/1) */
int LZ4IO_setOverwrite(LZ4IO_prefs_t* const prefs, int yes);

/* Default setting : testMode = 0;
   return : testMode (0/1) */
int LZ4IO_setTestMode(LZ4IO_prefs_t* const prefs, int yes);

/* blockSizeID : valid values : 4-5-6-7
   return : 0 if error, blockSize if OK */
size_t LZ4IO_setBlockSizeID(LZ4IO_prefs_t* const prefs, unsigned blockSizeID);

/* blockSize : valid values : 32 -> 4MB
   return : 0 if error, actual blocksize if OK */
size_t LZ4IO_setBlockSize(LZ4IO_prefs_t* const prefs, size_t blockSize);

/* Default setting : independent blocks */
typedef enum { LZ4IO_blockLinked=0, LZ4IO_blockIndependent} LZ4IO_blockMode_t;
int LZ4IO_setBlockMode(LZ4IO_prefs_t* const prefs, LZ4IO_blockMode_t blockMode);

/* Default setting : no block checksum */
int LZ4IO_setBlockChecksumMode(LZ4IO_prefs_t* const prefs, int xxhash);

/* Default setting : stream checksum enabled */
int LZ4IO_setStreamChecksumMode(LZ4IO_prefs_t* const prefs, int xxhash);

/* Default setting : 0 (no notification) */
int LZ4IO_setNotificationLevel(int level);

/* Default setting : 0 (disabled) */
int LZ4IO_setSparseFile(LZ4IO_prefs_t* const prefs, int enable);

/* Default setting : 1 == regular files are memory-mapped for compression, when supported.
 * Note : a file truncated during its compression, or unreadable (I/O error), raises SIGBUS :
 * a handler, installed while the file is mapped, reports it and terminates the process (error 53).
 * A file modified during its compression is also detected (kept with --rm, as a file renamed or replaced) :
 * input read once only triggers a warning ;
 * memory-mapped input is read several times : when its content can't be verified unchanged
 * (using the frame checksum, ex: file only grown), the output is removed (error 54). */
int LZ4IO_setMmap(LZ4IO_prefs_t* const prefs, int enable);

/* Default setting : 0 == no content size present in frame header */
int LZ4IO_setContentSize(LZ4IO_prefs_t* const prefs, int enable);

/* Default setting : 0 == src file preserved */
void LZ4IO_setRemoveSrcFile(LZ4IO_prefs_t* const prefs, unsigned flag);

/* Default setting : 0 == favor compression ratio
 * Note : 1 only works for high compression levels (10+) */
void LZ4IO_favorDecSpeed(LZ4IO_prefs_t* const prefs, int favor);

/* LZ4IO_framePreferences() :
 * frame parameters of compressed files, excluding compression level and nb of workers :
 * frameInfo.contentSize != 0 means that it's set to each file's size (--content-size).
 * @return : 0, or custom block size (blocks of this size, one LZ4F_compressUpdate() invocation per block, with autoFlush) */
size_t LZ4IO_framePreferences(const LZ4IO_prefs_t* prefs, LZ4F_preferences_t* framePrefs);


/* implement --list
 * @return 0 on success, 1 on error */
int LZ4IO_displayCompressedFilesInfo(const char** inFileNames, size_t ifnIdx);


#endif  /* LZ4IO_H_237902873 */
