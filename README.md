LZ4 - Extremely fast compression
================================

LZ4 is lossless compression algorithm, providing compression speed at 400 MB/s per core, scalable with multi-cores CPU. It also features an extremely fast decoder, with speed in multiple GB/s per core, typically reaching RAM speed limits on multi-core systems.
A high compression derivative, called LZ4_HC, is also provided. It trades CPU time for compression ratio.

|Branch      |Status   |
|------------|---------|
|master      | [![Build Status](https://travis-ci.org/Cyan4973/lz4.svg?branch=master)](https://travis-ci.org/Cyan4973/lz4) |
|dev         | [![Build Status](https://travis-ci.org/Cyan4973/lz4.svg?branch=dev)](https://travis-ci.org/Cyan4973/lz4) |

This is an official mirror of LZ4 project, [hosted on Google Code](http://code.google.com/p/lz4/).
The intention is to offer github's capabilities to lz4 users, such as cloning, branch, or source download.

The "master" branch will reflect, the status of lz4 at its official homepage. Other branches will also exist, typically to fix some open issues or new requirements, and be available for testing before merge into master.


Benchmarks
-------------------------

The benchmark uses the [Open-Source Benchmark program by m^2 (v0.14.2)](http://encode.ru/threads/1371-Filesystem-benchmark?p=33548&viewfull=1#post33548) compiled with GCC v4.6.1 on Linux Ubuntu 64-bits v11.10,
The reference system uses a Core i5-3340M @2.7GHz.
Benchmark evaluates the compression of reference [Silesia Corpus](http://sun.aei.polsl.pl/~sdeor/index.php?page=silesia) in single-thread mode.

<table>
  <tr>
    <th>Compressor</th><th>Ratio</th><th>Compression</th><th>Decompression</th>
  </tr>
  <tr>
    <th>LZ4 (r101)</th><th>2.084</th><th>422 MB/s</th><th>1820 MB/s</th>
  </tr>
  <tr>
    <th>LZO 2.06</th><th>2.106</th><th>414 MB/s</th><th>600 MB/s</th>
  </tr>
  <tr>
    <th>QuickLZ 1.5.1b6</th><th>2.237</th><th>373 MB/s</th><th>420 MB/s</th>
  </tr>
  <tr>
    <th>Snappy 1.1.0</th><th>2.091</th><th>323 MB/s</th><th>1070 MB/s</th>
  </tr>
  <tr>
    <th>LZF</th><th>2.077</th><th>270 MB/s</th><th>570 MB/s</th>
  </tr>
  <tr>
    <th>zlib 1.2.8 -1</th><th>2.730</th><th>65 MB/s</th><th>280 MB/s</th>
  </tr>
  <tr>
    <th>LZ4 HC (r101)</th><th>2.720</th><th>25 MB/s</th><th>2080 MB/s</th>
  </tr>
  <tr>
    <th>zlib 1.2.8 -6</th><th>3.099</th><th>21 MB/s</th><th>300 MB/s</th>
  </tr>
</table>

