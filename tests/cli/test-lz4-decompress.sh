#!/bin/sh

# decompression : frames, legacy frames and skippable frames of any size, one after the other,
# from a file or a pipe, into files or stdout, with sparse output or not ;
# decoded size, errors, several inputs (-m, lz4cat)
# sections are independent : they only read the inputs created below

FPREFIX="tmp-tld"

set -e
unset LZ4_CLEVEL LZ4_NBWORKERS LZ4_MEMORY   # set by the tests only

remove () {
    rm -rf $FPREFIX*
}

trap remove EXIT

set -x

# inputs
SRC=$FPREFIX-src;  datagen -g300KB > $SRC
F=$FPREFIX-f.lz4;  lz4 -q $SRC $F       # frame
L=$FPREFIX-l.lz4;  lz4 -q -l $SRC $L    # legacy frame
OUT=$FPREFIX-out
ERR=$FPREFIX-err

# le32 N : N as 4 bytes, little endian ; skip SIZE : skippable frame of SIZE bytes
le32() { printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $(($1 & 255)) $(($1 >> 8 & 255)) $(($1 >> 16 & 255)) $(($1 >> 24 & 255)))"; }
skip() { printf '\120\052\115\030'; le32 $1; if [ $1 -gt 0 ]; then head -c $1 $SRC; fi; }   # (BSD head : no -c 0)
# fails COMMAND MESSAGE : COMMAND fails with an error code, not a crash, and displays MESSAGE ("" : nothing)
fails() {
    rc=0; sh -c "$1" > /dev/null 2> $ERR || rc=$?
    if [ $rc -gt 0 ] && [ $rc -lt 128 ]; then
        if [ -z "$2" ] && [ ! -s $ERR ]; then return 0; fi
        if [ -n "$2" ] && grep -q "$2" $ERR; then return 0; fi
    fi
    cat $ERR; return 1
}


# frames of any type, one after the other ; skippable frames : from a file (seek) or a pipe (read)
{ cat $L; skip 0; skip 1; skip 2; skip 3; skip 20000; cat $L; skip 5; cat $F; } > $FPREFIX-cat.lz4
cat $SRC $SRC $SRC > $FPREFIX-cat
lz4 -d -f $FPREFIX-cat.lz4 $OUT 2> $ERR
cmp $FPREFIX-cat $OUT
grep -q "decoded 921600 bytes" $ERR
cat $FPREFIX-cat.lz4 | lz4 -dc > $OUT
cmp $FPREFIX-cat $OUT
{ skip 0; cat $L; } | lz4 -dc > $OUT
cmp $SRC $OUT
# empty blocks (not written by lz4) : decoded
printf '\002\041\114\030\001\000\000\000\000' | lz4 -t        # legacy frame
{ printf 'mozLz40\000'; le32 0; printf '\000'; } | lz4 -t      # mozlz4

# zeros : skipped in sparse files (output files, and stdout with --sparse), same content
{ head -c 131072 /dev/zero; printf 'abcdefgh\000'; } > $FPREFIX-z1   # ends with a single zero
{ head -c 100000 /dev/zero; printf 'x'; head -c 120000 /dev/zero; } > $FPREFIX-z2
for z in $FPREFIX-z1 $FPREFIX-z2; do
    lz4 -q -f -B4 $z $z.lz4
    lz4 -q -f -l $z $z-l.lz4
    { printf 'mozLz40\000'; le32 $(wc -c < $z); tail -c +9 $z-l.lz4; } > $z.jsonlz4   # (single legacy block)
    for c in $z.lz4 $z-l.lz4 $z.jsonlz4; do
        lz4 -q -d -f $c $OUT;  cmp $z $OUT
        lz4 -q -d -f --no-sparse $c $OUT;  cmp $z $OUT
        lz4 -q -d -c --sparse $c > $OUT;  cmp $z $OUT
    done
    lz4 -q -d -c -f --sparse $z > $OUT;  cmp $z $OUT   # not lz4 : pass-through (-c -f)
done

# checksums : content checksums verified by lz4 itself in multi-threaded decompression (see LZ4IO_frameCheck_t),
# by liblz4 otherwise ; block checksums verified by liblz4 ; frames of each kind, one after the other
B=$FPREFIX-big;  datagen -g9MB > $B   # several 4 MB blocks
lz4 -q $B $B-c.lz4
lz4 -q --no-frame-crc $B $B-n.lz4
lz4 -q -BX $B $B-x.lz4
{ cat $B-c.lz4 $B-n.lz4; skip 20000; cat $B-x.lz4 $F $B-c.lz4; } | lz4 -dc > $OUT
cat $B $B $B $SRC $B | cmp - $OUT
# input is read by pieces (INBUFF_FIRST_READS, then INBUFF_SIZE), ending at multiples of their size :
# a frame of 4194306 bytes (64 stored blocks of 64 KB) ends with its checksum across offset 4 MB
R=$FPREFIX-rnd;  datagen -g4194035 -P0 > $R
lz4 -q -B4 $R $R.lz4
test "$(wc -c < $R.lz4)" -eq 4194306
lz4 -dc $R.lz4 > $OUT;  cmp $R $OUT
# wrong checksums : detected
flip() { b=$(od -An -tu1 -j$2 -N1 $1 | tr -d ' \n'); printf "$(printf '\\%03o' $((b ^ 255)))" | dd of=$1 bs=1 seek=$2 conv=notrunc 2> /dev/null; }   # flip FILE OFFSET
for o in 4194302 4194303 4194304 4194305; do
    cp $R.lz4 $FPREFIX-bad.lz4;  flip $FPREFIX-bad.lz4 $o
    fails "lz4 -dc $FPREFIX-bad.lz4" "contentChecksum_invalid"
done
cp $B-c.lz4 $FPREFIX-bad.lz4;  flip $FPREFIX-bad.lz4 $(($(wc -c < $B-c.lz4) - 1))
fails "lz4 -dc $FPREFIX-bad.lz4" "contentChecksum_invalid"
fails "lz4 -t $FPREFIX-bad.lz4" "contentChecksum_invalid"
fails "cat $B-c.lz4 $B-n.lz4 $FPREFIX-bad.lz4 | lz4 -dc" "contentChecksum_invalid"
lz4 -q -dc --no-frame-crc $FPREFIX-bad.lz4 > $OUT;  cmp $B $OUT   # not verified, on request
cat $B-c.lz4 $FPREFIX-bad.lz4 | lz4 -q -dc --no-frame-crc > $OUT;  cat $B $B | cmp - $OUT   # in every frame
cat $B-c.lz4 $FPREFIX-bad.lz4 | lz4 -q -dc --no-crc > $OUT;  cat $B $B | cmp - $OUT
fails "lz4 -dc --no-crc --frame-crc $FPREFIX-bad.lz4" "contentChecksum_invalid"   # last option prevails
cp $B-x.lz4 $FPREFIX-bad.lz4;  flip $FPREFIX-bad.lz4 $(($(wc -c < $B-x.lz4) - 9))   # last block checksum
fails "lz4 -dc $FPREFIX-bad.lz4" "blockChecksum_invalid"
fails "cat $B-x.lz4 $FPREFIX-bad.lz4 | lz4 -dc --no-frame-crc" "blockChecksum_invalid"   # frame checksums only
cat $B-x.lz4 $FPREFIX-bad.lz4 | lz4 -q -dc --no-crc > $OUT;  cat $B $B | cmp - $OUT
fails "lz4 -bdi0 --no-frame-crc $FPREFIX-bad.lz4" "blockChecksum_invalid"   # decompression benchmark : same
lz4 -q -bdi0 --no-crc $FPREFIX-bad.lz4
cp $B-x.lz4 $FPREFIX-bad.lz4;  flip $FPREFIX-bad.lz4 $(($(wc -c < $B-x.lz4) - 1))   # with block checksums
fails "lz4 -dc $FPREFIX-bad.lz4" "contentChecksum_invalid"
lz4 -q -dc --no-frame-crc $FPREFIX-bad.lz4 > $OUT;  cmp $B $OUT   # block checksums verified, frame checksum skipped
cp $F $FPREFIX-bad.lz4;  flip $FPREFIX-bad.lz4 $(($(wc -c < $F) - 1))   # frame decoded at once
fails "lz4 -dc $FPREFIX-bad.lz4" "contentChecksum_invalid"
fails "cat $B-c.lz4 $FPREFIX-bad.lz4 | lz4 -dc" "contentChecksum_invalid"
# frame header split across reads (the first one ends at offset 256 KB) : large frame, then frame decoded at once
N=$((262144 - 3 - $(wc -c < $F) - 8));  test $N -gt 0   # skippable frame : next header starts 3 bytes before
{ cat $F; skip $N; cat $B-c.lz4; } > $FPREFIX-split.lz4
lz4 -dc $FPREFIX-split.lz4 > $OUT;  cat $SRC $B | cmp - $OUT
flip $FPREFIX-split.lz4 $(($(wc -c < $FPREFIX-split.lz4) - 1))
fails "lz4 -dc $FPREFIX-split.lz4" "contentChecksum_invalid"
{ cat $F; skip $N; cat $F; } > $FPREFIX-split.lz4
lz4 -dc $FPREFIX-split.lz4 > $OUT;  cat $SRC $SRC | cmp - $OUT
flip $FPREFIX-split.lz4 $(($(wc -c < $FPREFIX-split.lz4) - 1))
fails "lz4 -dc $FPREFIX-split.lz4" "contentChecksum_invalid"


# errors
fails "lz4 -t $SRC" "Unrecognized header : file cannot be decoded"
printf '\004\042\115\030' > $FPREFIX-bad.lz4                 # frame : magic number only
fails "lz4 -t $FPREFIX-bad.lz4" "Error"
{ cat $F; head -c 7 $F; } > $FPREFIX-bad.lz4               # frame, then header of the next one only
fails "lz4 -t $FPREFIX-bad.lz4" "Error"
printf '\002\041\114\030\001\000\000\000\360' > $FPREFIX-bad.lz4   # legacy block : invalid
fails "lz4 -t $FPREFIX-bad.lz4" "Corrupted input"
{ skip 4; printf 'ab'; } > $FPREFIX-bad.lz4
fails "lz4 -t $FPREFIX-bad.lz4" "Magic Number unreadable"
{ cat $L; printf 'ab'; } > $FPREFIX-bad.lz4
fails "lz4 -t $FPREFIX-bad.lz4" "cannot read block size"
{ printf 'mozLz40\000'; le32 4294967295; printf '\000'; } > $FPREFIX-bad.lz4
fails "lz4 -t $FPREFIX-bad.lz4" "too large"
# undecodable data after a frame : position reported, except with -q
{ skip 4; printf 'garbage!'; } > $FPREFIX-bad.lz4
fails "lz4 -t $FPREFIX-bad.lz4" "undecodable data at position [0-9]* *$"
fails "lz4 -q -t $FPREFIX-bad.lz4" ""
# input read errors (stdin opened for writing) : not an empty input
if [ "$(uname)" = Linux ]; then
    fails "lz4 -dc 0>/dev/null" "Read error"
    fails "lz4 -t 0>/dev/null" "Read error"
fi


# several inputs : each one decoded from its beginning, whatever the end of the previous one
cp $F $FPREFIX-1.lz4
cp $F $FPREFIX-2.bin
fails "lz4 -d -f -m $FPREFIX-1.lz4 $FPREFIX-2.bin" "File extension doesn't match"   # (.lz4 expected)
cmp $SRC $FPREFIX-1
test ! -f $FPREFIX-2
fails "lz4 -qq -d -f -m $FPREFIX-1.lz4 $FPREFIX-2.bin" ""
fails "lz4cat $FPREFIX-absent.lz4 $F > $OUT" "absent.lz4"   # (missing input)
cmp $SRC $OUT
# an existing output is kept without -f
cp $SRC $FPREFIX-exists
fails "lz4 -q -d $L $FPREFIX-exists" "not overwritten"
cmp $SRC $FPREFIX-exists
# an existing output is kept when the source can't be opened (the source is opened first)
cp $SRC $FPREFIX-keep
fails "lz4 -q -d -f $FPREFIX-absent.lz4 $FPREFIX-keep" "absent.lz4"
cmp $SRC $FPREFIX-keep
# output modification time : that of the source
touch -t 200001010000 $FPREFIX-1.lz4
lz4 -q -d -f $FPREFIX-1.lz4
test ! $FPREFIX-1 -nt $FPREFIX-1.lz4
