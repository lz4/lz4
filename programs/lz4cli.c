/*
  LZ4cli.c - LZ4 Command Line Interface
  Copyright (C) Yann Collet 2011-2014
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
  - LZ4 source repository : http://code.google.com/p/lz4/
  - LZ4 public forum : https://groups.google.com/forum/#!forum/lz4c
*/
/*
  Note : this is stand-alone program.
  It is not part of LZ4 compression library, it is a user program of the LZ4 library.
  The license of LZ4 library is BSD.
  The license of xxHash library is BSD.
  The license of this compression CLI program is GPLv2.
*/

//**************************************
// Tuning parameters
//**************************************
// DISABLE_LZ4C_LEGACY_OPTIONS :
// Control the availability of -c0, -c1 and -hc legacy arguments
// Default : Legacy options are enabled
// #define DISABLE_LZ4C_LEGACY_OPTIONS


//**************************************
// Compiler Options
//**************************************
// Disable some Visual warning messages
#ifdef _MSC_VER  // Visual Studio
#  define _CRT_SECURE_NO_WARNINGS
#  define _CRT_SECURE_NO_DEPRECATE     // VS2005
#  pragma warning(disable : 4127)      // disable: C4127: conditional expression is constant
#endif

#define _FILE_OFFSET_BITS 64   // Large file support on 32-bits unix
#define _POSIX_SOURCE 1        // for fileno() within <stdio.h> on unix


//****************************
// Includes
//****************************
#include <stdio.h>    // fprintf, fopen, fread, _fileno, stdin, stdout
#include <stdlib.h>   // malloc
#include <string.h>   // strcmp, strlen
#include <time.h>     // clock
#include "lz4.h"
#include "lz4hc.h"
#include "xxhash.h"
#include "bench.h"
#include "lz4io.h"


//****************************
// OS-specific Includes
//****************************
#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(_WIN32) || defined(__CYGWIN__)
#  include <fcntl.h>    // _O_BINARY
#  include <io.h>       // _setmode, _isatty
#  ifdef __MINGW32__
   int _fileno(FILE *stream);   // MINGW somehow forgets to include this windows declaration into <stdio.h>
#  endif
#  define SET_BINARY_MODE(file) _setmode(_fileno(file), _O_BINARY)
#  define IS_CONSOLE(stdStream) _isatty(_fileno(stdStream))
#else
#  include <unistd.h>   // isatty
#  define SET_BINARY_MODE(file)
#  define IS_CONSOLE(stdStream) isatty(fileno(stdStream))
#endif


//**************************************
// Compiler-specific functions
//**************************************
#define GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)

#if defined(_MSC_VER)    // Visual Studio
#  define swap32 _byteswap_ulong
#elif (GCC_VERSION >= 403) || defined(__clang__)
#  define swap32 __builtin_bswap32
#else
  static inline unsigned int swap32(unsigned int x)
  {
    return ((x << 24) & 0xff000000 ) |
           ((x <<  8) & 0x00ff0000 ) |
           ((x >>  8) & 0x0000ff00 ) |
           ((x >> 24) & 0x000000ff );
  }
#endif


//****************************
// Constants
//****************************
#define COMPRESSOR_NAME "LZ4 Compression CLI"
#ifndef LZ4_VERSION
#  define LZ4_VERSION "r125"
#endif
#define AUTHOR "Yann Collet"
#define WELCOME_MESSAGE "*** %s %i-bits %s, by %s (%s) ***\n", COMPRESSOR_NAME, (int)(sizeof(void*)*8), LZ4_VERSION, AUTHOR, __DATE__
#define LZ4_EXTENSION ".lz4"
#define LZ4_CAT "lz4cat"

#define KB *(1U<<10)
#define MB *(1U<<20)
#define GB *(1U<<30)

#define LZ4_BLOCKSIZEID_DEFAULT 7


//**************************************
// Macros
//**************************************
#define DISPLAY(...)         fprintf(stderr, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...) if (displayLevel>=l) { DISPLAY(__VA_ARGS__); }


//**************************************
// Local Parameters
//**************************************
static char* programName;
static int displayLevel = 2;   // 0 : no display  // 1: errors  // 2 : + result + interaction + warnings ;  // 3 : + progression;  // 4 : + information


//**************************************
// Exceptions
//**************************************
#define DEBUG 0
#define DEBUGOUTPUT(...) if (DEBUG) DISPLAY(__VA_ARGS__);
#define EXM_THROW(error, ...)                                             \
{                                                                         \
    DEBUGOUTPUT("Error defined at %s, line %i : \n", __FILE__, __LINE__); \
    DISPLAYLEVEL(1, "Error %i : ", error);                                \
    DISPLAYLEVEL(1, __VA_ARGS__);                                         \
    DISPLAYLEVEL(1, "\n");                                                \
    exit(error);                                                          \
}


//**************************************
// Version modifiers
//**************************************
#define EXTENDED_ARGUMENTS
#define EXTENDED_HELP
#define EXTENDED_FORMAT
#define DEFAULT_COMPRESSOR   LZ4IO_compressFilename
#define DEFAULT_DECOMPRESSOR LZ4IO_decompressFilename
int LZ4IO_compressFilename_Legacy(char* input_filename, char* output_filename, int compressionlevel);


//****************************
// Functions
//****************************
int usage(void)
{
    DISPLAY( "Usage :\n");
    DISPLAY( "      %s [arg] [input] [output]\n", programName);
    DISPLAY( "\n");
    DISPLAY( "input   : a filename\n");
    DISPLAY( "          with no FILE, or when FILE is - or %s, read standard input\n", stdinmark);
    DISPLAY( "Arguments :\n");
    DISPLAY( " -1     : Fast compression (default) \n");
    DISPLAY( " -9     : High compression \n");
    DISPLAY( " -d     : decompression (default for %s extension)\n", LZ4_EXTENSION);
    DISPLAY( " -z     : force compression\n");
    DISPLAY( " -f     : overwrite output without prompting \n");
    DISPLAY( " -h/-H  : display help/long help and exit\n");
    return 0;
}

int usage_advanced(void)
{
    DISPLAY(WELCOME_MESSAGE);
    usage();
    DISPLAY( "\n");
    DISPLAY( "Advanced arguments :\n");
    DISPLAY( " -V     : display Version number and exit\n");
    DISPLAY( " -v     : verbose mode\n");
    DISPLAY( " -q     : suppress warnings; specify twice to suppress errors too\n");
    DISPLAY( " -c     : force write to standard output, even if it is the console\n");
    DISPLAY( " -t     : test compressed file integrity\n");
    DISPLAY( " -l     : compress using Legacy format (Linux kernel compression)\n");
    DISPLAY( " -B#    : Block size [4-7](default : 7)\n");
    DISPLAY( " -BD    : Block dependency (improve compression ratio)\n");
    DISPLAY( " -BX    : enable block checksum (default:disabled)\n");
    DISPLAY( " -Sx    : disable stream checksum (default:enabled)\n");
    DISPLAY( "Benchmark arguments :\n");
    DISPLAY( " -b     : benchmark file(s)\n");
    DISPLAY( " -i#    : iteration loops [1-9](default : 3), benchmark mode only\n");
#if !defined(DISABLE_LZ4C_LEGACY_OPTIONS)
    DISPLAY( "Legacy arguments :\n");
    DISPLAY( " -c0    : fast compression\n");
    DISPLAY( " -c1    : high compression\n");
    DISPLAY( " -hc    : high compression\n");
    DISPLAY( " -y     : overwrite output without prompting \n");
    DISPLAY( " -s     : suppress warnings \n");
#endif // DISABLE_LZ4C_LEGACY_OPTIONS
    EXTENDED_HELP;
    return 0;
}

int usage_longhelp(void)
{
    DISPLAY( "\n");
    DISPLAY( "Which values can get [output] ? \n");
    DISPLAY( "[output] : a filename\n");
    DISPLAY( "          '%s', or '-' for standard output (pipe mode)\n", stdoutmark);
    DISPLAY( "          '%s' to discard output (test mode)\n", NULL_OUTPUT);
    DISPLAY( "[output] can be left empty. In this case, it receives the following value : \n");
    DISPLAY( "          - if stdout is not the console, then [output] = stdout \n");
    DISPLAY( "          - if stdout is console : \n");
    DISPLAY( "               + if compression selected, output to filename%s \n", LZ4_EXTENSION);
    DISPLAY( "               + if decompression selected, output to filename without '%s'\n", LZ4_EXTENSION);
    DISPLAY( "                    > if input filename has no '%s' extension : error\n", LZ4_EXTENSION);
    DISPLAY( "\n");
    DISPLAY( "Compression levels : \n");
    DISPLAY( "There are technically 2 accessible compression levels.\n");
    DISPLAY( "-0 ... -2 => Fast compression\n");
    DISPLAY( "-3 ... -9 => High compression\n");
    DISPLAY( "\n");
    DISPLAY( "stdin, stdout and the console : \n");
    DISPLAY( "To protect the console from binary flooding (bad argument mistake)\n");
    DISPLAY( "%s will refuse to read from console, or write to console \n", programName);
    DISPLAY( "except if '-c' command is specified, to force output to console \n");
    DISPLAY( "\n");
    DISPLAY( "Simple example :\n");
    DISPLAY( "1 : compress 'filename' fast, using default output name 'filename.lz4'\n");
    DISPLAY( "          %s filename\n", programName);
    DISPLAY( "\n");
    DISPLAY( "Arguments can be appended together, or provided independently. For example :\n");
    DISPLAY( "2 : compress 'filename' in high compression mode, overwrite output if exists\n");
    DISPLAY( "          %s -f9 filename \n", programName);
    DISPLAY( "    is equivalent to :\n");
    DISPLAY( "          %s -f -9 filename \n", programName);
    DISPLAY( "\n");
    DISPLAY( "%s can be used in 'pure pipe mode', for example :\n", programName);
    DISPLAY( "3 : compress data stream from 'generator', send result to 'consumer'\n");
    DISPLAY( "          generator | %s | consumer \n", programName);
#if !defined(DISABLE_LZ4C_LEGACY_OPTIONS)
    DISPLAY( "\n");
    DISPLAY( "Warning :\n");
    DISPLAY( "Legacy arguments take precedence. Therefore : \n");
    DISPLAY( "          %s -hc filename\n", programName);
    DISPLAY( "means 'compress filename in high compression mode'\n");
    DISPLAY( "It is not equivalent to :\n");
    DISPLAY( "          %s -h -c filename\n", programName);
    DISPLAY( "which would display help text and exit\n");
#endif // DISABLE_LZ4C_LEGACY_OPTIONS
    return 0;
}

int badusage(void)
{
    DISPLAYLEVEL(1, "Incorrect parameters\n");
    if (displayLevel >= 1) usage();
    exit(1);
}


void waitEnter(void)
{
    DISPLAY("Press enter to continue...\n");
    getchar();
}


int main(int argc, char** argv)
{
    int i,
        cLevel=0,
        decode=0,
        bench=0,
        filenamesStart=2,
        legacy_format=0,
        forceStdout=0,
        forceCompress=0,
        pause=0;
    char* input_filename=0;
    char* output_filename=0;
    char* dynNameSpace=0;
    char nullOutput[] = NULL_OUTPUT;
    char extension[] = LZ4_EXTENSION;
    int blockSize;

    // Init
    programName = argv[0];
    LZ4IO_setOverwrite(0);
    blockSize = LZ4IO_setBlockSizeID(LZ4_BLOCKSIZEID_DEFAULT);

    // lz4cat behavior
    if (!strcmp(programName, LZ4_CAT)) { decode=1; forceStdout=1; output_filename=stdoutmark; displayLevel=1; }

    // command switches
    for(i=1; i<argc; i++)
    {
        char* argument = argv[i];

        if(!argument) continue;   // Protection if argument empty

        // Decode command (note : aggregated commands are allowed)
        if (argument[0]=='-')
        {
            // '-' means stdin/stdout
            if (argument[1]==0)
            {
                if (!input_filename) input_filename=stdinmark;
                else output_filename=stdoutmark;
            }

            while (argument[1]!=0)
            {
                argument ++;

#if !defined(DISABLE_LZ4C_LEGACY_OPTIONS)
                // Legacy options (-c0, -c1, -hc, -y, -s)
                if ((argument[0]=='c') && (argument[1]=='0')) { cLevel=0; argument++; continue; }          // -c0 (fast compression)
                if ((argument[0]=='c') && (argument[1]=='1')) { cLevel=9; argument++; continue; }          // -c1 (high compression)
                if ((argument[0]=='h') && (argument[1]=='c')) { cLevel=9; argument++; continue; }          // -hc (high compression)
                if (*argument=='y') { LZ4IO_setOverwrite(1); continue; }                                   // -y (answer 'yes' to overwrite permission)
                if (*argument=='s') { displayLevel=1; continue; }                                          // -s (silent mode)
#endif // DISABLE_LZ4C_LEGACY_OPTIONS

                if ((*argument>='0') && (*argument<='9'))
                {
                    cLevel = 0;
                    while ((*argument >= '0') && (*argument <= '9'))
                    {
                        cLevel *= 10;
                        cLevel += *argument - '0';
                        argument++;
                    }
                    argument--;
                    continue;
                }

                switch(argument[0])
                {
                    // Display help
                case 'V': DISPLAY(WELCOME_MESSAGE); return 0;   // Version
                case 'h': usage_advanced(); return 0;
                case 'H': usage_advanced(); usage_longhelp(); return 0;

                    // Compression (default)
                case 'z': forceCompress = 1; break;

                    // Use Legacy format (for Linux kernel compression)
                case 'l': legacy_format=1; break;

                    // Decoding
                case 'd': decode=1; break;

                    // Force stdout, even if stdout==console
                case 'c': forceStdout=1; output_filename=stdoutmark; displayLevel=1; break;

                    // Test
                case 't': decode=1; LZ4IO_setOverwrite(1); output_filename=nulmark; break;

                    // Overwrite
                case 'f': LZ4IO_setOverwrite(1); break;

                    // Verbose mode
                case 'v': displayLevel=4; break;

                    // Quiet mode
                case 'q': displayLevel--; break;

                    // keep source file (default anyway, so useless) (for xz/lzma compatibility)
                case 'k': break;

                    // Modify Block Properties
                case 'B':
                    while (argument[1]!=0)
                    {
                        int exitBlockProperties=0;
                        switch(argument[1])
                        {
                        case '4':
                        case '5':
                        case '6':
                        case '7':
                        {
                            int B = argument[1] - '0';
                            blockSize = LZ4IO_setBlockSizeID(B);
                            BMK_SetBlocksize(blockSize);
                            argument++;
                            break;
                        }
                        case 'D': LZ4IO_setBlockMode(chainedBlocks); argument++; break;
                        case 'X': LZ4IO_setBlockChecksumMode(1); argument ++; break;
                        default : exitBlockProperties=1;
                        }
                        if (exitBlockProperties) break;
                    }
                    break;

                    // Modify Stream properties
                case 'S': if (argument[1]=='x') { LZ4IO_setStreamChecksumMode(0); argument++; break; } else { badusage(); }

                    // Benchmark
                case 'b': bench=1; break;

                    // Modify Nb Iterations (benchmark only)
                case 'i':
                    if ((argument[1] >='1') && (argument[1] <='9'))
                    {
                        int iters = argument[1] - '0';
                        BMK_SetNbIterations(iters);
                        argument++;
                    }
                    break;

                    // Pause at the end (hidden option)
                case 'p': pause=1; BMK_SetPause(); break;

                EXTENDED_ARGUMENTS;

                    // Unrecognised command
                default : badusage();
                }
            }
            continue;
        }

        // first provided filename is input
        if (!input_filename) { input_filename=argument; filenamesStart=i; continue; }

        // second provided filename is output
        if (!output_filename)
        {
            output_filename=argument;
            if (!strcmp (output_filename, nullOutput)) output_filename = nulmark;
            continue;
        }
    }

    DISPLAYLEVEL(3, WELCOME_MESSAGE);
    if (!decode) DISPLAYLEVEL(4, "Blocks size : %i KB\n", blockSize>>10);

    // No input filename ==> use stdin
    if(!input_filename) { input_filename=stdinmark; }

    // Check if input or output are defined as console; trigger an error in this case
    if (!strcmp(input_filename, stdinmark)  && IS_CONSOLE(stdin)                 ) badusage();

    // Check if benchmark is selected
    if (bench) return BMK_benchFile(argv+filenamesStart, argc-filenamesStart, cLevel);

    // No output filename ==> try to select one automatically (when possible)
    while (!output_filename)
    {
        if (!IS_CONSOLE(stdout)) { output_filename=stdoutmark; break; }   // Default to stdout whenever possible (i.e. not a console)
        if ((!decode) && !(forceCompress))   // auto-determine compression or decompression, based on file extension
        {
            size_t l = strlen(input_filename);
            if (!strcmp(input_filename+(l-4), LZ4_EXTENSION)) decode=1;
        }
        if (!decode)   // compression to file
        {
            size_t l = strlen(input_filename);
            dynNameSpace = (char*)calloc(1,l+5);
            output_filename = dynNameSpace;
            strcpy(output_filename, input_filename);
            strcpy(output_filename+l, LZ4_EXTENSION);
            DISPLAYLEVEL(2, "Compressed filename will be : %s \n", output_filename);
            break;
        }
        // decompression to file (automatic name will work only if input filename has correct format extension)
        {
            size_t outl;
            size_t inl = strlen(input_filename);
            dynNameSpace = (char*)calloc(1,inl+1);
            output_filename = dynNameSpace;
            strcpy(output_filename, input_filename);
            outl = inl;
            if (inl>4)
                while ((outl >= inl-4) && (input_filename[outl] ==  extension[outl-inl+4])) output_filename[outl--]=0;
            if (outl != inl-5) { DISPLAYLEVEL(1, "Cannot determine an output filename\n"); badusage(); }
            DISPLAYLEVEL(2, "Decoding file %s \n", output_filename);
        }
    }

    // No warning message in pure pipe mode (stdin + stdout)
    if (!strcmp(input_filename, stdinmark) && !strcmp(output_filename,stdoutmark) && (displayLevel==2)) displayLevel=1;

    // Check if input or output are defined as console; trigger an error in this case
    if (!strcmp(input_filename, stdinmark)  && IS_CONSOLE(stdin)                 ) badusage();
    if (!strcmp(output_filename,stdoutmark) && IS_CONSOLE(stdout) && !forceStdout) badusage();

    // IO Stream/File
    LZ4IO_setNotificationLevel(displayLevel);
    if (decode) DEFAULT_DECOMPRESSOR(input_filename, output_filename);
    else
    // compression is default action
    {
        if (legacy_format)
        {
            DISPLAYLEVEL(3, "! Generating compressed LZ4 using Legacy format (deprecated !) ! \n");
            LZ4IO_compressFilename_Legacy(input_filename, output_filename, cLevel);
        }
        else
        {
            DEFAULT_COMPRESSOR(input_filename, output_filename, cLevel);
        }
    }

    if (pause) waitEnter();
    free(dynNameSpace);
    return 0;
}
