LZ4 - Extremely fast compression
================================

LZ4 is lossless compression algorithm, providing compression speed at 400 MB/s per core, scalable with multi-cores CPU. It also features an extremely fast decoder, with speed in multiple GB/s per core, typically reaching RAM speed limits on multi-core systems.
A high compression derivative, called LZ4_HC, is also provided. It trades CPU time for compression ratio.

|Branch      |Status   |
|------------|---------|
|master      | [![Build Status](https://travis-ci.org/Cyan4973/lz4.svg?branch=master)](https://travis-ci.org/Cyan4973/lz4) |
|dev         | [![Build Status](https://travis-ci.org/Cyan4973/lz4.svg?branch=dev)](https://travis-ci.org/Cyan4973/lz4) |


> **Branch Policy:**

> - The "master" branch is considered stable, at all times.
> - The "dev" branch is the one where all contributions must be merged before being promoted to master.
>  - If you plan to propose a patch, please commit into the "dev" branch. Direct commit to "master" are not permitted.
> - Feature branches can also exist, for dedicated testing of larger modifications before merge into "dev" branch.

Benchmarks
-------------------------

The benchmark uses the [Open-Source Benchmark program by m^2 (v0.14.3)](http://encode.ru/threads/1371-Filesystem-benchmark?p=33548&viewfull=1#post33548) compiled with GCC v4.8.2 on Linux Mint 64-bits v17.
The reference system uses a Core i5-4300U @1.9GHz.
Benchmark evaluates the compression of reference [Silesia Corpus](http://sun.aei.polsl.pl/~sdeor/index.php?page=silesia) in single-thread mode.

|  Compressor       | Ratio   | Compression | Decompression |
|  ----------       | -----   | ----------- | ------------- |
|**LZ4 (r129)**     |  2.101  |**385 MB/s** |**1850 MB/s**  |
|  LZO 2.06         |  2.108  |  350 MB/s   |   510 MB/s    |
|  QuickLZ 1.5.1.b6 |  2.238  |  320 MB/s   |   380 MB/s    |
|  Snappy 1.1.0     |  2.091  |  250 MB/s   |   960 MB/s    |
|  zlib 1.2.8 -1    |  2.730  |   59 MB/s   |   250 MB/s    |
|**LZ4 HC (r129)**  |**2.720**|   22 MB/s   |**1830 MB/s**  |
|  zlib 1.2.8 -6    |  3.099  |   18 MB/s   |   270 MB/s    |

The LZ4 block compression format is detailed within [lz4_Block_format](lz4_Block_format.md).

For streaming unknown amount of data and compress files of any size, a frame format has been published, and can be consulted within the file LZ4_Frame_Format.html .

