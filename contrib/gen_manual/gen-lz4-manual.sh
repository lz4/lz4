#!/bin/sh

LIBVER_MAJOR_SCRIPT=`sed -n '/define LZ4_VERSION_MAJOR/s/.*[[:blank:]]\([0-9][0-9]*\).*/\1/p' < ../../lib/lz4.h`
LIBVER_MINOR_SCRIPT=`sed -n '/define LZ4_VERSION_MINOR/s/.*[[:blank:]]\([0-9][0-9]*\).*/\1/p' < ../../lib/lz4.h`
LIBVER_PATCH_SCRIPT=`sed -n '/define LZ4_VERSION_RELEASE/s/.*[[:blank:]]\([0-9][0-9]*\).*/\1/p' < ../../lib/lz4.h`
LIBVER_SCRIPT=$LIBVER_MAJOR_SCRIPT.$LIBVER_MINOR_SCRIPT.$LIBVER_PATCH_SCRIPT

echo LZ4_VERSION=$LIBVER_SCRIPT
./gen_manual $LIBVER_SCRIPT ../../lib/lz4.h ./lz4_manual.html
