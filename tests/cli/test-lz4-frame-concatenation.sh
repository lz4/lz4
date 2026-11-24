#!/bin/sh

FPREFIX="tmp-lfc"

set -e
remove () {
    rm $FPREFIX*
}

trap remove EXIT

set -x

: > $FPREFIX-empty
echo hi > $FPREFIX-nonempty
cat $FPREFIX-nonempty $FPREFIX-empty $FPREFIX-nonempty > $FPREFIX-src
lz4 -zq $FPREFIX-empty -c > $FPREFIX-empty.lz4
lz4 -zq $FPREFIX-nonempty -c > $FPREFIX-nonempty.lz4
cat $FPREFIX-nonempty.lz4 $FPREFIX-empty.lz4 $FPREFIX-nonempty.lz4 > $FPREFIX-concat.lz4
lz4 -d $FPREFIX-concat.lz4 -c > $FPREFIX-result
cmp $FPREFIX-src $FPREFIX-result

# A zero-byte source must produce a real empty frame, not just a newline.
lz4 -dc $FPREFIX-empty.lz4 > $FPREFIX-result
test ! -s $FPREFIX-result
# Empty frames at either edge, or without any nonempty frame, are valid too.
for frames in "$FPREFIX-empty.lz4 $FPREFIX-nonempty.lz4" \
              "$FPREFIX-nonempty.lz4 $FPREFIX-empty.lz4" \
              "$FPREFIX-empty.lz4 $FPREFIX-empty.lz4"; do
    cat $frames > $FPREFIX-edge.lz4
    lz4 -dc $FPREFIX-edge.lz4 > $FPREFIX-result
    case "$frames" in
        *nonempty*) cmp $FPREFIX-nonempty $FPREFIX-result ;;
        *) test ! -s $FPREFIX-result ;;
    esac
    lz4 -t $FPREFIX-edge.lz4
    lz4 -b -d -q -i0 $FPREFIX-edge.lz4
done

# block checksums, no content checksum, last block compressed > 64 KB :
# decoding the first frame must not read into the next one
datagen -g250KB -s1 > $FPREFIX-big1
datagen -g200KB -s2 > $FPREFIX-big2
cat $FPREFIX-big1 $FPREFIX-big2 > $FPREFIX-bigsrc
lz4 -zq -B5 -BX --no-frame-crc $FPREFIX-big1 -c > $FPREFIX-big1.lz4
lz4 -zq -B5 -BX --no-frame-crc $FPREFIX-big2 -c > $FPREFIX-big2.lz4
cat $FPREFIX-big1.lz4 $FPREFIX-big2.lz4 > $FPREFIX-bigconcat.lz4
lz4 -d $FPREFIX-bigconcat.lz4 -c > $FPREFIX-bigresult
cmp $FPREFIX-bigsrc $FPREFIX-bigresult

# the decompression benchmark (-b -d) decodes all frames of a file, skippable ones included
lz4 -b -d -q -i0 $FPREFIX-concat.lz4
lz4 -b -d -q -i0 $FPREFIX-bigconcat.lz4
printf '\120\052\115\030\004\000\000\000abcd' > $FPREFIX-skippable
cat $FPREFIX-skippable $FPREFIX-big1.lz4 > $FPREFIX-skipconcat.lz4
lz4 -b -d -q -i0 $FPREFIX-skipconcat.lz4
