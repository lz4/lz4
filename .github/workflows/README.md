This directory contains [GitHub Actions](https://github.com/features/actions) workflow files.

# Known issues

## USAN, ASAN (`lz4-ubsan-x64`, `lz4-ubsan-x86`, `lz4-asan-x64`)

For now, `lz4-ubsan-*` uses the `-fsanitize-recover=pointer-overflow` flag:
there are known cases of pointer overflow arithmetic within `lz4.c` fast compression.
These cases are not dangerous with known architecture,
but they are not guaranteed to work by the C standard,
which means that, in some future, some new architecture or some new compiler
may decide to do something funny that could break this behavior.
Hence, in anticipation, it's better to remove them.
This has been already achieved in `lz4hc.c`.
However, the same attempt in `lz4.c` resulted in massive speed losses,
which is not an acceptable cost for preemptively solving a "potential future" problem
not active anywhere today.
Therefore, a more acceptable work-around will have to be found first.



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
