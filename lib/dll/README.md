The static and dynamic LZ4 libraries
====================================

#### The package content

- `dll\liblz4.dll` : The DLL of LZ4 library
- `dll\liblz4.lib` : The import library of LZ4 library
- `include\` :       Header files required with LZ4 library
- `fullbench\` :     The example of usage of LZ4 library
- `static\liblz4_static.lib` : The static LZ4 library


#### The example of usage of static and dynamic LZ4 libraries with gcc/MinGW

Use `make` to build `fullbench-dll` and `fullbench-lib`.
`fullbench-dll` uses a dynamic LZ4 library from the `dll` directory.
`fullbench-lib` uses a static LZ4 library from the `lib` directory.


#### Using LZ4 DLL with gcc/MinGW

The header files from `include\` and the import library `dll\liblz4.lib`
are required to compile a project using gcc/MinGW.
The import library has to be added to linking options.
It means that if a project that uses LZ4 consists of a single `test-dll.c`
file it should be compiled with "liblz4.lib". For example:
```
    gcc $(CFLAGS) -Iinclude/ test-dll.c -o test-dll dll\liblz4.lib
```
The compiled executable will require LZ4 DLL which is available at `dll\liblz4.dll`.


#### The example of usage of static and dynamic LZ4 libraries with Visual C++

Open `fullbench\fullbench-dll.sln` to compile `fullbench-dll` that uses a
dynamic LZ4 library from the `dll` directory. The solution works with Visual C++
2010 or newer. When one will open the solution with Visual C++ newer than 2010
then the solution will upgraded to the current version.


#### Using LZ4 DLL with Visual C++

The header files from `include\` and the import library `dll\liblz4.lib`
are required to compile a project using Visual C++.

1. The header files should be added to `Additional Include Directories` that can
   be found in project properties `C/C++` then `General`.
2. The import library has to be added to `Additional Dependencies` that can
   be found in project properties `Linker` then `Input`.
   If one will provide only the name `liblz4.lib` without a full path to the library
   the directory has to be added to `Linker\General\Additional Library Directories`.

The compiled executable will require LZ4 DLL which is available at `dll\liblz4.dll`.
