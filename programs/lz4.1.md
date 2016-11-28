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

`lz4` is an extremely fast lossless compression algorithm,
based on **byte-aligned LZ77** family of compression scheme.
`lz4` offers compression speeds of 400 MB/s per core, linearly scalable with
multi-core CPUs.
It features an extremely fast decoder, with speed in multiple GB/s per core,
typically reaching RAM speed limit on multi-core systems.
The native file format is the `.lz4` format.

### Difference between lz4 and gzip

`lz4` supports a command line syntax similar
_but not identical_ to `gzip(1)`.
Differences are :

  * `lz4` preserves original files
  * `lz4` compresses a single file by default (use `-m` for multiple files)
  * `lz4 file1 file2` means : compress file1 _into_ file2
  * When no destination name is provided, compressed file name receives
    a `.lz4` suffix
  * When no destination name is provided, if `stdout` is _not_ the console,
    it becomes the output (like a silent `-c`).
    Therefore `lz4 file > /dev/null` will not create `file.lz4`
  * `lz4 file` shows real-time statistics during compression
    (use `-q` to silent them)

Default behaviors can be modified by opt-in commands, described below.
`lz4 --quiet --multiple` more closely mimics `gzip` behavior.

### Concatenation of .lz4 files

It is possible to concatenate `.lz4` files as is.
`lz4` will decompress such files as if they were a single `.lz4` file.
For example:
    lz4 file1  > foo.lz4
    lz4 file2 >> foo.lz4

then
    lz4cat foo.lz4

is equivalent to :
    cat file1 file2


OPTIONS
-------

### Short commands concatenation

In some cases, some options can be expressed using short command `-x`
or long command `--long-word`.
Short commands can be concatenated together.
For example, `-d -c` is equivalent to `-dc`.
Long commands cannot be concatenated.
They must be clearly separated by a space.

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

* `-t` `--test`:
  Test the integrity of compressed `.lz4` files.
  The decompressed data is discarded.
  No files are created nor removed.

* `-b#`:
  Benchmark mode, using `#` compression level.

### Operation modifiers

* `-#`:
  Compression level, with # being any value from 1 to 16.
  Higher values trade compression speed for compression ratio.
  Values above 16 are considered the same as 16.
  Recommended values are 1 for fast compression (default),
  and 9 for high compression.
  Speed/compression trade-off will vary depending on data to compress.
  Decompression speed remains fast at all settings.

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
  Multiple file names.
  By default, the second filename is used as the destination filename
  for the compressed file.
  With `-m`, it is possible to specify any number of input filenames.
  Each of them will be compressed independently, and the resulting name of
  each compressed file will be `filename.lz4`.

* `-B#`:
  Block size \[4-7\](default : 7)<br/>
  `-B4`= 64KB ; `-B5`= 256KB ; `-B6`= 1MB ; `-B7`= 4MB

* `-BD`:
  Block dependency (improves compression ratio on small blocks)

* `--[no-]frame-crc`:
  Select frame checksum (default:enabled)

* `--[no-]content-size`:
  Header includes original size (default:not present)<br/>
  Note : this option can only be activated when the original size can be
  determined, hence for a file. It won't work with unknown source size,
  such as stdin or pipe.

* `--[no-]sparse`:
  Sparse mode support (default:enabled on file, disabled on stdout)

* `-l`:
  Use Legacy format (typically used for Linux Kernel compression)<br/>
  Note : `-l` is not compatible with `-m` (`--multiple`)

### Other options

* `-v` `--verbose`:
  Verbose mode

* `-q` `--quiet`:
  Suppress warnings and real-time statistics; specify twice to suppress
  errors too

* `-h` `-H` `--help`:
  Display help/long help and exit

* `-V` `--version`:
  Display Version number and exit

* `-k` `--keep`:
  Don't delete source file.
  This is default behavior anyway, so this option is just for compatibility
  with `gzip(1)` / `xz(1)`.


### Benchmark mode

* `-b#`:
  Benchmark file(s), using # compression level

* `-e#`:
  Benchmark multiple compression levels, from b# to e# (included)

* `-i#`:
  Minimum evaluation in seconds \[1-9\] (default : 3)

* `-r`:
  Operate recursively on directories


BUGS
----

Report bugs at: https://github.com/lz4/lz4/issues


AUTHOR
------

Yann Collet
