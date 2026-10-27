lz4(1) -- lz4, unlz4, lz4cat - Compress or decompress .lz4 files
================================================================

SYNOPSIS
--------

`lz4` [*OPTIONS*] [-|INPUT-FILE] <OUTPUT-FILE>

`unlz4` is equivalent to `lz4 -d`

`lz4cat` is equivalent to `lz4 -dcfm`

When writing scripts that need to decompress files,
it is recommended to always use the name `lz4` with appropriate arguments
(`lz4 -d` or `lz4 -dc`) instead of the names `unlz4` and `lz4cat`.


DESCRIPTION
-----------

`lz4` is a CLI based on `liblz4`, an extremely fast implementation of lossless compression algorithm.
It provides a default compression speed of typically > 500 MB/s per core.
Speed can traded for better compression ratio, by increasing the compression level parameter.
While decompression is single-threaded, it reaches multiple GB/s, generally fast enough to be I/O bound.
`lz4` native file format is the `.lz4` format.

### Difference between lz4 and gzip

`lz4` supports a command line syntax similar _but not identical_ to `gzip(1)`.
Differences are :

  * `lz4` compresses a single file by default (see `-m` for multiple files)
  * `lz4 file1 file2` means : compress file1 _into_ file2
  * `lz4 file.lz4` will default to decompression (use `-z` to force compression)
  * `lz4` preserves original files (see `--rm` to erase source file on completion)
  * `lz4` shows real-time notification statistics
     during compression or decompression of a single file
     (use `-q` to silence them)
  * When no destination is specified, result is sent on implicit output,
    which depends on `stdout` status.
    When `stdout` _is Not the console_, it becomes the implicit output.
    Otherwise, if `stdout` is the console, the implicit output is `filename.lz4`.
  * It is considered bad practice to rely on implicit output in scripts.
    because the script's environment may change.
    Always use explicit output in scripts.
    `-c` ensures that output will be `stdout`.
    Conversely, providing a destination name, or using `-m`
    ensures that the output will be either the specified name, or `filename.lz4` respectively.

Default behaviors can be modified by opt-in commands, detailed below.

  * `lz4 -m` makes it possible to provide multiple input filenames,
    which will be compressed into files using suffix `.lz4`.
    Progress notifications become disabled by default (use `-v` to enable them).
    This mode has a behavior which more closely mimics `gzip` command line,
    with the main remaining difference being that source files are preserved by default.
  * Similarly, `lz4 -m -d` can decompress multiple `*.lz4` files.
  * It's possible to opt-in to erase source files
    on successful compression or decompression, using `--rm` command.
  * Consequently, `lz4 -m --rm` features a behavior closer to the `gzip` one.

### Concatenation of .lz4 files

It is possible to concatenate `.lz4` files as is.
`lz4` will decompress such files as if they were a single `.lz4` file.
For example:

    lz4 file1  > foo.lz4
    lz4 file2 >> foo.lz4

Then `lz4cat foo.lz4` is equivalent to `cat file1 file2`.

OPTIONS
-------

### Short commands concatenation

In some cases, some options can be expressed using short command `-x`
or long command `--long-word`.
Short commands can be concatenated together.
For example, `-d -c` is equivalent to `-dc`.
Long commands cannot be concatenated. They must be clearly separated by a space.

### Multiple commands

When multiple contradictory commands are issued on a same command line,
only the latest one will be applied.

### Operation mode

* `-z` `--compress`:
  Compress.
  This is the default operation mode when no operation mode option is
  specified, no other operation mode is implied from the command name
  (for example, `unlz4` implies `--decompress`),
  nor from the input file name
  (for example, a file extension `.lz4` implies  `--decompress` by default).
  `-z` can also be used to force compression of an already compressed
  `.lz4` file.

* `-d` `--decompress` `--uncompress`:
  Decompress.
  `--decompress` is also the default operation when the input filename has an
  `.lz4` extension.
  `lz4` can also decode Firefox's `mozlz4` files (`.jsonlz4`, `.mozlz4`),
  for example `lz4cat sessionstore.jsonlz4`.

* `-t` `--test`:
  Test the integrity of compressed `.lz4` files.
  The decompressed data is discarded.
  No files are created nor removed.

* `-b#`:
  Benchmark mode, using `#` compression level.

* `--list`:
  List information about .lz4 files.
  For detailed information on files with multiple frames, use `-v`.
  `--list` automatically triggers `-m` modifier.

### Operation modifiers

* `-#`:
  Compression level, with # being any value from 1 to 12.
  Higher values trade compression speed for better compression ratio.
  Values above 12 are considered the same as 12.
  Recommended values are 1 for fast compression (default),
  and 9 for high compression.
  Speed/compression trade-off will vary depending on data to compress.
  Decompression speed remains fast at all settings.

* `--fast[=#]`:
  Switch to ultra-fast compression levels.
  The higher the value, the faster the compression speed, but at the cost of compressed size.
  If `=#` is not present, it defaults to `1`.
  Values above `65536` make no difference.
  This setting overrides compression level if one was set previously.
  Similarly, if a compression level is set after `--fast`, it overrides it.

* `--best`:
  Set highest compression level. Same as -12.

* `-T#`, `--threads=#`:
  Use `#` worker threads for compression.
  The main thread reads and writes files meanwhile.
  When `0`, or none provided: automatically determined from nb of detected cores.
  In both cases, within memory budget (see `--memory`), with a warning when it reduces `#`,
  and no more threads than the jobs of each file (1 MB, or a single block when larger).
  At most 1024 threads (128 on 32-bit systems).
  Ignored with a custom block size (see `-B#`).
  Compressed output doesn't depend on `#`, nor on multithreading support.

* `--single-thread`:
  Compress frames with the main thread alone, without cutting them into jobs (liblz4's single-threaded mode).
  Different from `-T1`, which compresses jobs within one worker thread, while the main thread reads and writes files :
  with linked blocks (`-BD`), compressed output can differ slightly.

* `--memory=#`:
  Memory budget of multi-threaded compression, in bytes, with optional suffixes `K`, `M`, `G` (powers of 1024)
  (default: 4 GB, 1 GB on 32-bit systems). It limits the number of worker threads.
  Each worker employs ~20 MB with 4 MB blocks (default), ~5 MB with smaller blocks.

* `--favor-decSpeed`:
  Generate compressed data optimized for decompression speed.
  Compressed data will be larger as a consequence (typically by ~1%),
  while decompression speed will be improved by 5-20%, depending on use cases.
  This option only works in combination with very high compression levels (>=10).

* `-D dictionaryName`:
  Compress, decompress or benchmark using dictionary _dictionaryName_.
  Compression and decompression must use the same dictionary to be compatible.
  Using a different dictionary during decompression will either
  abort due to decompression error, or generate a checksum error.

* `-f` `--[no-]force`:
  This option has several effects:

  If the target file already exists, overwrite it without prompting.

  When used with `--decompress` and `lz4` cannot recognize the type of
  the source file, copy the source file as is to standard output.
  This allows `lz4cat --force` to be used like `cat (1)` for files
  that have not been compressed with `lz4`.

* `-c` `--stdout` `--to-stdout`:
  Force write to standard output, even if it is the console.

* `-m` `--multiple`:
  Multiple input files.
  Compressed file names will be appended a `.lz4` suffix.
  This mode also reduces notification level.
  Can also be used to list multiple files.
  `lz4 -m` has a behavior equivalent to `gzip -k`
  (it preserves source files by default).

* `-r` :
  operate recursively on directories.
  This mode also sets `-m` (multiple input files).
  Symbolic links to directories found within them are not followed.

* `-B#`:
  Block size \[4-7\](default : 7)<br/>
  `-B4`= 64KB ; `-B5`= 256KB ; `-B6`= 1MB ; `-B7`= 4MB<br/>
  `-B#` with `#` >= 32 : block size in bytes.
  Sizes other than 64KB, 256KB, 1MB and 4MB are compressed single-threaded (`-T#` is ignored).

* `-BI`:
  Produce independent blocks (default)

* `-BD`:
  Blocks depend on predecessors (improves compression ratio, more noticeable on small blocks)

* `-BX`:
  Generate block checksums (default:disabled)

* `--[no-]frame-crc`:
  Select frame checksum (default:enabled)

* `--no-crc`:
  Disable both frame and block checksums

* `--[no-]content-size`:
  Header includes original size (default:not present)<br/>
  Note : this option can only be activated when the original size can be
  determined, hence for a file. It won't work with unknown source size,
  such as stdin or pipe.

* `--[no-]sparse`:
  Sparse mode support (default:enabled on file, disabled on stdout)

* `--[no-]mmap`:
  Memory-map regular files to compress, so that compression threads read them in place
  (default:enabled when supported)<br/>
  Note : a file truncated while being compressed, or an I/O error while reading it,
  then interrupts `lz4` with error 53 (instead of compressing the content read up to that point) ;
  use `--no-mmap` when sources may be truncated or rewritten concurrently.<br/>
  Note : a source file modified while being compressed (size or times changed), or renamed or replaced (ex: log rotation),
  is detected after its compression, and is kept, even with `--rm`. When the output is valid, `lz4` only issues a warning :
  with `--no-mmap`, input is read once, so the output holds the content read (ex: up to the end of a log file growing meanwhile) ;
  memory-mapped input is read several times, so its content is compared with the frame checksum
  (ex: log file growing meanwhile : the output holds the file as it was when opened).
  Otherwise (content rewritten, `--no-frame-crc`, or file shrunk), the output could be inconsistent :
  `lz4` reports error 54 and removes it.

* `-l`:
  Use Legacy format (typically for Linux Kernel compression)<br/>
  Note : `-l` is not compatible with `-m` (`--multiple`) nor `-r`

### Other options

* `-v` `--verbose`:
  Verbose mode

* `-q` `--quiet`:
  Suppress warnings and real-time statistics;
  specify twice to suppress errors too

* `-h` `-H` `--help`:
  Display help/long help and exit

* `-V` `--version`:
  Display Version number and exit.
  The version line also tells if compression is multithreaded (`multithread`) or not (`single-thread`)

* `-k` `--keep`:
  Preserve source files (default behavior)

* `--rm` :
  Delete source files on successful compression or decompression.
  A source file modified, renamed or replaced during its compression is kept (see `--[no-]mmap`).

* `--` :
  Treat all subsequent arguments as files


### Benchmark mode

* `-b#`:
  Benchmark file(s), using # compression level.
  Each file is compressed into a frame, as `lz4` would with the same options
  (`-B#`, `-BD`, `-BX`, `--content-size`, `--favor-decSpeed`, `-D`),
  except the content checksum, disabled unless `--frame-crc`.
  With one compression thread (as `-T1`), unless `-T#` or `--single-thread` is provided.

* `-e#`:
  Benchmark multiple compression levels, from b# to e# (included)

* `-i#`:
  Minimum evaluation time in seconds \[1-9\] (default : 3)


### Environment Variables

It's possible to pass some parameters to `lz4` via environment variables.
This can be useful in situations where `lz4` is known to be invoked (from a script for example) but there is no way to pass `lz4` parameters to influence the compression session.
The environment variable has higher priority than executable default, but lower priority than corresponding runtime command.
When set as global environment variables, it can be a way to enforce personalized defaults different from the executable set ones.

* `LZ4_CLEVEL`:
  specify a default compression level that `lz4` employs for compression when no other compression level is specified on command line. Executable default is generally `1`.

* `LZ4_NBWORKERS`:
  specify a default number of threads that `lz4` will employ for compression. Executable default is generally `0`, which means auto-determined based on local cpu, within memory budget. This functionality is only relevant when `lz4` is compiled with multithreading support.

* `LZ4_MEMORY`:
  specify a memory budget for multi-threaded compression, same as `--memory=#`.


BUGS
----

Report bugs at: https://github.com/lz4/lz4/issues


AUTHOR
------

Yann Collet
