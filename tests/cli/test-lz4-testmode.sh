#!/bin/sh

FPREFIX="tmp-ltm"

set -e

remove () {
    rm $FPREFIX*
}

trap remove EXIT

set -x

lz4 -bi0
datagen > $FPREFIX
lz4 -f $FPREFIX -c > $FPREFIX.lz4
lz4 -bdi0 $FPREFIX.lz4 # test benchmark decode-only mode
lz4 -bdi0 --no-crc $FPREFIX.lz4 # test benchmark decode-only mode
echo "---- benchmark : frames compressed as lz4 does, with the same options (content checksum : --frame-crc) ----"
datagen -g5MB -s5 > $FPREFIX-bench
datagen -g64KB -s7 > $FPREFIX-dict
for opts in "" "-T3" "-BD" "-B5 -BD -T2" "-9 -BD" "-B6 -BX --frame-crc" "--content-size" "-B2048" "-B100000 -BD" "-D $FPREFIX-dict" \
            "--single-thread" "--single-thread -BD" "--single-thread -B5 -BD -BX"
do
    crc="--no-frame-crc"; case "$opts" in *frame-crc*) crc="";; esac
    bsize=$(lz4 -bi0 -q $opts $FPREFIX-bench | awk '/^-/ {print $2}')
    lsize=$(lz4 -c $crc $opts $FPREFIX-bench | wc -c)
    test "$bsize" -eq "$lsize"
done
echo "---- test mode ----"
datagen | lz4 -t && exit 1
datagen | lz4 -tf && exit 1
echo "---- pass-through mode ----"
echo "Why hello there " > ${FPREFIX}2.lz4
lz4 -f ${FPREFIX}2.lz4 > $FPREFIX-trash && exit 1
datagen | lz4 -dc  > $FPREFIX-trash && exit 1
datagen | lz4 -df > $FPREFIX-trash && exit 1
datagen | lz4 -dcf > $FPREFIX-trash
echo "Hello World !" > ${FPREFIX}1
lz4 -dcf ${FPREFIX}1
echo "from underground..." > ${FPREFIX}2
lz4 -dcfm ${FPREFIX}1 ${FPREFIX}2
echo "---- non-existing source (must fail cleanly) ----"
lz4     file-does-not-exist && exit 1
lz4 -f  file-does-not-exist && exit 1
lz4 -t  file-does-not-exist && exit 1
lz4 -fm file1-dne file2-dne && exit 1
true
