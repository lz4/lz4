Programs and scripts for automated testing of LZ4
=======================================================

This directory contains the following programs and scripts:
- `datagen` : Synthetic and parametrable data generator, for tests
- `frametest` : Test tool that checks lz4frame integrity on target platform
- `fullbench`  : Precisely measure speed for each lz4 inner functions
- `fuzzer`  : Test tool, to check lz4 integrity on target platform
- `test-lz4-versions.py` : compatibility test between lz4 versions stored on Github


#### `test-lz4-versions.py` - script for testing lz4 interoperability between versions

This script creates `versionsTest` directory to which lz4 repository is cloned.
Then all taged (released) versions of lz4 are compiled.
In the following step interoperability between lz4 versions is checked.


#### License

All files in this directory are licensed under GPL-v2.
See [COPYING](COPYING) for details.
The text of the license is also included at the top of each source file.
