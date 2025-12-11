Programs and scripts for automated testing of LZ4
=======================================================

This directory contains the following programs and scripts:
- `datagen` : Synthetic and parametrable data generator, for tests
- `frametest` : Test tool that checks lz4frame integrity on target platform
- `fullbench`  : Precisely measure speed for each lz4 inner functions
- `fuzzer`  : Test tool, to check lz4 integrity on target platform
- `test-lz4-speed.py` : script for testing lz4 speed difference between commits
- `test-lz4-versions.py` : compatibility test between lz4 versions stored on Github


Test Programs (C Programs)
---------------------------

#### `datagen` - Synthetic Data Generator
Used to generate parameterizable test data. Parameters can control the size of generated data (`-g`), compressibility (`-P`), and random seed (`-s`).

#### `frametest` - LZ4 Frame Format Integrity Test
Used to detect the correctness of the lz4frame API on the target platform. This tool performs extensive random testing to verify frame compression and decompression functionality.

#### `fullbench` - LZ4 Performance Benchmark
Precisely measures the speed of various LZ4 internal functions, including compression and decompression performance at different compression levels.

#### `fuzzer` - LZ4 Fuzzing Tool
Performs integrity testing of the LZ4 compression algorithm using randomly generated data, verifying the correctness of compression and decompression under various boundary conditions.

#### `abiTest` - ABI Stability Test
Ensures that new versions do not break ABI (Application Binary Interface) stability expectations, guaranteeing backward compatibility.

#### `checkFrame` - Frame Header Validation Tool
Validates the correctness of LZ4 frame headers, checking whether block sizes and frame formats comply with specifications.

#### `roundTripTest` - Round Trip Test
Executes LZ4 round trip tests (compression + decompression), compares results with original data, and triggers abort() when data corruption is detected, for use with fuzzing tools like AFL.

#### `decompress-partial` - Partial Decompression Test
Tests the functionality of the `LZ4_decompress_safe_partial` function, verifying the correctness of partial decompression.

#### `decompress-partial-usingDict` - Dictionary-based Partial Decompression Test
Tests partial decompression functionality when using dictionaries, verifying decompression correctness with different dictionary sizes.

#### `freestanding` - Standalone Mode Test
Tests the `LZ4_FREESTANDING` compilation mode, verifying whether LZ4 can work properly in environments that do not depend on the standard library (such as embedded systems).


Python Test Scripts
--------------------

#### `test-lz4-versions.py` - LZ4 Version Compatibility Test Script

This script creates a `versionsTest` directory and clones the lz4 repository, then compiles all tagged (released) lz4 versions, and finally checks interoperability between different versions.

#### `test-lz4-speed.py` - LZ4 Speed Regression Test Script

This script creates a `speedTest` directory and clones the lz4 repository, then compiles all branches and performs speed benchmarks on specified file lists. The script periodically checks the repository for new commits, and if found, compiles new commits and performs benchmarks. If compression or decompression speed falls below the threshold, warning emails are sent.

Additional notes:
- To ensure accurate speed test results, this script should be run on a "stable" system with no other tasks running
- Using it on virtual machines may cause significant fluctuations in speed results
- Speed benchmarks are only performed when the system load average is below `maxLoadAvg` (default 0.75)
- The script uses `mutt` to send emails; if `mutt` is unavailable, it uses `mail` (without attachments); if neither is available, it only prints warnings

Example usage (two test files, one email address, additional message):
```
./test-lz4-speed.py "silesia.tar calgary.tar" "email@gmail.com" --message "tested on my laptop" --sleepTime 60
```

Background execution:
```
nohup ./test-lz4-speed.py testFileNames emails &
```

Complete argument list:
```
positional arguments:
  testFileNames         List of file names for speed benchmarking
  emails                List of email addresses to receive warnings

optional arguments:
  -h, --help            Show help information
  --message MESSAGE     Additional message to append to emails
  --lowerLimit LOWERLIMIT
                        Send email when speed falls below this limit
  --maxLoadAvg MAXLOADAVG
                        Maximum system load average to start testing
  --lastCLevel LASTCLEVEL
                        Highest compression level to test
  --sleepTime SLEEPTIME
                        Interval in seconds to check for repository updates
```

#### `test-lz4-abi.py` - ABI Compatibility Test Script
Tests ABI compatibility between different lz4 versions, ensuring that data compressed by older versions can be correctly decompressed by newer versions, and vice versa.

#### `test-lz4-list.py` - List Function Test Script
Tests the functionality of the `lz4 --list` command, verifying that it can correctly display metadata of compressed files (number of frames, blocks, sizes, etc.).


Shell Test Scripts
------------------

#### `test-lz4-basic.sh` - Basic Functionality Test
Tests the basic functionality of lz4, including:
- Compression/decompression of various data sizes
- `--rm` option (remove source file after compression)
- stdin/stdout handling
- Block checksums (`-BX`)
- Fast compression mode (`--fast`)
- Multi-threaded compression (`-T`)
- Frame CRC options (`--no-frame-crc`)

#### `test-lz4-dict.sh` - Dictionary Compression Test
Tests compression and decompression functionality using external dictionaries (`-D` option), verifies that dictionaries can effectively improve compression ratios, and tests loading of different-sized dictionaries.

#### `test-lz4-sparse.sh` - Sparse File Test
Tests sparse file handling capabilities (`--sparse` / `--no-sparse` options), including:
- Sparse file decompression with different block sizes (B4-B7)
- Compatibility with console and append modes

#### `test-lz4-contentSize.sh` - Content Size Test
Tests the `--content-size` option, verifying correct writing and handling of the content size field in frame headers.

#### `test-lz4-frame-concatenation.sh` - Frame Concatenation Test
Tests correct decompression of concatenated multiple LZ4 frames, including combinations of empty and non-empty frames.

#### `test-lz4-multiple.sh` - Multiple File Processing Test
Tests the multi-file batch compression/decompression functionality of the `-m` option, including:
- Multi-file compression (each file generates a corresponding .lz4 file)
- Multi-file decompression
- Multi-file output to stdout
- `-tm` test mode

#### `test-lz4-multiple-legacy.sh` - Legacy Format Multiple File Test
Tests batch compression and decompression of multiple files using the legacy format (`-l` option).

#### `test-lz4-testmode.sh` - Test Mode Validation
Verifies correct behavior of `-t` (test) mode and pass-through mode, including:
- Decode-only mode for benchmarking (`-bdi0`)
- Error handling for invalid data
- Error handling for non-existent files

#### `test-lz4-skippable.sh` - Skippable Frame Test
Tests LZ4's ability to handle skippable frames, verifying that the decompressor can correctly skip these frames.

#### `test-lz4-opt-parser.sh` - Optimal Parser Test
Tests the optimal parser functionality at high compression levels (-11, -12), using data of different sizes and compressibility for verification.

#### `test-lz4-fast-hugefile.sh` - Large File Fast Compression Test
Tests the ability of fast compression mode to handle very large files (6GB), including decompression with `--content-size` and `--sparse` options.

#### `test-lz4hc-hugefile.sh` - HC Mode Large File Test
Tests the ability of LZ4 HC (High Compression) mode to handle very large files (4.2GB).

#### `test_custom_block_sizes.sh` - Custom Block Size Test
Tests compression functionality with various custom block sizes (32 bytes to 10MB), verifying correct handling of block size boundary conditions.

#### `test_install.sh` - Installation Test
Verifies correct behavior of Makefile's install/uninstall targets under different configuration parameters.

#### `unicode_lint.sh` - Unicode Character Detection
Checks source code files (*.c, *.h) for Unicode characters to ensure code portability.

#### `check_liblz4_version.sh` - Library Version Check
Checks whether the version information of the liblz4 library is correct.

#### `test-lz4wrapper-basic.sh` - Wrapper Library Basic Test
Checks the basic functionality of the lz4wrapper library

#### `test-lz4wrapper-memory.sh` - Wrapper Library Memory Test
Checks for memory leaks in the lz4wrapper library

#### License

All files in this directory are licensed under GPL-v2.
See [COPYING](COPYING) for details.
The text of the license is also included at the top of each source file.
