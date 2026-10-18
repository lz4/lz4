#!/bin/sh

FPREFIX="tmp-lml"

set -e

remove () {
    rm $FPREFIX*
}

trap remove EXIT

set -x

datagen -s1        > "${FPREFIX}1" 2> $FPREFIX-trash
datagen -s2 -g100K > "${FPREFIX}2" 2> $FPREFIX-trash
datagen -s3 -g200K > "${FPREFIX}3" 2> $FPREFIX-trash
# compress multiple files using legacy format: one .lz4 per source file
lz4 -f -l -m $FPREFIX*
test -f ${FPREFIX}1.lz4
test -f ${FPREFIX}2.lz4
test -f ${FPREFIX}3.lz4
# decompress multiple files compressed using legacy format: one output file per .lz4
mv ${FPREFIX}1 ${FPREFIX}1-orig
mv ${FPREFIX}2 ${FPREFIX}2-orig
mv ${FPREFIX}3 ${FPREFIX}3-orig
lz4 -d -f -m $FPREFIX*.lz4
lz4 -l -d -f -m $FPREFIX*.lz4 # -l mustn't impact -d option
cmp ${FPREFIX}1 ${FPREFIX}1-orig   # must be identical
cmp ${FPREFIX}2 ${FPREFIX}2-orig
cmp ${FPREFIX}3 ${FPREFIX}3-orig
# compress multiple files into stdout using legacy format
cat ${FPREFIX}1.lz4 ${FPREFIX}2.lz4 ${FPREFIX}3.lz4 > $FPREFIX-concat1
rm -f $FPREFIX*.lz4
lz4 -l -m ${FPREFIX}1 ${FPREFIX}2 ${FPREFIX}3 -c > $FPREFIX-concat2
test ! -f ${FPREFIX}1.lz4  # must not create .lz4 artefact
cmp $FPREFIX-concat1 $FPREFIX-concat2  # must be equivalent
# # # decompress multiple files into stdout using legacy format
rm $FPREFIX-concat1 $FPREFIX-concat2
lz4 -l -f -m ${FPREFIX}1 ${FPREFIX}2 ${FPREFIX}3   # generate .lz4 to decompress
cat ${FPREFIX}1 ${FPREFIX}2 ${FPREFIX}3 > ${FPREFIX}-concat1   # create concatenated reference
rm ${FPREFIX}1 ${FPREFIX}2 ${FPREFIX}3
lz4 -d -m ${FPREFIX}1.lz4 ${FPREFIX}2.lz4 ${FPREFIX}3.lz4 -c > $FPREFIX-concat2
lz4 -d -l -m ${FPREFIX}1.lz4 ${FPREFIX}2.lz4 ${FPREFIX}3.lz4 -c > $FPREFIX-concat2 # -l mustn't impact option -d
test ! -f ${FPREFIX}1  # must not create file artefact
cmp $FPREFIX-concat1 $FPREFIX-concat2  # must be equivalent
# # # compress multiple files, one of which is absent (must fail)
rm -f $FPREFIX-concat2.lz4
lz4 -f -l -m $FPREFIX-concat1 notHere-legacy $FPREFIX-concat2 && exit 1 # must fail : notHere-legacy not present
test -f $FPREFIX-concat2.lz4 # notHere was a non-blocking error, concat2.lz4 should be present
# test multiple legacy files without writing decoded content
lz4 -tm $FPREFIX-concat1.lz4 $FPREFIX-concat2.lz4 > $FPREFIX-test-output
test ! -s $FPREFIX-test-output

# The Linux kernel accepts a zero block-size word as a legacy end marker.
# Use several blocks and a sparse tail to exercise queued writes and flushing.
datagen -s4 -g25M > $FPREFIX-zero-orig
datagen -g1M -P100 >> $FPREFIX-zero-orig
lz4 -l -f $FPREFIX-zero-orig $FPREFIX-zero.lz4
printf '\000\000\000\000' >> $FPREFIX-zero.lz4

check_legacy_endmark () {
    lz4 -d -f "$1" $FPREFIX-zero-decoded
    cmp "$2" $FPREFIX-zero-decoded
    cat "$1" | lz4 -dc > $FPREFIX-zero-decoded
    cmp "$2" $FPREFIX-zero-decoded
    lz4 -t "$1" > $FPREFIX-test-output
    test ! -s $FPREFIX-test-output
    cat "$1" | lz4 -t > $FPREFIX-test-output
    test ! -s $FPREFIX-test-output
    # Verbose compressed sizes include every header, including the end marker.
    lz4 --list -v "$1" > $FPREFIX-zero-list
    listed_size=$(awk '$1 ~ /^[0-9]+$/ {size += $5} END {print size}' $FPREFIX-zero-list)
    test "$listed_size" -eq "$(wc -c < "$1")"
}

check_legacy_endmark $FPREFIX-zero.lz4 $FPREFIX-zero-orig

# An empty terminated frame is just its magic number and a zero size word.
printf '\002\041\114\030\000\000\000\000' > $FPREFIX-zero-empty.lz4
: > $FPREFIX-zero-empty-orig
check_legacy_endmark $FPREFIX-zero-empty.lz4 $FPREFIX-zero-empty-orig
test "$(awk '$2 == "LegacyFrame" {print $1, $5}' $FPREFIX-zero-list)" = "1 8"

# Continue at the next frame, even through a pipe: legacy, skippable and modern.
lz4 -f ${FPREFIX}2-orig $FPREFIX-zero-modern.lz4
printf '\120\052\115\030\003\000\000\000abc' > $FPREFIX-zero-skip.lz4
cat $FPREFIX-zero.lz4 ${FPREFIX}1.lz4 $FPREFIX-zero-empty.lz4 \
    $FPREFIX-zero-skip.lz4 $FPREFIX-zero-modern.lz4 > $FPREFIX-zero-concat.lz4
cat $FPREFIX-zero-orig ${FPREFIX}1-orig ${FPREFIX}2-orig > $FPREFIX-zero-concat-orig
check_legacy_endmark $FPREFIX-zero-concat.lz4 $FPREFIX-zero-concat-orig
test "$(awk '$1 ~ /^[0-9]+$/ {print $2}' $FPREFIX-zero-list)" = "LegacyFrame
LegacyFrame
LegacyFrame
SkippableFrame
LZ4Frame"

# A terminator cannot make following block-like data part of the old frame.
cat $FPREFIX-zero.lz4 > $FPREFIX-zero-invalid.lz4
printf '\001\000\000\000\000' >> $FPREFIX-zero-invalid.lz4
lz4 -t $FPREFIX-zero-invalid.lz4 && exit 1
lz4 --list $FPREFIX-zero-invalid.lz4 && exit 1

# Partial block headers and actually corrupt blocks must still fail.
for suffix in '\000' '\000\000' '\000\000\000' '\003\000\000\000\377\377\377'; do
    cat ${FPREFIX}1.lz4 > $FPREFIX-zero-invalid.lz4
    printf "$suffix" >> $FPREFIX-zero-invalid.lz4
    lz4 -t $FPREFIX-zero-invalid.lz4 && exit 1
done
true
