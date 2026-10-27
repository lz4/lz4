Programs and scripts for automated testing of LZ4
=======================================================

This directory is organized as follows :
- `unit/` : unit tests, each one autonomous (see `unit/unittest.h` to add tests)
- `fuzz/` : randomized tests of the library (`fuzzer`, `frametest`, `frameMT`)
- `cli/` : tests of the `lz4` command line (`test-lz4-*.sh`, `test-lz4-list.py`)
- `compat/` : ABI, versions, installation, freestanding and build environment checks
- `bench/` : `fullbench` (speed of each lz4 function), `test-lz4-speed.py` (speed difference between commits)
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


#### `bench/test-lz4-speed.py` - script for testing lz4 speed difference between commits

This script creates `speedTest` directory to which lz4 repository is cloned.
Then it compiles all branches of lz4 and performs a speed benchmark for a given list of files (the `testFileNames` parameter).
After `sleepTime` (an optional parameter, default 300 seconds) seconds the script checks repository for new commits.
If a new commit is found it is compiled and a speed benchmark for this commit is performed.
The results of the speed benchmark are compared to the previous results.
If compression or decompression speed for one of lz4 levels is lower than `lowerLimit` (an optional parameter, default 0.98) the speed benchmark is restarted.
If second results are also lower than `lowerLimit` the warning e-mail is sent to recipients from the list (the `emails` parameter).

Additional remarks:
- To be sure that speed results are accurate the script should be run on a "stable" target system with no other jobs running in parallel
- Using the script with virtual machines can lead to large variations of speed results
- The speed benchmark is not performed until computers' load average is lower than `maxLoadAvg` (an optional parameter, default 0.75)
- The script sends e-mails using `mutt`; if `mutt` is not available it sends e-mails without attachments using `mail`; if both are not available it only prints a warning


The example usage with two test files, one e-mail address, and with an additional message:
```
./bench/test-lz4-speed.py "silesia.tar calgary.tar" "email@gmail.com" --message "tested on my laptop" --sleepTime 60
```

To run the script in background please use:
```
nohup ./bench/test-lz4-speed.py testFileNames emails &
```

The full list of parameters:
```
positional arguments:
  testFileNames         file names list for speed benchmark
  emails                list of e-mail addresses to send warnings

optional arguments:
  -h, --help            show this help message and exit
  --message MESSAGE     attach an additional message to e-mail
  --lowerLimit LOWERLIMIT
                        send email if speed is lower than given limit
  --maxLoadAvg MAXLOADAVG
                        maximum load average to start testing
  --lastCLevel LASTCLEVEL
                        last compression level for testing
  --sleepTime SLEEPTIME
                        frequency of repository checking in seconds
```


#### License

All files in this directory are licensed under GPL-v2.
See [COPYING](COPYING) for details.
The text of the license is also included at the top of each source file.
