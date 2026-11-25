#!/bin/sh

FPREFIX="tmp-lfc"

set -e
remove () {
    rm $FPREFIX*
}

trap remove EXIT

set -x

echo > $FPREFIX-empty
echo hi > $FPREFIX-nonempty
cat $FPREFIX-nonempty $FPREFIX-empty $FPREFIX-nonempty > $FPREFIX-src
lz4 -zq $FPREFIX-empty -c > $FPREFIX-empty.lz4
lz4 -zq $FPREFIX-nonempty -c > $FPREFIX-nonempty.lz4
cat $FPREFIX-nonempty.lz4 $FPREFIX-empty.lz4 $FPREFIX-nonempty.lz4 > $FPREFIX-concat.lz4
lz4 -d $FPREFIX-concat.lz4 -c > $FPREFIX-result
cmp $FPREFIX-src $FPREFIX-result

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

# a legacy stream can follow a regular frame (issue #2166)
printf 'hello\n' > $FPREFIX-legacy1
printf 'world\n' > $FPREFIX-legacy2
lz4 -zq -c $FPREFIX-legacy1 > $FPREFIX-legacy.lz4
lz4 -zq -l -c $FPREFIX-legacy2 >> $FPREFIX-legacy.lz4
cat $FPREFIX-legacy1 $FPREFIX-legacy2 > $FPREFIX-legacysrc
lz4 -d -c $FPREFIX-legacy.lz4 > $FPREFIX-legacyresult
cmp $FPREFIX-legacysrc $FPREFIX-legacyresult
lz4 -t $FPREFIX-legacy.lz4
# same, with an empty regular frame and with a skippable frame in between
: > $FPREFIX-void
printf '\120\052\115\030\004\000\000\000abcd' > $FPREFIX-skippable
lz4 -zq -c $FPREFIX-void > $FPREFIX-legacyskip.lz4
cat $FPREFIX-skippable >> $FPREFIX-legacyskip.lz4
lz4 -zq -l -c $FPREFIX-legacy2 >> $FPREFIX-legacyskip.lz4
lz4 -d -c $FPREFIX-legacyskip.lz4 > $FPREFIX-legacyskipresult
cmp $FPREFIX-legacy2 $FPREFIX-legacyskipresult
# legacy blocks can be larger than the decoding input buffers
datagen -g5M -P0 > $FPREFIX-legacybig
lz4 -zq -c $FPREFIX-legacy1 > $FPREFIX-legacybig.lz4
lz4 -zq -l -c $FPREFIX-legacybig >> $FPREFIX-legacybig.lz4
cat $FPREFIX-legacy1 $FPREFIX-legacybig > $FPREFIX-legacybigsrc
lz4 -d -c $FPREFIX-legacybig.lz4 > $FPREFIX-legacybigresult
cmp $FPREFIX-legacybigsrc $FPREFIX-legacybigresult

# the decompression benchmark (-b -d) decodes all frames of a file, skippable ones included
lz4 -b -d -q -i0 $FPREFIX-concat.lz4
lz4 -b -d -q -i0 $FPREFIX-bigconcat.lz4
cat $FPREFIX-skippable $FPREFIX-big1.lz4 > $FPREFIX-skipconcat.lz4
lz4 -b -d -q -i0 $FPREFIX-skipconcat.lz4
