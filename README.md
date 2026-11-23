LZ4 - Extremely fast compression
================================

LZ4 is a lossless compression algorithm,
providing compression speed > 500 MB/s per core,
scalable with multi-core CPUs.
It features an extremely fast decoder,
with speed in multiple GB/s per core,
typically reaching RAM speed limits on multi-core systems.

Compression speed can be tuned dynamically, by selecting an "acceleration" factor
which trades compression ratio for faster speed.
On the other end, a high compression derivative, LZ4_HC, is also provided,
trading CPU time for improved compression ratio.
All versions feature the same decompression speed.

LZ4 is also compatible with [dictionary compression](https://github.com/facebook/zstd#the-case-for-small-data-compression),
both at [API](lib/lz4frame.h) and [CLI](programs/lz4.1.md#operation-modifiers) (`-D`) levels.
It can ingest any input file as dictionary, though only the final 64 KB are used.
This capability can be combined with the [Zstandard Dictionary Builder](https://github.com/facebook/zstd/blob/dev/programs/zstd.1.md#dictionary-builder),
in order to drastically improve compression performance on small files.

The LZ4 library is provided as open-source software using the BSD 2-Clause license.
The `lz4` command line utility is provided under the GPL-2.0-or-later license.


Benchmarks
-------------------------

The benchmark uses [lzbench] v2.4.2, from @inikep
compiled with GCC v13.3.0 on Linux 64-bits (Ubuntu 24.04).
The reference system uses a Core i7-9700K CPU.
Benchmark evaluates the compression of reference [Silesia Corpus]
in single-thread mode.

[lzbench]: https://github.com/inikep/lzbench
[Silesia Corpus]: https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia

| Compressor                |    Factor |  Compression | Decompression |
| ----------                | --------- | ------------ | ------------- |
| memcpy                    |     1.000 |   17100 MB/s |    17500 MB/s |
| **LZ4 default (v1.11.1)** | **2.113** | **780 MB/s** | **5060 MB/s** |
| LZO 2.10 -1               |     2.108 |     603 MB/s |      808 MB/s |
| QuickLZ 1.5.1b7 -1        |     2.238 |     599 MB/s |      710 MB/s |
| Snappy 1.3.1              |     2.090 |     529 MB/s |     1370 MB/s |
| [Zstandard] 1.5.7 -1      |     2.896 |     512 MB/s |     1570 MB/s |
| LZF v3.6                  |     2.077 |     431 MB/s |      781 MB/s |
| zlib-ng 2.3.3 -1          |     2.252 |     249 MB/s |      577 MB/s |
| [zlib] deflate 1.4 -1     |     2.743 |     108 MB/s |      385 MB/s |
| **LZ4 HC -9 (v1.11.1)**   | **2.721** |      42 MB/s | **4970 MB/s** |
| [zlib] deflate 1.4 -6     |     3.107 |      36 MB/s |      413 MB/s |

All levels of these compressors, and the `lz4` command line in pipelines, are charted on [lz4.org](https://lz4.org/#benchmarks).

[zlib]: https://www.zlib.net/
[Zstandard]: https://facebook.github.io/zstd/


Installation
-------------------------

```
make
make install     # this command may require root permissions
```

LZ4's `Makefile` supports standard [Makefile conventions],
including [staged installs], [redirection], or [command redefinition].
It is compatible with parallel builds (`-j#`).

[Makefile conventions]: https://www.gnu.org/prep/standards/html_node/Makefile-Conventions.html
[staged installs]: https://www.gnu.org/prep/standards/html_node/DESTDIR.html
[redirection]: https://www.gnu.org/prep/standards/html_node/Directory-Variables.html
[command redefinition]: https://www.gnu.org/prep/standards/html_node/Utilities-in-Makefiles.html

Alternative build systems (CMake, Meson, Visual Studio) are provided in [build/](build/).

### Building LZ4 - Using vcpkg

You can download and install LZ4 using the [vcpkg](https://github.com/Microsoft/vcpkg) dependency manager,
once vcpkg itself is installed (see its documentation):

    vcpkg install lz4

The LZ4 port in vcpkg is kept up to date by Microsoft team members and community contributors. If the version is out of date, please [create an issue or pull request](https://github.com/Microsoft/vcpkg) on the vcpkg repository.

Documentation
-------------------------

The raw LZ4 block compression format is detailed within [lz4_Block_format].

Arbitrarily long files or data streams are compressed using multiple blocks,
for streaming requirements. These blocks are organized into a frame,
defined in [lz4_Frame_format].
Interoperable versions of LZ4 must also respect the frame format.

The library API is documented in its headers:
[lz4.h] (block API), [lz4hc.h] (high compression) and [lz4frame.h] (frame API).

[lz4_Block_format]: doc/lz4_Block_format.md
[lz4_Frame_format]: doc/lz4_Frame_format.md
[lz4.h]: lib/lz4.h
[lz4hc.h]: lib/lz4hc.h
[lz4frame.h]: lib/lz4frame.h


Other source versions
-------------------------

Beyond the C reference source,
many contributors have created versions of lz4 in multiple languages
(Java, C#, Python, Perl, Ruby, etc.).
A list of known source ports is maintained on the [LZ4 Homepage].

[LZ4 Homepage]: https://lz4.org

### Packaging status

Most distributions are bundled with a package manager
which allows easy installation of both the `liblz4` library
and the `lz4` command line interface.

[![Packaging status](https://repology.org/badge/vertical-allrepos/lz4.svg?columns=4&exclude_unsupported=1)](https://repology.org/project/lz4/versions)


### Special Thanks

- Takayuki Matsuoka, aka @t-mat, for exceptional first-class support throughout the lifetime of this project
