#!/bin/sh

SKIPFILE="data/goldenSamples/skip.bin"
FPREFIX="tmp-lsk"

set -e

remove () {
    rm "$FPREFIX"*
}

trap remove EXIT

set -x

lz4 -dc $SKIPFILE
lz4 -dc < $SKIPFILE
printf "Hello from Valid Frame!\n" | lz4 -c > $FPREFIX.lz4
cat $SKIPFILE $FPREFIX.lz4 $SKIPFILE | lz4 -dc

# truncated skippable frame : must fail, whether input is a file or a pipe
head -c 20 $SKIPFILE > $FPREFIX-trunc.lz4
if lz4 -t $FPREFIX-trunc.lz4; then false; fi
if lz4 -t < $FPREFIX-trunc.lz4; then false; fi
if cat $FPREFIX-trunc.lz4 | lz4 -t; then false; fi
if lz4 --list $FPREFIX-trunc.lz4; then false; fi
# skippable frame ending exactly at end of file remains valid
lz4 -t $SKIPFILE
lz4 --list $SKIPFILE
