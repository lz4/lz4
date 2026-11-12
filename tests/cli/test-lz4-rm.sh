#!/bin/sh

# --rm removes the source only once its output is complete :
# each operation (compression, legacy format, decompression) and output (file, multiple files, stdout)
# removes the source on success, and keeps it on failure (corrupted input, output not written) or in test mode

FPREFIX="tmp-trm"

set -e

remove () {
    rm -rf $FPREFIX*
}

trap remove EXIT

set -x

isErrorCode() { test "$1" -gt 0 && test "$1" -lt 128; }   # not a crash (signal, assert)
# removed SRC COMMAND : COMMAND succeeds and removes SRC
removed() { rc=0; sh -c "$2" || rc=$?; test $rc = 0; test ! -e "$1"; }
# kept SRC COMMAND : COMMAND fails, and keeps SRC
kept() { rc=0; sh -c "$2" || rc=$?; isErrorCode $rc; test -f "$1"; }

datagen -g300K > $FPREFIX-src
datagen -g9M > $FPREFIX-big   # several jobs
datagen -g5K > $FPREFIX-small   # output within stdio buffers : write errors only reported when flushed
lz4 -q -f $FPREFIX-src $FPREFIX-ref.lz4
lz4 -q -f $FPREFIX-small $FPREFIX-small.lz4
lz4 -q -f -l $FPREFIX-src $FPREFIX-legacy.lz4
cp $FPREFIX-ref.lz4 $FPREFIX-bad.lz4
printf 'corrupted' | dd of=$FPREFIX-bad.lz4 bs=1 seek=100000 conv=notrunc 2> /dev/null
cp $FPREFIX-ref.lz4 $FPREFIX-trailing.lz4
printf 'trailing garbage' >> $FPREFIX-trailing.lz4

# compression
cp $FPREFIX-src $FPREFIX-c1
removed $FPREFIX-c1 "lz4 -q --rm -f $FPREFIX-c1 $FPREFIX-c1.lz4"
lz4 -dc $FPREFIX-c1.lz4 | cmp - $FPREFIX-src
cp $FPREFIX-src $FPREFIX-c2
removed $FPREFIX-c2 "lz4 -q --rm -f -l $FPREFIX-c2 $FPREFIX-c2.lz4"
lz4 -dc $FPREFIX-c2.lz4 | cmp - $FPREFIX-src
cp $FPREFIX-src $FPREFIX-c3a; cp $FPREFIX-src $FPREFIX-c3b
removed $FPREFIX-c3a "lz4 -q --rm -f -m $FPREFIX-c3a $FPREFIX-c3b"
test ! -e $FPREFIX-c3b
lz4 -dc $FPREFIX-c3b.lz4 | cmp - $FPREFIX-src
cp $FPREFIX-src $FPREFIX-c4
removed $FPREFIX-c4 "lz4 -q --rm -c $FPREFIX-c4 > $FPREFIX-c4.lz4"
lz4 -dc $FPREFIX-c4.lz4 | cmp - $FPREFIX-src
cp $FPREFIX-big $FPREFIX-c5
removed $FPREFIX-c5 "lz4 -q --rm -f -T4 $FPREFIX-c5 $FPREFIX-c5.lz4"
lz4 -dc $FPREFIX-c5.lz4 | cmp - $FPREFIX-big

# decompression
cp $FPREFIX-ref.lz4 $FPREFIX-d1.lz4
removed $FPREFIX-d1.lz4 "lz4 -q --rm -f -d $FPREFIX-d1.lz4 $FPREFIX-d1"
cmp $FPREFIX-d1 $FPREFIX-src
cp $FPREFIX-ref.lz4 $FPREFIX-d2a.lz4; cp $FPREFIX-ref.lz4 $FPREFIX-d2b.lz4
removed $FPREFIX-d2a.lz4 "lz4 -q --rm -f -d -m $FPREFIX-d2a.lz4 $FPREFIX-d2b.lz4"
test ! -e $FPREFIX-d2b.lz4
cmp $FPREFIX-d2b $FPREFIX-src
cp $FPREFIX-ref.lz4 $FPREFIX-d3.lz4
removed $FPREFIX-d3.lz4 "lz4 -q --rm -d -c $FPREFIX-d3.lz4 > $FPREFIX-d3"
cmp $FPREFIX-d3 $FPREFIX-src
cp $FPREFIX-ref.lz4 $FPREFIX-d4a.lz4; cp $FPREFIX-ref.lz4 $FPREFIX-d4b.lz4
removed $FPREFIX-d4a.lz4 "lz4 -q --rm -d -m -c $FPREFIX-d4a.lz4 $FPREFIX-d4b.lz4 > $FPREFIX-d4"
test ! -e $FPREFIX-d4b.lz4
cat $FPREFIX-src $FPREFIX-src | cmp - $FPREFIX-d4
cp $FPREFIX-legacy.lz4 $FPREFIX-d5.lz4
removed $FPREFIX-d5.lz4 "lz4 -q --rm -f -d $FPREFIX-d5.lz4 $FPREFIX-d5"
cmp $FPREFIX-d5 $FPREFIX-src

# test mode removes nothing
cp $FPREFIX-ref.lz4 $FPREFIX-t.lz4
lz4 -q -t --rm $FPREFIX-t.lz4
test -f $FPREFIX-t.lz4

# corrupted input
cp $FPREFIX-bad.lz4 $FPREFIX-e1.lz4
kept $FPREFIX-e1.lz4 "lz4 -q --rm -f -d $FPREFIX-e1.lz4 $FPREFIX-e1"
cp $FPREFIX-bad.lz4 $FPREFIX-e2.lz4
kept $FPREFIX-e2.lz4 "lz4 -q --rm -d -c $FPREFIX-e2.lz4 > $FPREFIX-e2"
cp $FPREFIX-bad.lz4 $FPREFIX-e3a.lz4; cp $FPREFIX-ref.lz4 $FPREFIX-e3b.lz4
kept $FPREFIX-e3a.lz4 "lz4 -q --rm -f -d -m $FPREFIX-e3a.lz4 $FPREFIX-e3b.lz4"
test -f $FPREFIX-e3b.lz4   # processing stops at the corrupted file
cp $FPREFIX-bad.lz4 $FPREFIX-e4a.lz4; cp $FPREFIX-ref.lz4 $FPREFIX-e4b.lz4
kept $FPREFIX-e4a.lz4 "lz4 -q --rm -d -m -c $FPREFIX-e4a.lz4 $FPREFIX-e4b.lz4 > $FPREFIX-e4"
test -f $FPREFIX-e4b.lz4
cp $FPREFIX-trailing.lz4 $FPREFIX-e5.lz4
kept $FPREFIX-e5.lz4 "lz4 -q --rm -f -d $FPREFIX-e5.lz4 $FPREFIX-e5"

# output not written : write errors, including those only reported when output is flushed or closed
if [ "$(uname)" = Linux ] && [ -w /dev/full ]; then
    cp $FPREFIX-small $FPREFIX-f1
    kept $FPREFIX-f1 "lz4 -q --rm -f $FPREFIX-f1 /dev/full"
    cp $FPREFIX-small $FPREFIX-f2
    kept $FPREFIX-f2 "lz4 -q --rm -f -l $FPREFIX-f2 /dev/full"
    cp $FPREFIX-small $FPREFIX-f3
    kept $FPREFIX-f3 "lz4 -q --rm -c $FPREFIX-f3 > /dev/full"
    cp $FPREFIX-big $FPREFIX-f4
    kept $FPREFIX-f4 "lz4 -q --rm -f -T4 $FPREFIX-f4 /dev/full"
    cp $FPREFIX-small.lz4 $FPREFIX-f5.lz4
    kept $FPREFIX-f5.lz4 "lz4 -q --rm -f -d $FPREFIX-f5.lz4 /dev/full"
    cp $FPREFIX-small.lz4 $FPREFIX-f6.lz4
    kept $FPREFIX-f6.lz4 "lz4 -q --rm -f -d --no-sparse $FPREFIX-f6.lz4 /dev/full"
    cp $FPREFIX-small.lz4 $FPREFIX-f7.lz4
    kept $FPREFIX-f7.lz4 "lz4 -q --rm -d -c $FPREFIX-f7.lz4 > /dev/full"
    cp $FPREFIX-small.lz4 $FPREFIX-f8a.lz4; cp $FPREFIX-small.lz4 $FPREFIX-f8b.lz4
    kept $FPREFIX-f8a.lz4 "lz4 -q --rm -d -m -c $FPREFIX-f8a.lz4 $FPREFIX-f8b.lz4 > /dev/full"
    test -f $FPREFIX-f8b.lz4
    cp $FPREFIX-ref.lz4 $FPREFIX-f9.lz4
    kept $FPREFIX-f9.lz4 "lz4 -q --rm -f -d $FPREFIX-f9.lz4 /dev/full"
fi

# source renamed and replaced (ex: log rotation), or modified, during its processing : --rm keeps the file at its name
# output goes into a fifo, read only once the source has changed : lz4 is blocked writing its first output
# (compression : see test-lz4-multithread.sh)
case "$(uname)" in MINGW*|MSYS*|CYGWIN*) exit 0 ;; esac
command -v mkfifo > /dev/null || exit 0

# --rm applies to a regular source only : a non-regular source (fifo, device, ...) is kept
rm -f $FPREFIX-fifo $FPREFIX-fifo.lz4
mkfifo $FPREFIX-fifo
printf 'fifo content' > $FPREFIX-fifo &
lz4 -q --rm -f $FPREFIX-fifo $FPREFIX-fifo.lz4
test -p $FPREFIX-fifo   # the fifo source is kept
rm -f $FPREFIX-fifo

lz4 -q -f $FPREFIX-big $FPREFIX-big.lz4
# decompressChanged MODIFICATION : decompresses $FPREFIX-rot.lz4 into $FPREFIX-rot with --rm
decompressChanged() {
    rm -f $FPREFIX-rot* $FPREFIX.fifo
    cp $FPREFIX-big.lz4 $FPREFIX-rot.lz4
    touch -t 200001010000 $FPREFIX-rot.lz4   # modification detected even with 1-second timestamps
    mkfifo $FPREFIX.fifo
    lz4 -q -f -d --no-sparse --rm $FPREFIX-rot.lz4 $FPREFIX.fifo &
    pid=$!
    { dd bs=4 count=1 2> /dev/null; $1 < /dev/null; cat; } < $FPREFIX.fifo > $FPREFIX-rot
    wait $pid
    cmp $FPREFIX-rot $FPREFIX-big
}
replaceSrc() { mv $FPREFIX-rot.lz4 $FPREFIX-rot.old; echo "new file" > $FPREFIX-rot.lz4; }
touchSrc()   { touch $FPREFIX-rot.lz4; }
decompressChanged replaceSrc
test "$(cat $FPREFIX-rot.lz4)" = "new file"
decompressChanged touchSrc
test -f $FPREFIX-rot.lz4
