LZ4 - Extremely fast compression
================================

LZ4 is lossless compression algorithm, 
providing compression speed at 400 MB/s per core, 
scalable with multi-cores CPU. 
It also features an extremely fast decoder, 
with speed in multiple GB/s per core, 
typically reaching RAM speed limits on multi-core systems.

A high compression derivative, called LZ4_HC, is also provided. 
It trades CPU time for compression ratio.

|Branch      |Status   |
|------------|---------|
|master      | [![Build Status](https://travis-ci.org/Cyan4973/lz4.svg?branch=master)](https://travis-ci.org/Cyan4973/lz4) |
|dev         | [![Build Status](https://travis-ci.org/Cyan4973/lz4.svg?branch=dev)](https://travis-ci.org/Cyan4973/lz4) |
|visual      | [![Build status](https://ci.appveyor.com/api/projects/status/v6kxv9si529477cq?svg=true)](https://ci.appveyor.com/project/YannCollet/lz4) |



> **Branch Policy:**

> - The "master" branch is considered stable, at all times.
> - The "dev" branch is the one where all contributions must be merged 
    before being promoted to master.
>   + If you plan to propose a patch, please commit into the "dev" branch. 
      Direct commit to "master" are not permitted.
> - Feature branches can also exist,
    for dedicated tests of larger modifications before merge into "dev" branch.

Benchmarks
-------------------------

The benchmark uses the [Open-Source Benchmark program by m^2 (v0.14.3)]
compiled with GCC v4.8.2 on Linux Mint 64-bits v17.
The reference system uses a Core i5-4300U @1.9GHz.
Benchmark evaluates the compression of reference [Silesia Corpus]
in single-thread mode.

|  Compressor       | Ratio   | Compression | Decompression |
|  ----------       | -----   | ----------- | ------------- |
|  memcpy           |  1.000  | 4200 MB/s   |   4200 MB/s   |
|  RLE64 v3.0       |  1.029  | 2800 MB/s   |   2800 MB/s   |
|  density -c1      |  1.592  |  700 MB/s   |    920 MB/s   |
|**LZ4 fast (r129)**|  1.607  |**680 MB/s** | **2220 MB/s** |
|**LZ4 (r129)**     |**2.101**|**385 MB/s** | **1850 MB/s** |
|  density -c2      |  2.083  |  370 MB/s   |    505 MB/s   |
|  LZO 2.06         |  2.108  |  350 MB/s   |    510 MB/s   |
|  QuickLZ 1.5.1.b6 |  2.238  |  320 MB/s   |    380 MB/s   |
|  Snappy 1.1.0     |  2.091  |  250 MB/s   |    960 MB/s   |
|  density -c3      |  2.370  |  190 MB/s   |    185 MB/s   |
|  zlib 1.2.8 -1    |  2.730  |   59 MB/s   |    250 MB/s   |
|**LZ4 HC (r129)**  |**2.720**|   22 MB/s   | **1830 MB/s** |
|  zlib 1.2.8 -6    |  3.099  |   18 MB/s   |    270 MB/s   |

The LZ4 block compression format is detailed within [lz4_Block_format].

Block format doesn't deal with header information, 
nor how to handle arbitrarily long files or data streams.
This is the purpose of the Frame format.
Interoperable versions of LZ4 should use the same frame format, 
defined into [lz4_Frame_format].

[Open-Source Benchmark program by m^2 (v0.14.3)]: http://encode.ru/threads/1371-Filesystem-benchmark?p=34029&viewfull=1#post34029
[Silesia Corpus]: http://sun.aei.polsl.pl/~sdeor/index.php?page=silesia
[lz4_Block_format]: lz4_Block_format.md
[lz4_Frame_format]: lz4_Frame_format.md
