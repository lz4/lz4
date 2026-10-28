LZ4 Windows binary package
==========================

This package contains `lz4.exe`, the LZ4 command line utility for Windows.
It is standalone : no DLL is needed.


#### Contents

- `lz4.exe`     : the command line utility
- `README.md`   : this file
- `NEWS`        : changes in each version
- `COPYING`     : license of `lz4.exe` (GPL-2.0-or-later)
- `LICENSE-lib` : license of the LZ4 library included in `lz4.exe` (BSD 2-Clause)


#### Usage

    lz4 file          compresses file into file.lz4
    lz4 -d file.lz4   decompresses file.lz4 into file

Source files are kept.
Compression levels range from `-1` (fastest, default) to `-12` (slowest, strongest).
`lz4 -h` lists the main options, `lz4 -H` all of them,
and `lz4 -b` benchmarks a file in memory.
Full manual : https://github.com/lz4/lz4/blob/release/programs/lz4.1.md


#### Library

This package doesn't contain the LZ4 library for development.
To use it in a program, build it from the sources (https://github.com/lz4/lz4)
with the provided CMake, Meson, Visual Studio or make builds,
or get it from a package manager, such as vcpkg or conan.
