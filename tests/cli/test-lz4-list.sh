#!/bin/sh

# --list : frames, types, blocks and sizes of each file, summarized, or detailed with -v
# compressed sizes depend on the platform : expected sizes are those of the files

FPREFIX="tmp-tll"

set -e
unset LZ4_CLEVEL LZ4_NBWORKERS LZ4_MEMORY   # set by the tests only

remove () {
    rm -rf $FPREFIX*
}

trap remove EXIT

set -x

# frames, each in its file
datagen -g300KB > $FPREFIX-src
datagen -g100KB -P0 > $FPREFIX-noise
datagen -g5M > $FPREFIX-big
C=$FPREFIX-c.lz4;  lz4 -q -B4 -BD --content-size $FPREFIX-src $C   # linked 64 KB blocks, content size
X=$FPREFIX-x.lz4;  lz4 -q -B4 -BX --no-frame-crc $FPREFIX-src $X   # block checksums, no content checksum
F=$FPREFIX-f.lz4;  lz4 -q -B5 $FPREFIX-src $F                      # 256 KB blocks
N=$FPREFIX-n.lz4;  lz4 -q -B4 $FPREFIX-noise $N                    # incompressible : 64 KB stored block
B=$FPREFIX-b.lz4;  lz4 -q --content-size $FPREFIX-big $B           # 4 MB blocks
L=$FPREFIX-l.lz4;  lz4 -q -l $FPREFIX-src $L                       # legacy frame
S=$FPREFIX-s.lz4;  printf '\137\052\115\030\004\000\000\000skip' > $S   # skippable frame (magic 0x184D2A5F)
ERR=$FPREFIX-err

# size FILE : in bytes ; human SIZE, ratio COMPRESSED UNCOMPRESSED : as displayed
size()  { wc -c < $1 | tr -d ' '; }
human() { awk "BEGIN { s = $1; split(\"K M G\", units); for (i = 1; s >= 1024; i++) { s /= 1024; u = units[i] }; printf \"%.2f%s\", s, u }"; }
ratio() { awk "BEGIN { printf \"%.2f%%\", $1 / $2 * 100 }"; }
# lists EXPECTED OPTIONS... : lz4 --list OPTIONS displays EXPECTED (fields separated by one space)
lists() {
    printf '%s\n' "$1" > $FPREFIX-expected; shift
    lz4 --list "$@" > $FPREFIX-out
    tr -d '\r' < $FPREFIX-out | sed 's/  */ /g; s/^ //; s/ $//' > $FPREFIX-got   # (\r : Windows)
    if ! cmp -s $FPREFIX-expected $FPREFIX-got; then cat $FPREFIX-out; diff $FPREFIX-expected $FPREFIX-got; return 1; fi
}
# fails COMMAND MESSAGE : COMMAND fails with an error code, not a crash, and displays MESSAGE ("" : nothing)
fails() {
    rc=0; sh -c "$1" > /dev/null 2> $ERR || rc=$?
    if [ $rc -gt 0 ] && [ $rc -lt 128 ]; then
        if [ -z "$2" ] && [ ! -s $ERR ]; then return 0; fi
        if [ -n "$2" ] && grep -q "$2" $ERR; then return 0; fi
    fi
    cat $ERR; return 1
}


# summary : one line per file, named without its path
T="Frames Type Block Compressed Uncompressed Ratio Filename"
lists "$T
1 LZ4Frame B4D $(human $(size $C)) 300.00K $(ratio $(size $C) 307200) $C
1 LZ4Frame B4I $(human $(size $X)) - - $X
1 LZ4Frame B5I $(human $(size $F)) - - $F
1 LZ4Frame B4I $(human $(size $N)) - - $N
1 LZ4Frame B7I $(human $(size $B)) 5.00M $(ratio $(size $B) 5242880) $B
1 LegacyFrame - $(human $(size $L)) - - $L
1 SkippableFrame - 12.00 - - $S" ./$C $X $F $N $B $L $S

# summary of several frames : their common type, block type and total content size, or -
cat $C $C > $FPREFIX-cc.lz4
cat $C $X > $FPREFIX-cx.lz4   # block modes differ ; a content size is unknown
cat $X $F > $FPREFIX-xf.lz4   # block sizes differ
cat $L $C > $FPREFIX-lc.lz4   # frame types differ
cat $C $L > $FPREFIX-cl.lz4
cat $C $S > $FPREFIX-cs.lz4
cat $L $L > $FPREFIX-ll.lz4
cat $S $S > $FPREFIX-ss.lz4
cat $C $X $L $S $F > $FPREFIX-all.lz4
lists "$T
2 LZ4Frame B4D $(human $(size $FPREFIX-cc.lz4)) 600.00K $(ratio $(size $FPREFIX-cc.lz4) 614400) $FPREFIX-cc.lz4
2 LZ4Frame - $(human $(size $FPREFIX-cx.lz4)) - - $FPREFIX-cx.lz4
2 LZ4Frame - $(human $(size $FPREFIX-xf.lz4)) - - $FPREFIX-xf.lz4
2 - - $(human $(size $FPREFIX-lc.lz4)) - - $FPREFIX-lc.lz4
2 - - $(human $(size $FPREFIX-cl.lz4)) - - $FPREFIX-cl.lz4
2 - - $(human $(size $FPREFIX-cs.lz4)) - - $FPREFIX-cs.lz4
2 LegacyFrame - $(human $(size $FPREFIX-ll.lz4)) - - $FPREFIX-ll.lz4
2 SkippableFrame - 24.00 - - $FPREFIX-ss.lz4
5 - - $(human $(size $FPREFIX-all.lz4)) - - $FPREFIX-all.lz4" \
    $FPREFIX-cc.lz4 $FPREFIX-cx.lz4 $FPREFIX-xf.lz4 $FPREFIX-lc.lz4 $FPREFIX-cl.lz4 $FPREFIX-cs.lz4 \
    $FPREFIX-ll.lz4 $FPREFIX-ss.lz4 $FPREFIX-all.lz4

# sizes : units from 1024
datagen -g1023 > $FPREFIX-1023;  lz4 -q --content-size $FPREFIX-1023 $FPREFIX-1023.lz4
datagen -g1024 > $FPREFIX-1024;  lz4 -q --content-size $FPREFIX-1024 $FPREFIX-1024.lz4
lists "$T
1 LZ4Frame B4I $(human $(size $FPREFIX-1023.lz4)) 1023.00 $(ratio $(size $FPREFIX-1023.lz4) 1023) $FPREFIX-1023.lz4
1 LZ4Frame B4I $(human $(size $FPREFIX-1024.lz4)) 1.00K $(ratio $(size $FPREFIX-1024.lz4) 1024) $FPREFIX-1024.lz4" \
    $FPREFIX-1023.lz4 $FPREFIX-1024.lz4

# -v : each file, its frames, and an empty line
H="Frame Type Block Checksum Compressed Uncompressed Ratio"
lists "$FPREFIX-all.lz4(1/2)
$H
1 LZ4Frame B4D XXH32 $(size $C) 307200 $(ratio $(size $C) 307200)
2 LZ4Frame B4I - $(size $X) - -
3 LegacyFrame - - $(size $L) - -
4 SkippableFrame - - 12 - -
5 LZ4Frame B5I XXH32 $(size $F) - -

$B(2/2)
$H
1 LZ4Frame B7I XXH32 $(size $B) 5242880 $(ratio $(size $B) 5242880)
" -v $FPREFIX-all.lz4 $B

# files are closed : more files than allowed open files (when this limit can be set)
files=""; n=0
while [ $n -lt 40 ]; do files="$files $C"; n=$((n + 1)); done
(if ulimit -n 20 2> /dev/null; then lz4 --list $files > /dev/null; fi)


# errors : reported, except with -qq
mkdir $FPREFIX-dir
fails "lz4 --list $FPREFIX-dir" "lz4: $FPREFIX-dir is not a regular file"
fails "lz4 -qq --list $FPREFIX-dir" ""
fails "lz4 --list $FPREFIX-src" "lz4: $FPREFIX-src: File format not recognized"
fails "lz4 -qq --list $FPREFIX-src" ""

# truncated or corrupted frames, alone or after a valid frame
head -c $(($(size $X) - 2)) $X > $FPREFIX-bad.lz4              # end mark cut
fails "lz4 --list $FPREFIX-bad.lz4" "File format not recognized"
{ cat $C; head -c 4 $C; } > $FPREFIX-bad.lz4                    # magic number only
fails "lz4 --list $FPREFIX-bad.lz4" "File format not recognized"
lz4 -q -B4 --no-frame-crc $FPREFIX-src $FPREFIX-y.lz4
{ cat $C; head -c 6 $FPREFIX-y.lz4; printf '\000'; tail -c +8 $FPREFIX-y.lz4; } > $FPREFIX-bad.lz4   # wrong header checksum (0x82)
fails "lz4 --list $FPREFIX-bad.lz4" "File format not recognized"
head -c $(($(size $L) - 1)) $L > $FPREFIX-bad.lz4
fails "lz4 --list $FPREFIX-bad.lz4" "Corrupted legacy frame"
fails "lz4 -qq --list $FPREFIX-bad.lz4" ""
# legacy blocks : up to LZ4_COMPRESSBOUND(8 MB) = 8421520 bytes, as accepted by the decoder (content not checked)
{ printf '\002\041\114\030\220\200\200\000'; datagen -g8421520; } > $FPREFIX-max.lz4
lz4 --list $FPREFIX-max.lz4
{ printf '\002\041\114\030\221\200\200\000'; datagen -g8421521; } > $FPREFIX-bad.lz4
fails "lz4 --list $FPREFIX-bad.lz4" "Corrupted legacy frame"
fails "lz4 -vv --list $FPREFIX-bad.lz4" "block in legacy frame is too large"

# undecodable data after a frame : position reported with -v
{ cat $C; printf 'garb'; cat $C; } > $FPREFIX-bad.lz4
fails "lz4 --list $FPREFIX-bad.lz4" "File format not recognized"
test "$(grep -c undecodable $ERR)" = 0
fails "lz4 -v --list $FPREFIX-bad.lz4" "undecodable data at position [0-9]* *$"
{ cat $C; printf 'ga'; } > $FPREFIX-bad.lz4
fails "lz4 --list $FPREFIX-bad.lz4" "Magic Number unreadable"
