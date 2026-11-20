Programs and scripts for automated testing of LZ4
=======================================================

This directory is organized as follows :
- `unit/` : unit tests, each one autonomous (see `unit/unittest.h` to add tests)
- `fuzz/` : randomized tests of the library (`fuzzer`, `frametest`, `frameMT`)
- `cli/` : tests of the `lz4` command line (`test-lz4-*.sh`)
- `compat/` : ABI, versions, installation, freestanding and build environment checks
- `bench/` : `fullbench` (speed of each lz4 function)
- `tools/` : `datagen` (synthetic data generator), `checkFrame` (frame properties checker)
- `data/` : sample files

Test programs are built in this directory, by `make <program>`, and run by `make test-<name>` (see `Makefile`).


#### Running tests

- `make test-unittest` : unit tests, built twice : `unittest`, and `unittest-mt` with multi-threading support.
  `./unittest -l` lists them, `./unittest <filter>` runs the tests whose name contains `<filter>`,
  `-s#` changes the seed of their data, `-r` selects a random one, reported at the end.
  `make test-unittest` uses a random seed.
- `make test-lz4-essentials` : main tests of the `lz4` command line
- `make test` : all tests (long)


#### Adding tests

Unit tests of the library go to `unit/`, in the file of the corresponding library source
(`unittest_lz4.c`, `unittest_lz4hc.c`, `unittest_lz4frame.c`, `unittest_lz4frameMT.c` for multi-threaded compression) :
```c
TEST(getBlockSize_64KB)
{
    ASSERT_EQ(LZ4F_getBlockSize(LZ4F_max64KB), 64 KB);
}
```
Tests register themselves, and run in the order of the source.
Each test runs in its own context : buffers from `UT_malloc()` and `UT_sample()` are released at the end of the test,
even when it fails, and a failed assertion only ends this test.
See `unit/unittest.h` for assertions and helpers, and `unit/lz4frame_helpers.h` for lz4frame.

Tests of the command line go to `cli/` : extend an existing script, or create `cli/test-lz4-<name>.sh`,
with a `test-lz4-<name>` target in `Makefile` and an entry in `build/meson/meson/tests/meson.build`.
Scripts run from this directory, and find `lz4` and `datagen` in `PATH`.
Their temporary files start with a prefix unique to the script, itself starting with `tmp`,
and are removed on success.


#### `compat/test-lz4-versions.py` - script for testing lz4 interoperability between versions

This script creates `versionsTest` directory to which lz4 repository is cloned.
Then all tagged (released) versions of lz4 are compiled.
In the following step interoperability between lz4 versions is checked.


#### License

All files in this directory are licensed under GPL-2.0-or-later, unless a file states otherwise.
See [COPYING](data/COPYING) for details.
The text of the license is also included at the top of each source file.
