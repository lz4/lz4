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
skip() { printf '\120\052\115\030'; le32 $1; head -c $1 $SRC; }
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


# errors
fails "lz4 -t $SRC" "Unrecognized header : file cannot be decoded"
printf '\004\042\115\030' > $FPREFIX-bad.lz4                 # frame : magic number only
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
# output modification time : that of the source
touch -t 200001010000 $FPREFIX-1.lz4
lz4 -q -d -f $FPREFIX-1.lz4
test ! $FPREFIX-1 -nt $FPREFIX-1.lz4
