#!/bin/sh

# command line options : each one has its documented effect, observed in the output
# (frame header, compressed data, files kept or overwritten, messages)
# sections are independent : they only read the inputs created below

FPREFIX="tmp-tlo"

set -e
unset LZ4_CLEVEL LZ4_NBWORKERS LZ4_MEMORY   # set by the tests only

remove () {
    rm -rf $FPREFIX* null
}

trap remove EXIT

set -x

# inputs
SRC=$FPREFIX-src;      datagen -g20KB > $SRC        # a single block
HDR=$FPREFIX-hdr;      datagen -g300KB > $HDR       # several 64 KB blocks
BIG=$FPREFIX-big;      datagen -g5M -P70 > $BIG     # several 4 MB blocks
REF=$FPREFIX-ref.lz4;  lz4 -q -f $SRC $REF
BAD=$FPREFIX-bad.lz4;  cp $REF $BAD                 # wrong content checksum
printf '\377' | dd of=$BAD bs=1 seek=$(($(wc -c < $REF) - 1)) conv=notrunc 2> /dev/null
mkdir $FPREFIX-bin   # lz4c : legacy commands, selected by the name of the tested executable
LZ4=$(command -v lz4)
cp "$LZ4" "$FPREFIX-bin/lz4c${LZ4##*/lz4}"
LZ4C=$FPREFIX-bin/lz4c
MT=0; if lz4 -V 2>&1 | grep -q "multithread"; then MT=1; fi   # otherwise, threads and memory budget are ignored
OUT=$FPREFIX.lz4
ERR=$FPREFIX-err

# fails COMMAND : COMMAND fails with an error code, not a crash ; its output is shown otherwise
fails() {
    rc=0; sh -c "$1" > $FPREFIX-fails 2>&1 || rc=$?
    if [ $rc -gt 0 ] && [ $rc -lt 128 ]; then return 0; fi
    cat $FPREFIX-fails; return 1
}
# rejected OPTIONS..., accepted OPTIONS... : compression of $SRC with each OPTIONS
rejected() { for o in "$@"; do fails "lz4 -q -f $o $SRC $OUT"; done; }
accepted() { for o in "$@"; do lz4 -q -f $o $SRC $OUT; lz4 -dc $OUT | cmp - $SRC; done; }
# same INPUT COMMAND... : all commands compress INPUT into the same frame
same() {
    input=$1; shift
    sh -c "$1 -q -f $input $FPREFIX-1.lz4"; shift
    for c in "$@"; do sh -c "$c -q -f $input $FPREFIX-2.lz4"; cmp $FPREFIX-1.lz4 $FPREFIX-2.lz4; done
}
# header OPTIONS FLG : FLG byte of the frame header (version 0x40, B.Indep 0x20, B.Checksum 0x10, C.Size 0x08, C.Checksum 0x04)
header() { lz4 -q -f $1 $HDR $OUT; test "$(od -An -tx1 -j4 -N1 $OUT | tr -d ' \n')" = $2; }
# blockSizeID OPTIONS BD [INPUT] : BD byte of the frame header (block size ID << 4)
blockSizeID() { lz4 -q -f $1 ${3:-$BIG} $OUT; test "$(od -An -tx1 -j5 -N1 $OUT | tr -d ' \n')" = $2; }
# warns WORD COMMAND, warnsNot WORD COMMAND, silent COMMAND : messages of COMMAND (in a file : a pipe can cut them)
warns()    { sh -c "$2" > $ERR 2>&1; if ! grep -q "$1" $ERR; then cat $ERR; return 1; fi; }
warnsNot() { sh -c "$2" > $ERR 2>&1; if grep -q "$1" $ERR; then cat $ERR; return 1; fi; }
silent()   { sh -c "$1" 2> $ERR; if [ -s $ERR ]; then cat $ERR; return 1; fi; }


# frame header
header "" 64
header "--no-frame-crc" 60
header "--no-frame-crc --frame-crc" 64
header "-BX" 74
header "-BX --no-crc" 60
header "-B4 -BD" 44   # linked blocks : needs several blocks
header "-B4DX" 54
header "-B4 -BD -BI" 64
header "--content-size" 6c
header "--content-size --no-content-size" 64
blockSizeID "" 40 $SRC   # small input : 64 KB blocks
blockSizeID "" 70
for B in 4 5 6 7; do blockSizeID -B$B ${B}0; done
blockSizeID "-B4v" 40    # block properties end at another command
rejected "-B0" "-B3" "-B8" "-B9"   # block size IDs : 4 to 7 ; custom block sizes : 32 bytes or more
accepted "-B32"

# content checksum : verified, unless --no-crc
fails "lz4 -dc $BAD"
lz4 -dc --no-crc $BAD | cmp - $SRC

# numeric values : suffixes K, KB, KiB, M, MB, MiB, and G, GB, GiB for --memory ; limits (32-bit, 64-bit)
same $SRC "lz4 -B65536" "lz4 -B64K" "lz4 -B64KB" "lz4 -B64KiB"
same $BIG "lz4 -B1048576" "lz4 -B1M" "lz4 -B1MB" "lz4 -B1MiB"
accepted "-T4294967295" "-T4194303K" "-T4095M" "--threads=1KiB" "--threads=1KB"   # capped
rejected "-T4294967296" "-T4194304K" "-T4096M"
accepted "--memory=18446744073709551615" "--memory=18014398509481983K" "--memory=17179869183G" "--memory=1GiB" "--memory 1GB"
rejected "--memory=18446744073709551616" "--memory=18014398509481984K" "--memory=17179869184G"
rejected "--memory=0" "--memory=10Q" "--memory" "--memory -q"   # no argument : a file name, or another command
fails "lz4 -q -f $SRC $OUT --memory"

# memory budget : the same value, whatever its suffix or origin, gives the same nb of threads
if [ $MT = 1 ]; then
    # budget FILE COMMAND : 64 threads need much more than 50 MB (4 MB jobs) : COMMAND reduces them
    budget() { warns "budget" "$2 -f -T64 $SRC $OUT"; grep "budget" $ERR > $1; }
    budget $FPREFIX-b50 "lz4 --memory=50M"
    for c in "lz4 --memory=51200K" "lz4 --memory=51200KB" "lz4 --memory=52428800" "lz4 --memory=50MB" \
             "lz4 --memory=50MiB" "LZ4_MEMORY=50M lz4"; do
        budget $FPREFIX-b "$c";  cmp $FPREFIX-b $FPREFIX-b50
    done
    budget $FPREFIX-b49 "lz4 --memory=49M"
    budget $FPREFIX-b "lz4 --memory=51380224";  cmp $FPREFIX-b $FPREFIX-b49
fi
warnsNot "budget" "lz4 -f $SRC $OUT"   # the default budget is not reported

# environment variables : invalid values are ignored, with a warning ; the command line wins
for v in LZ4_MEMORY=0 LZ4_MEMORY=10Q LZ4_NBWORKERS=9x LZ4_CLEVEL=9x; do warns "Ignore" "$v lz4 -f $SRC $OUT"; done
for v in LZ4_MEMORY=50M LZ4_MEMORY=1 LZ4_NBWORKERS=0 LZ4_NBWORKERS=9; do warnsNot "Ignore" "$v lz4 -f $SRC $OUT"; done
same $SRC "lz4" "LZ4_CLEVEL=9x lz4"
same $SRC "lz4 -9" "LZ4_CLEVEL=9 lz4" "LZ4_CLEVEL=09 lz4"
same $SRC "lz4 -12" "LZ4_CLEVEL=2147483647 lz4"   # largest int : maximum level
same $SRC "lz4 -1" "LZ4_CLEVEL=9 lz4 -1"

# threads : more than possible are reported ; any nb gives the same frame, even with linked blocks
if [ $MT = 1 ]; then
    warns "reduced" "lz4 -f -T4294967295 $SRC $OUT"
    warns "reduced" "lz4 -b1 -i0 -T4294967295 $SRC"
    warnsNot "multithreading" "lz4 -f -T2 $SRC $OUT"
else
    warns "multithreading" "lz4 -f -T2 $SRC $OUT"   # not supported
fi
warnsNot "reduced" "lz4 -f -T1 $SRC $OUT"
warnsNot "reduced" "lz4 -b1 -i0 -T1 $SRC"
same $BIG "lz4 -BD -T1" "lz4 -BD" "lz4 -BD --threads 2"
lz4 -q -f -BD --single-thread $BIG $OUT
lz4 -dc $OUT | cmp - $BIG

# compression levels : --best, and lz4c's legacy commands
same $BIG "lz4 -0" "$LZ4C -c0"
same $BIG "lz4 -9" "$LZ4C -c1"
same $BIG "lz4 -12" "$LZ4C -c2" "$LZ4C -hc" "lz4 --best"
accepted "-12 --favor-decSpeed"

# existing output : overwritten only with -f, --force, lz4c's -y, or an interactive 'y'
cp $SRC $FPREFIX-out
fails "lz4 -q $SRC $FPREFIX-out"
fails "echo n | lz4 $SRC $FPREFIX-out"
fails "lz4 -q -f --no-force $SRC $FPREFIX-out"
cmp $FPREFIX-out $SRC
lz4 -q --force $SRC $FPREFIX-out;  cmp $FPREFIX-out $REF
cp $SRC $FPREFIX-out
echo y | lz4 $SRC $FPREFIX-out;    cmp $FPREFIX-out $REF
cp $SRC $FPREFIX-out
$LZ4C -q -y $SRC $FPREFIX-out;     cmp $FPREFIX-out $REF

# source file : removed with --rm, kept with -k / --keep : the last one wins
cp $SRC $FPREFIX-k
lz4 -q -f --rm -k $FPREFIX-k $OUT;      test -f $FPREFIX-k
lz4 -q -f --rm --keep $FPREFIX-k $OUT;  test -f $FPREFIX-k
lz4 -q -f -k --rm $FPREFIX-k $OUT;      test ! -e $FPREFIX-k

# input and output names
cat $SRC | lz4 -q -f - $OUT;  cmp $OUT $REF
for c in "$SRC -" "--to-stdout $SRC" "--stdout $SRC"; do lz4 -q $c > $OUT; cmp $OUT $REF; done
cp $REF $FPREFIX.dat   # decompressed whatever its name
for d in --decompress --uncompress -d; do lz4 -q -f $d $FPREFIX.dat $FPREFIX-dec; cmp $FPREFIX-dec $SRC; done
lz4 -q -f $SRC null   # "null" : no output file
test ! -e null
lz4 -q -f $SRC $OUT $FPREFIX-extra   # unused 3rd name : ignored with -f, error otherwise
fails "lz4 -q $SRC $FPREFIX-out3 $FPREFIX-extra"
cp $SRC $FPREFIX-dict   # the source itself : its frame refers to the dictionary
lz4 -q -f -D $FPREFIX-dict $SRC $OUT
lz4 -dc -D$FPREFIX-dict $OUT | cmp - $SRC
fails "lz4 -dc $OUT"
fails "lz4 -q -f $SRC $OUT -D"   # missing dictionary

# multiple files : -m / --multiple, --list, lz4cat (decodes all to stdout, silently) ; directories skipped
cp $SRC $FPREFIX-m1
cp $SRC $FPREFIX-m2
lz4 -q -f --multiple $FPREFIX-m1 $FPREFIX-m2
cmp $FPREFIX-m1.lz4 $REF
cmp $FPREFIX-m2.lz4 $REF
lz4 --list $FPREFIX-m1.lz4 $FPREFIX-m2.lz4 > $FPREFIX-list
grep -q $FPREFIX-m1.lz4 $FPREFIX-list
grep -q $FPREFIX-m2.lz4 $FPREFIX-list
silent "lz4cat $FPREFIX-m1.lz4 $FPREFIX-m2.lz4 > $FPREFIX-cat"
cat $SRC $SRC | cmp - $FPREFIX-cat
fails "lz4cat $BAD > /dev/null 2> $ERR"   # errors are reported
test -s $ERR
mkdir $FPREFIX-dir
cp $SRC $FPREFIX-dir/f
fails "lz4 -q -f -m $FPREFIX-dir"
test ! -e $FPREFIX-dir/f.lz4

# messages : a summary on the console ; nothing with --quiet, to stdout, or for multiple files unless -v
warns  "Compressed" "lz4 -f $SRC $OUT"
silent "lz4 -f --quiet $SRC $OUT"
silent "lz4 -qqqq -f $SRC $OUT"
silent "lz4 -c $SRC > $OUT"
silent "cat $SRC | lz4 > $OUT"
silent "lz4 -f -m $SRC $HDR"
warns  "Compressed" "lz4 -v -f -m $SRC $HDR"
warns  "content-size" "cat $SRC | lz4 --content-size > $OUT"   # size unknown
lz4 -V > /dev/null   # version and help succeed
lz4 -h > /dev/null

# benchmark : one result per level of the range, per file with -S ; decompression with -d
# results N ARGUMENTS : the benchmark displays N results, each ending with a line "level#"
# (not the other lines : their nb, and their marks, depend on the nb of measurement loops)
results() { n=$1; shift; lz4 -i0 "$@" > $ERR 2>&1; test "$(tr '\r' '\n' < $ERR | grep -c '^ *[0-9][0-9]*#$')" = $n; }
results 3 -b1e3 $SRC
results 2 -b1 -S $SRC $HDR
results 1 -b1 $SRC $HDR
warns "Decompression" "lz4 -b -d -i0 $REF"
warns "Decompression" "lz4 -b --decompress -i0 $REF"
