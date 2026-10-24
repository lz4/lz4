#!/bin/sh

FPREFIX="tmp-tlm"

set -e

remove () {
    rm -f $FPREFIX*
}

trap remove EXIT

# le32 N : write N as 4 bytes, little endian
le32 () {
    printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $(($1 & 255)) $(($1 >> 8 & 255)) $(($1 >> 16 & 255)) $(($1 >> 24 & 255)))"
}

# mozlz4 SRC DST : the legacy format stores SRC (< 8 MB) as a single LZ4 block,
# after a 4-byte magic number and a 4-byte block size
mozlz4 () {
    { printf 'mozLz40\0'; le32 $(wc -c < "$1"); lz4 -c -l "$1" | tail -c +9; } > "$2"
}

set -x

datagen -g1MB > $FPREFIX-src
mozlz4 $FPREFIX-src $FPREFIX.jsonlz4
lz4 -d $FPREFIX.jsonlz4 $FPREFIX-dec
cmp $FPREFIX-src $FPREFIX-dec
lz4cat < $FPREFIX.jsonlz4 | cmp $FPREFIX-src -
lz4 -t $FPREFIX.jsonlz4
# empty content : Firefox writes no block, other tools write an empty block
{ printf 'mozLz40\0'; le32 0; } > $FPREFIX-empty.jsonlz4
test "$(lz4cat $FPREFIX-empty.jsonlz4 | wc -c)" -eq 0
{ printf 'mozLz40\0'; le32 0; printf '\0'; } > $FPREFIX-empty.jsonlz4
test "$(lz4cat $FPREFIX-empty.jsonlz4 | wc -c)" -eq 0
{ printf 'mozLz40\0'; le32 0; printf 'X'; } | lz4 -t && exit 1   # not an empty block
{ printf 'mozLz40\0'; le32 1; } | lz4 -t && exit 1               # missing block
# like Firefox, the decompressed size is only used as decoding capacity
{ printf 'mozLz40\0'; le32 $(( $(wc -c < $FPREFIX-src) + 1000 )); tail -c +13 $FPREFIX.jsonlz4; } > $FPREFIX-large.jsonlz4
lz4cat $FPREFIX-large.jsonlz4 | cmp $FPREFIX-src -

# corrupted files
head -c 1000 $FPREFIX.jsonlz4 > $FPREFIX-trunc.jsonlz4
lz4 -t $FPREFIX-trunc.jsonlz4 && exit 1    # truncated block
{ printf 'mozLz40\0'; le32 1000; tail -c +13 $FPREFIX.jsonlz4; } > $FPREFIX-small.jsonlz4
lz4 -t $FPREFIX-small.jsonlz4 && exit 1    # decompressed size too small
{ printf 'mozLz40\0'; le32 4294967295; printf '\0'; } > $FPREFIX-huge.jsonlz4
lz4 -t $FPREFIX-huge.jsonlz4 && exit 1     # decompressed size > LZ4_MAX_INPUT_SIZE
printf 'mozLz40\0' | lz4 -t && exit 1      # decompressed size missing

# not mozlz4 : pass-through unchanged, including the bytes read to check the magic
printf 'mozLz41 : not mozlz4' > $FPREFIX-txt
lz4cat $FPREFIX-txt | cmp $FPREFIX-txt -
lz4 -t $FPREFIX-txt && exit 1
printf 'mozLz4' > $FPREFIX-short
lz4cat $FPREFIX-short | cmp $FPREFIX-short -
