#!/bin/sh

FPREFIX="tmp-tmt"

set -e

remove () {
    rm -rf $FPREFIX*
}

trap remove EXIT

set -x

datagen -g13M > $FPREFIX-src   # several jobs, last one incomplete

# standard block sizes : frames are cut into jobs (custom block sizes : see test-lz4-blocksize.sh)
# same output whatever the nb of threads, memory-mapped input or not, file or stdin, memory budget
for opts in "-1" "-1 -BD" "-9 -BD -BX" "-3 -B4 -BD" "-1 -B5" "--fast=3 -BD --no-frame-crc"
do
    lz4 -f -T1 $opts $FPREFIX-src $FPREFIX-T1.lz4
    lz4 -f -T4 $opts $FPREFIX-src $FPREFIX.lz4
    cmp $FPREFIX-T1.lz4 $FPREFIX.lz4
    lz4 -f -T0 $opts $FPREFIX-src $FPREFIX.lz4
    cmp $FPREFIX-T1.lz4 $FPREFIX.lz4
    lz4 -f -T3 --no-mmap $opts $FPREFIX-src $FPREFIX.lz4
    cmp $FPREFIX-T1.lz4 $FPREFIX.lz4
    lz4 -T2 $opts < $FPREFIX-src > $FPREFIX.lz4
    cmp $FPREFIX-T1.lz4 $FPREFIX.lz4
    lz4 -f --memory=50M $opts $FPREFIX-src $FPREFIX.lz4
    cmp $FPREFIX-T1.lz4 $FPREFIX.lz4
    lz4 -dc $FPREFIX-T1.lz4 | cmp - $FPREFIX-src
done

# memory budget : --memory=# and LZ4_MEMORY limit the nb of threads, not the output
lz4 -f --memory=abc $FPREFIX-src $FPREFIX.lz4 && exit 1  # must fail
lz4 -f --memory=0 $FPREFIX-src $FPREFIX.lz4 && exit 1    # must fail
lz4 -f --memory=18446744073709551616 $FPREFIX-src $FPREFIX.lz4 && exit 1  # must fail (overflow)
lz4 -f --memory=17179869184G $FPREFIX-src $FPREFIX.lz4 && exit 1          # must fail (overflow)
lz4 -f -1 $FPREFIX-src $FPREFIX-ref.lz4
MT=0; if lz4 -V 2>&1 | grep -q "multithread"; then MT=1; fi   # otherwise, -T# and memory budget are ignored
lz4 -v -f -T64 --memory=50M $FPREFIX-src $FPREFIX.lz4 2> $FPREFIX.err
cmp $FPREFIX-ref.lz4 $FPREFIX.lz4
if [ $MT = 1 ]; then grep "Memory budget (50 MB)" $FPREFIX.err; fi
LZ4_MEMORY=50MB lz4 -v -f -T64 $FPREFIX-src $FPREFIX.lz4 2> $FPREFIX.err
if [ $MT = 1 ]; then grep "Memory budget (50 MB)" $FPREFIX.err; fi
LZ4_MEMORY=2GiB lz4 -v -f -T64 --memory 50M $FPREFIX-src $FPREFIX.lz4 2> $FPREFIX.err   # command line wins
if [ $MT = 1 ]; then grep "Memory budget (50 MB)" $FPREFIX.err; fi
LZ4_MEMORY=abc lz4 -f $FPREFIX-src $FPREFIX.lz4 2>&1 | grep "LZ4_MEMORY=abc"   # ignored, with a warning
cmp $FPREFIX-ref.lz4 $FPREFIX.lz4
lz4 -f --memory=1M $FPREFIX-src $FPREFIX.lz4 2> $FPREFIX.err   # warning only
cmp $FPREFIX-ref.lz4 $FPREFIX.lz4
if [ $MT = 1 ]; then grep "too small" $FPREFIX.err; fi


# Source modified during its compression (see --[no-]mmap) :
# output goes into a fifo, read only once the source is modified :
# lz4 is then blocked writing its first output, after reading its first 4 MB chunk.
# -T1 : 3 job slots, so lz4 is blocked before reaching the end of the source.
case "$(uname)" in MINGW*|MSYS*|CYGWIN*) exit 0 ;; esac
command -v mkfifo > /dev/null || exit 0
MMAP=0; if lz4 -V 2>&1 | grep -q "64-bit"; then MMAP=1; fi   # memory-mapped input : POSIX 64-bit systems

rewriteStart() { printf 'modified' | dd of=$FPREFIX-mod bs=8 count=1 conv=notrunc 2> /dev/null; }
appendData()   { echo "appended" >> $FPREFIX-mod; }
replaceSrc()   { mv $FPREFIX-mod $FPREFIX-mod.old; echo "new file" > $FPREFIX-mod; }

# compressModified OPTIONS MODIFICATION : compresses $FPREFIX-mod into $FPREFIX.out, sets rc
compressModified() {
    rm -f $FPREFIX-mod* $FPREFIX.fifo $FPREFIX.out
    cp $FPREFIX-src $FPREFIX-mod
    touch -t 200001010000 $FPREFIX-mod   # modification detected even with 1-second timestamps
    mkfifo $FPREFIX.fifo
    lz4 -f -T1 $1 $FPREFIX-mod $FPREFIX.fifo 2> $FPREFIX.err &
    pid=$!
    { dd bs=4 count=1 2> /dev/null; $2 < /dev/null; cat; } < $FPREFIX.fifo > $FPREFIX.out
    rc=0; wait $pid || rc=$?
    cat $FPREFIX.err
}

# content already read is rewritten : memory-mapped input, read again, would be inconsistent (error 54) ;
# except on macOS, where a private mapping keeps the content as opened : then the output is valid
compressModified "" rewriteStart
if [ $MMAP = 1 ] && [ "$(uname)" != Darwin ]; then
    test $rc = 54
    grep -q "output removed" $FPREFIX.err
    test ! -e $FPREFIX.fifo
else
    test $rc = 0
    lz4 -dc $FPREFIX.out | cmp - $FPREFIX-src
fi
# input read once : the output holds the content read
compressModified "--no-mmap" rewriteStart
test $rc = 0
grep -q "Warning" $FPREFIX.err
lz4 -dc $FPREFIX.out | cmp - $FPREFIX-src
# --rm not honored : warning also displayed when notifications are lowered (stdout)
rm -f $FPREFIX-mod $FPREFIX.fifo; cp $FPREFIX-src $FPREFIX-mod; touch -t 200001010000 $FPREFIX-mod; mkfifo $FPREFIX.fifo
lz4 -c -T1 --no-mmap --rm $FPREFIX-mod > $FPREFIX.fifo 2> $FPREFIX.err &
pid=$!
{ dd bs=4 count=1 2> /dev/null; rewriteStart < /dev/null; cat; } < $FPREFIX.fifo > $FPREFIX.out
wait $pid
grep -q "source kept" $FPREFIX.err
test -f $FPREFIX-mod

# growing file : memory-mapped input holds the file as it was when opened, verified with the frame checksum ;
# input read once also holds the appended content (unless end of file was already met : libc dependent) ; --rm keeps the source
compressModified "--rm" appendData
test $rc = 0
grep -q "source kept" $FPREFIX.err
test -f $FPREFIX-mod
if [ $MMAP = 1 ]; then lz4 -dc $FPREFIX.out | cmp - $FPREFIX-src; fi
compressModified "--rm --no-mmap" appendData
test $rc = 0
test -f $FPREFIX-mod
lz4 -dc $FPREFIX.out > $FPREFIX.dec
cmp $FPREFIX.dec $FPREFIX-mod || cmp $FPREFIX.dec $FPREFIX-src
# without content checksum, memory-mapped input can't be verified
compressModified "--no-frame-crc" appendData
if [ $MMAP = 1 ]; then test $rc = 54; else test $rc = 0; fi

# source renamed and replaced (ex: log rotation) : --rm must not remove the new file
compressModified "--rm" replaceSrc
test $rc = 0
grep -q "renamed or replaced" $FPREFIX.err
test "$(cat $FPREFIX-mod)" = "new file"
test -f $FPREFIX-mod.old
lz4 -dc $FPREFIX.out | cmp - $FPREFIX-src
