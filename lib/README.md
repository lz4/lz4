LZ4 - Library Files
================================

This directory contains many files, but you don't necessarily need them all.

To integrate LZ4 compression/decompression into your program, you basically just need to include "**lz4.c**" and "**lz4.h**".

For more compression at the cost of compression speed (while preserving decompression speed), use **lz4hc**. Compile "**lz4hc.c**" and include "**lz4hc.h**". Note that lz4hc needs lz4 to compile properly.

Next level, if you want to produce files or data streams compatible with lz4 utility, use and include "**lz4frame.c**" and **lz4frame.h**". This library encapsulate lz4-compressed blocks into the [official interoperable frame format]. In order to work properly, lz4frame needs lz4 and lz4hc, and also "**xxhash.c**" and "**xxhash.h**", which provide error detection algorithm.

A more complex "lz4frame_static.h" is also provided, although its usage is not recommended. It contains definitions which are not guaranteed to remain stable within future versions. Use for static linking ***only***.

The other files are not source code. There are :

 - LICENSE : contains the BSD license text
 - Makefile : script to compile or install lz4 library (static or dynamic)
 - liblz4.pc.in : for pkg-config (make install)

[official interoperable frame format]: ../lz4_Frame_format.md
