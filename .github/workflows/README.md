This directory contains [GitHub Actions](https://github.com/features/actions) workflow files.

# Known issues

## USAN, ASAN (`lz4-ubsan-x64`, `lz4-ubsan-x86`, `lz4-asan-x64`)

For now, `lz4-ubsan-*` uses the `-fsanitize-recover=pointer-overflow` flag:
there are known cases of pointer overflow arithmetic within `lz4.c` fast compression.
These cases are not dangerous with current architecture,
but they are not guaranteed to work by the C standard,
which means that, in some future, some new architecture or some new compiler
may decide to do something funny that would break this behavior.
Hence it's proper to remove them.
This has been done in `lz4hc.c`.
However, the same attempt in `lz4.c` resulted in massive speed loss,
which is not acceptable to solve a "potential future" problem that does not exist anywhere today.
Therefore, a better work-around will have to be found.


## C Compilers (`lz4-c-compilers`)

- Our test doesn't use `gcc-4.5` due to installation issue of its package.  (`apt-get install gcc-4.5` fails on GH-Actions VM)

- Currently, the following 32bit executable tests fail with all versions of `clang`.
  - `CC=clang-X CFLAGS='-O3' make V=1 -C tests clean test-lz4c32`
  - `CC=clang-X CFLAGS='-O3 -mx32' make V=1 -C tests clean test-lz4c32`
  - See [#991](https://github.com/lz4/lz4/issues/991) for details.

- Currently, the following 32bit executable tests fail with `gcc-11`
  - `CC=gcc-11 CFLAGS='-O3' make V=1 -C tests clean test-lz4c32`
  - `CC=gcc-11 CFLAGS='-O3 -mx32' make V=1 -C tests clean test-lz4c32`
  - See [#991](https://github.com/lz4/lz4/issues/991) for details.


## cppcheck (`lz4-cppcheck`)

This test script ignores the exit code of `make cppcheck`.
Because this project doesn't 100% follow their recommendation.
Also sometimes it reports false positives.



# Notes

- You can investigate various information at the right pane of GitHub
  Actions report page.

| Item                      | Section in the right pane             |
| ------------------------- | ------------------------------------- |
| OS, VM                    | Set up job                            |
| git repo, commit hash     | Run actions/checkout@v2               |
| Version of tools          | Environment info                      |



# Difference with `.travis.yml`

The following tests are not be included due to limitation of GH-Actions.

- name: aarch64 real-hw tests
- name: PPC64LE real-hw tests
- name: IBM s390x real-hw tests
