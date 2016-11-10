LZ4 - Library Files
================================

The directory contains many files, but depending on project's objectives,
not all of them are necessary.

#### Minimal LZ4 build

The minimum required is **`lz4.c`** and **`lz4.h`**,
which will provide the fast compression and decompression algorithm.


#### The High Compression variant od LZ4

For more compression at the cost of compression speed,
the High Compression variant **lz4hc** is available.
It's necessary to add **`lz4hc.c`** and **`lz4hc.h`**.
The variant still depends on regular `lz4` source files.
In particular, the decompression is still provided by `lz4.c`.


#### Compatibility issues

In order to produce files or streams compatible with `lz4` command line utility,
it's necessary to encode lz4-compressed blocks using the [official interoperable frame format].
This format is generated and decoded automatically by the **lz4frame** library.
In order to work properly, lz4frame needs lz4 and lz4hc, and also **xxhash**,
which provides error detection.
(_Advanced stuff_ : It's possible to hide xxhash symbols into a local namespace.
This is what `liblz4` does, to avoid symbol duplication
in case a user program would link to several libraries containing xxhash symbols.)


#### Advanced API 

A more complex `lz4frame_static.h` is also provided.
It contains definitions which are not guaranteed to remain stable within future versions.
It must be used with static linking ***only***.


#### Using MinGW+MSYS to create DLL

DLL can be created using MinGW+MSYS with the "make liblz4" command.
This command creates "liblz4.dll" and the import library "liblz4.dll.a".
To compile a project the import library has to be added to linking options.
It means that if a project that uses LZ4 consists of a single test-dll.c 
file it should be compiled with "liblz4.dll.a". For example:
```
    gcc $CFLAGS test-dll.c -o test-dll liblz4.dll.a
```


#### Miscellaneous 

Other files present in the directory are not source code. There are :

 - LICENSE : contains the BSD license text
 - Makefile : script to compile or install lz4 library (static or dynamic)
 - liblz4.pc.in : for pkg-config (make install)
 - README.md : this file

[official interoperable frame format]: ../doc/lz4_Frame_format.md


#### License 

All source material within __lib__ directory are BSD 2-Clause licensed.
See [LICENSE](LICENSE) for details.
The license is also repeated at the top of each source file.
