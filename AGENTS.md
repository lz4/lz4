# Notes for coding agents

LZ4 is a lossless compression library (`lib/`, BSD-2) with a command line tool (`programs/`, GPLv2+).
Pull requests target the `dev` branch.

## Layout
- `lib/` : the library : `lz4.c` (block format, fast levels), `lz4hc.c` (high compression levels),
  `lz4frame.c` (frame format), `lz4file.c` (file API), `xxhash.c`
- `programs/` : the `lz4` command line tool
- `tests/` : tests, organized by kind (see `tests/README.md`)
- `doc/` : format specifications and API manuals
- `build/` : cmake, meson and Visual Studio builds ; the reference build is `make`
- `examples/`, `contrib/`, `ossfuzz/`

## Build and test
- `make` : library and `lz4`
- `make -C tests test-unittest` : unit tests (fast)
- `make -C tests test-lz4-essentials` : main tests of the `lz4` command line
- `make -C tests test-fuzzer`, `make -C tests test-frametest` : randomized tests (90 s each)
- `make test` : all tests (long)
- sanitizers : `make usan`, or `CFLAGS=-fsanitize=address LDFLAGS=-fsanitize=address make -C tests test-unittest`

## Adding tests
- Library behavior : add a `TEST()` to `tests/unit/unittest_<file>.c` (`lz4`, `lz4hc`, `lz4frame`) :
  it registers itself (see `tests/unit/unittest.h`).
- Do not add tests to `FUZ_unitTests()` (`tests/fuzz/fuzzer.c`) : its tests are being moved to `tests/unit/`.
- Command line behavior : extend a script of `tests/cli/`, or add `tests/cli/test-lz4-<name>.sh`,
  with its target in `tests/Makefile` and its entry in `build/meson/meson/tests/meson.build`.

## Rules
- `lib/` and `programs/` are C90 (`/* */` comments, declarations at the top of blocks), with `long long`,
  and must also compile as C++ : see `CODING_STYLE`. Tests follow the same declaration rule.
- CI builds with `-Werror` (C90 to C11, C++, 32-bit), and runs tests under sanitizers.
- Speed matters : changes to compression or decompression code must be measured
  (`lz4 -b`, `tests/fullbench`), not estimated.
- User-visible changes (fixes, API, command line, performance) get a line in `NEWS`, in the same pull request.
