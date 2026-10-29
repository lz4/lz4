#!/bin/sh

FPREFIX="tmp-tlb"

set -e

remove () {
    rm -rf $FPREFIX*
}

trap remove EXIT

set -x

datagen -g0       | lz4 -v     | lz4 -t
datagen -g16KB    | lz4 -9     | lz4 -t
datagen -g20KB > $FPREFIX-dg20k
lz4 < $FPREFIX-dg20k | lz4 -d > $FPREFIX-dec
cmp $FPREFIX-dg20k $FPREFIX-dec
lz4 --no-frame-crc < $FPREFIX-dg20k | lz4 -d > $FPREFIX-dec
cmp $FPREFIX-dg20k $FPREFIX-dec
datagen           | lz4 -BI    | lz4 -t
datagen           | lz4 --no-crc | lz4 -t
datagen -g6M -P99 | lz4 -9BD   | lz4 -t
datagen -g17M     | lz4 -9v    | lz4 -qt
datagen -g33M     | lz4 --no-frame-crc | lz4 -t
datagen -g256MB   | lz4 -vqB4D | lz4 -t --no-crc
echo "hello world" > $FPREFIX-hw
lz4 --rm -f $FPREFIX-hw $FPREFIX-hw.lz4
test ! -f $FPREFIX-hw                   # must fail (--rm)
test   -f $FPREFIX-hw.lz4
lz4 -d --rm -f $FPREFIX-hw.lz4
test ! -f $FPREFIX-hw.lz4
lz4 -l --rm -f $FPREFIX-hw $FPREFIX-hw.lz4   # legacy format
test ! -f $FPREFIX-hw                   # must fail (--rm)
lz4 -d --rm -f $FPREFIX-hw.lz4
test ! -f $FPREFIX-hw.lz4
touch -t 202001010000 $FPREFIX-hw
lz4 -f $FPREFIX-hw $FPREFIX-hw.lz4
test ! $FPREFIX-hw.lz4 -nt $FPREFIX-hw  # keeps modification time
lz4 -l -f $FPREFIX-hw $FPREFIX-hw.lz4
test ! $FPREFIX-hw.lz4 -nt $FPREFIX-hw  # same with legacy format
rm $FPREFIX-hw.lz4
case "$(uname)" in MINGW*|MSYS*|CYGWIN*) ;; *)
    # output of a private source stays private while it is written :
    # a stream interrupted by truncated input leaves it before its permissions are copied
    datagen -g200KB > $FPREFIX-priv
    chmod 600 $FPREFIX-priv
    lz4 -l -f $FPREFIX-priv $FPREFIX-priv.lz4
    head -c 1000 $FPREFIX-priv.lz4 > $FPREFIX-trunc.lz4
    ( umask 022; ! lz4 -d -f $FPREFIX-trunc.lz4 $FPREFIX-priv2 )
    ls -l $FPREFIX-priv2 | grep '^-rw-------'
    ;;
esac
# an output which is the input itself is refused, instead of truncating the input before it is read
datagen -g200KB > $FPREFIX-same
cp $FPREFIX-same $FPREFIX-same.orig
lz4 -f $FPREFIX-same $FPREFIX-same && exit 1
cmp $FPREFIX-same $FPREFIX-same.orig
lz4 -l -f $FPREFIX-same $FPREFIX-same && exit 1
cmp $FPREFIX-same $FPREFIX-same.orig
lz4 -f $FPREFIX-same $FPREFIX-same.lz4
cp $FPREFIX-same.lz4 $FPREFIX-same.lz4.orig
lz4 -d -f $FPREFIX-same.lz4 $FPREFIX-same.lz4 && exit 1
cmp $FPREFIX-same.lz4 $FPREFIX-same.lz4.orig
case "$(uname)" in MINGW*|MSYS*|CYGWIN*) ;; *)
    # idem through a link
    ln $FPREFIX-same $FPREFIX-same.hard
    lz4 -f $FPREFIX-same $FPREFIX-same.hard && exit 1
    cmp $FPREFIX-same $FPREFIX-same.orig
    ln -s $FPREFIX-same $FPREFIX-same.sym
    lz4 -f $FPREFIX-same $FPREFIX-same.sym && exit 1
    cmp $FPREFIX-same $FPREFIX-same.orig
    ln -s $FPREFIX-same.lz4 $FPREFIX-same.lz4.sym
    lz4 -d -f $FPREFIX-same.lz4 $FPREFIX-same.lz4.sym && exit 1
    cmp $FPREFIX-same.lz4 $FPREFIX-same.lz4.orig
    ;;
esac
lz4 --rm -f $FPREFIX-hw > /dev/null
test   -f $FPREFIX-hw.lz4               # no more implicit stdout
lz4cat $FPREFIX-hw.lz4 | grep "hello world"
unlz4 --rm $FPREFIX-hw.lz4 $FPREFIX-hw
test   -f $FPREFIX-hw
test ! -f $FPREFIX-hw.lz4               # must fail (--rm)
test ! -f $FPREFIX-hw.lz4.lz4           # must fail (unlz4)
lz4cat $FPREFIX-hw                      # pass-through mode
test   -f $FPREFIX-hw
test ! -f $FPREFIX-hw.lz4               # must fail (lz4cat)
lz4 $FPREFIX-hw $FPREFIX-hw.lz4         # creates $FPREFIX-hw.lz4
lz4cat < $FPREFIX-hw.lz4 > ${FPREFIX}3  # checks lz4cat works with stdin (#285)
cmp $FPREFIX-hw ${FPREFIX}3
lz4cat < $FPREFIX-hw > ${FPREFIX}2      # checks lz4cat works in pass-through mode
cmp $FPREFIX-hw ${FPREFIX}2
cp $FPREFIX-hw ./-d
lz4 --rm -- -d -d.lz4                   # compresses ./d into ./-d.lz4
test   -f ./-d.lz4
test ! -f ./-d
mv ./-d.lz4 ./-z
lz4 -d --rm -- -z ${FPREFIX}4           # uncompresses ./-z into $FPREFIX4
test ! -f ./-z
cmp $FPREFIX-hw ${FPREFIX}4
lz4 ${FPREFIX}2 ${FPREFIX}3 ${FPREFIX}4 && exit 1  # must fail: refuse to handle 3+ file names
mkdir -p ${FPREFIX}-dir
lz4 ${FPREFIX}-dir && exit 1          # must fail: refuse to compress directory
test ! -f ${FPREFIX}-dir.lz4          # must not create artifact (#1211)
lz4 -f $FPREFIX-hw                    # create $FPREFIX-hw.lz4, for next tests
lz4 --list $FPREFIX-hw.lz4            # test --list on valid single-frame file
lz4 --list < $FPREFIX-hw.lz4          # test --list from stdin (file only)
lz4 --list data/goldenSamples/dictID.bin  # test --list on a frame carrying a dictID (11-byte header)
head -c $(( $(wc -c < $FPREFIX-hw.lz4) - 4 )) $FPREFIX-hw.lz4 > $FPREFIX-trunc.lz4
lz4 --list $FPREFIX-trunc.lz4 && exit 1  # must fail: truncated frame (content checksum missing)
head -c 5 $FPREFIX-hw.lz4 > $FPREFIX-trunc.lz4
lz4 --list $FPREFIX-trunc.lz4 && exit 1  # must fail: truncated frame header
cat $FPREFIX-hw >> $FPREFIX-hw.lz4
lz4 -f $FPREFIX-hw.lz4 && exit 1      # uncompress valid frame followed by invalid data (must fail now)
printf '\120\052\115\030\000\000\000\000BAD!' > $FPREFIX-bad.lz4
cp $FPREFIX-bad.lz4 $FPREFIX-bad-before.lz4
lz4 -d --rm -f $FPREFIX-bad.lz4 $FPREFIX-bad.out && exit 1
cmp $FPREFIX-bad-before.lz4 $FPREFIX-bad.lz4  # --rm keeps source after decoding failure
echo "rm test" > $FPREFIX-rm
lz4 $FPREFIX-rm $FPREFIX-rm.lz4
cp $FPREFIX-rm.lz4 $FPREFIX-rm-before.lz4
lz4 -t --rm $FPREFIX-rm.lz4
cmp $FPREFIX-rm-before.lz4 $FPREFIX-rm.lz4    # -t --rm keeps its input ("No files are created nor removed")
lz4 -t --rm < $FPREFIX-rm.lz4                     # nothing to remove when reading stdin
lz4 -c --rm < $FPREFIX-rm | lz4 -t                # idem when compressing
lz4 -dc --rm < $FPREFIX-rm.lz4 | cmp - $FPREFIX-rm  # idem when decompressing
lz4 -d --rm $FPREFIX-rm.lz4 $FPREFIX-rm-out
test ! -f $FPREFIX-rm.lz4                         # --rm still removes a regular source file
test   -f $FPREFIX-rm-out
lz4 -BX $FPREFIX-hw -c -q | lz4 -tv   # test block checksum
# datagen -g20KB generates the same file every single time
# cannot save output of datagen -g20KB as input file to lz4 because the following shell commands are run before datagen -g20KB
test "$(datagen -g20KB | lz4 -c --fast | wc -c)" -lt "$(datagen -g20KB | lz4 -c --fast=9 | wc -c)" # -1 vs -9
test "$(datagen -g20KB | lz4 -c -1 | wc -c)" -lt "$(datagen -g20KB| lz4 -c --fast=1 | wc -c)" # 1 vs -1
test "$(datagen -g20KB | lz4 -c --fast=1 | wc -c)" -eq "$(datagen -g20KB| lz4 -c --fast| wc -c)" # checks default fast compression is -1
test "$(lz4 -c -l -1 $FPREFIX-dg20k | wc -c)" -lt "$(lz4 -c -l --fast=1 $FPREFIX-dg20k | wc -c)" # same with legacy format
lz4 -c -l --fast=2147483647 $FPREFIX-dg20k > /dev/null  # legacy format : acceleration capped without overflow
test "$(lz4 -c -l -2 $FPREFIX-dg20k | wc -c)" -lt "$(lz4 -c -l -1 $FPREFIX-dg20k | wc -c)" # legacy format : -2 is high compression, as in the frame format
lz4 -c -l -2 $FPREFIX-dg20k | lz4 -d | diff -q - $FPREFIX-dg20k  # and decodes
lz4 -c --fast=0 $FPREFIX-dg20k && exit 1  # lz4 should fail when fast=0
lz4 -c --fast=-1 $FPREFIX-dg20k && exit 1 # lz4 should fail when fast=-1
lz4 -c --fast=5x $FPREFIX-dg20k && exit 1 # lz4 should fail on trailing garbage
lz4 -c --threadsX 2 $FPREFIX-dg20k && exit 1 # lz4 should fail on unknown long command
# numeric arguments must not silently overflow
lz4 -c --fast=3000000000 $FPREFIX-dg20k > /dev/null && exit 1  # > INT_MAX
lz4 -c --fast=4294967297 $FPREFIX-dg20k > /dev/null && exit 1  # > UINT_MAX (would wrap to 1)
lz4 -c -3000000000 $FPREFIX-dg20k > /dev/null && exit 1        # > INT_MAX
lz4 -c -4294967305 $FPREFIX-dg20k > /dev/null && exit 1        # > UINT_MAX (would wrap to 9)
lz4 -c -T4294967297 $FPREFIX-dg20k > /dev/null && exit 1       # > UINT_MAX (would wrap to 1)
# an overflowing LZ4_CLEVEL is ignored, like other invalid values
test "$(LZ4_CLEVEL=4294967305 lz4 -c $FPREFIX-dg20k | wc -c)" -eq "$(lz4 -c $FPREFIX-dg20k | wc -c)"
# a value followed by trailing garbage is ignored, like other invalid values
test "$(LZ4_CLEVEL=9x lz4 -c $FPREFIX-dg20k | wc -c)" -eq "$(lz4 -c $FPREFIX-dg20k | wc -c)"
LZ4_NBWORKERS=2x lz4 -c $FPREFIX-dg20k 2>&1 >/dev/null | grep "Ignore environment variable setting LZ4_NBWORKERS"
# acceleration (--fast=# + 1) is capped at LZ4_ACCELERATION_MAX (65537) :
# the largest --fast value behaves like --fast=65536, without overflow
lz4 -c --fast=65536 $FPREFIX-dg20k > $FPREFIX-fastmax.lz4
lz4 -c --fast=2147483647 $FPREFIX-dg20k | cmp - $FPREFIX-fastmax.lz4
lz4 -bq --fast=2147483647 -i0 $FPREFIX-dg20k
# -v reports the processing speed after a successful decompression, like after compression,
# but not after a failed one (it isn't reported on systems where MT time measurements are unsupported)
if lz4 -vf $FPREFIX-dg20k $FPREFIX-v.lz4 2>&1 | grep -q "Done in"; then
    lz4 -vdf $FPREFIX-v.lz4 $FPREFIX-v 2>&1 | grep "Done in"
fi
if lz4 -vdf $FPREFIX-missing.lz4 $FPREFIX-v 2>&1 | grep "Done in"; then exit 1; fi
# Multithreading commands
datagen -g16M | lz4 -T2 | lz4 -t
datagen -g16M | lz4 --threads=2 | lz4 -t
# High --fast values can result in out-of-bound dereferences #876
datagen -g1M | lz4 -c --fast=999999999 > $FPREFIX-trash
# Test for #596
echo "TEST" > $FPREFIX-test
lz4 -m $FPREFIX-test
lz4 $FPREFIX-test.lz4 $FPREFIX-test2
cmp $FPREFIX-test $FPREFIX-test2
# bug #1374
datagen -g4194302 | lz4 -B4 -c > $FPREFIX-test3
# -r doesn't follow symbolic links to directories : this one leads back to the listed directory
mkdir $FPREFIX-rdir
echo "hello world" > $FPREFIX-rdir/file
if ln -s . $FPREFIX-rdir/loop 2>/dev/null && test -L $FPREFIX-rdir/loop; then
    lz4 -r $FPREFIX-rdir 2> $FPREFIX-rdir.err
    grep "symbolic link" $FPREFIX-rdir.err
    lz4 -t $FPREFIX-rdir/file.lz4
fi
