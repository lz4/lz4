# lz4 for DOS/djgpp
This file details on how to compile lz4.exe, and liblz4.a for use on DOS/djgpp using
Andrew Wu's build-djgpp cross compilers ([GH][0], [Binaries][1]) on OSX, Linux.

## Setup
* Download a djgpp tarball [Binaries][1] for your platform.  
* Extract and install it (`tar jxvf djgpp-linux64-gcc492.tar.bz2`).  Note the path.  We'll assume `/home/user/djgpp`.
* Add the `bin` folder to your `PATH`.  In bash, do `export PATH=/home/user/djgpp/bin:$PATH`.

## Building LZ4 binary for DOS
* Run `make` substituting the cross compiler for `CC` and `AR`, e.g., `CC=i586-pc-msdosdjgpp-gcc AR=i586-pc-msdosdjgpp-ar make lz4programs`
* `lz4` and `lz4.exe` are then in the `programs/` subfolder.

## Building LZ4 library for DOS
* Run `make` substituting the cross compiler for `CC` and `AR`, e.g., `CC=i586-pc-msdosdjgpp-gcc AR=i586-pc-msdosdjgpp-ar make lz4lib-djgpp`
* `liblz4.z`, `lz4.h`, `lz4hc.h`, `lz4frame.h`, and `xxhash.h` are then in the `lib/` subfolder.

[0]: https://github.com/andrewwutw/build-djgpp
[1]: https://github.com/andrewwutw/build-djgpp/releases
