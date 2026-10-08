#!/bin/sh

FPREFIX="tmp-tbs"

set -e

remove () {
    rm -rf $FPREFIX*
}

trap remove EXIT

set -x

# checkFrame -B# -b# : every block, except the last one of each frame,
# must contain exactly -B# bytes, and each frame must declare block size ID -b#

datagen -g12345678    > $FPREFIX-src1   # several 4 MB jobs in multi-threaded mode
datagen -g12345678 -s2 > $FPREFIX-src2
datagen -g4194303     > $FPREFIX-4mb    # just below 4 MB
datagen -g100KB       > $FPREFIX-small  # single job

# invalid block sizes
lz4 -f -B3  $FPREFIX-small $FPREFIX.lz4 && exit 1  # must fail
lz4 -f -B31 $FPREFIX-small $FPREFIX.lz4 && exit 1  # must fail

# custom block sizes, across all block size IDs,
# compared across thread counts, and across concatenated frames
for bs_bsid in "32 4" "2048 4" "65535 4" "65536 4" "65537 5" "262143 5" "262144 5" \
               "262145 6" "1048575 6" "1048576 6" "1048577 7" "4194303 7" "4194304 7"
do
    set -- $bs_bsid
    lz4 -f -T1 -B$1 $FPREFIX-src1 $FPREFIX-T1.lz4
    lz4 -f -T4 -B$1 $FPREFIX-src1 $FPREFIX-T4.lz4
    cmp $FPREFIX-T1.lz4 $FPREFIX-T4.lz4
    lz4 -f -T4 -B$1 $FPREFIX-src2 $FPREFIX-2.lz4
    cat $FPREFIX-T4.lz4 $FPREFIX-2.lz4 > $FPREFIX-cat.lz4
    checkFrame -B$1 -b$2 $FPREFIX-cat.lz4
    lz4 -t $FPREFIX-cat.lz4
done

# block sizes > 4 MB are clamped to 4 MB
lz4 -f -B10485760 $FPREFIX-src1 $FPREFIX.lz4
checkFrame -B4194304 -b7 $FPREFIX.lz4

# input size just below the 4 MB job size
lz4 -f -T4 -B65535 $FPREFIX-4mb $FPREFIX.lz4
checkFrame -B65535 -b4 $FPREFIX.lz4
lz4 -t $FPREFIX.lz4

# input smaller than a single job (#1810)
lz4 -f -T4 -B2048 $FPREFIX-small $FPREFIX.lz4
checkFrame -B2048 -b4 $FPREFIX.lz4
lz4 -t $FPREFIX.lz4

# stdin (no content size)
lz4 -T4 -B2048 < $FPREFIX-src1 > $FPREFIX.lz4
checkFrame -B2048 -b4 $FPREFIX.lz4
lz4 -t $FPREFIX.lz4

# linked blocks, fast and HC
for clevel in 1 9
do
    lz4 -f -T1 -$clevel -BD -B100000 $FPREFIX-src1 $FPREFIX-T1.lz4
    lz4 -f -T4 -$clevel -BD -B100000 $FPREFIX-src1 $FPREFIX-T4.lz4
    cmp $FPREFIX-T1.lz4 $FPREFIX-T4.lz4
    checkFrame -B100000 -b5 $FPREFIX-T4.lz4
    lz4 -t $FPREFIX-T4.lz4
done

# block checksum, no content checksum
lz4 -f -T4 -BX --no-frame-crc -B2048 $FPREFIX-src1 $FPREFIX.lz4
checkFrame -B2048 -b4 $FPREFIX.lz4
lz4 -dc $FPREFIX.lz4 | cmp - $FPREFIX-src1
