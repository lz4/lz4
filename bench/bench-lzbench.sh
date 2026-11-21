#!/usr/bin/env bash
# bench-lzbench.sh : in-memory compression ratio and speed of LZ4 and other compressors, at many levels, with lzbench.
# Each compressor and level is measured alone, on a single thread : lzbench keeps the fastest of its iterations
# (at least 1 s of compression and 2 s of decompression, see -t).
# Speeds are in MB/s of original data, 1 MB = 1000000 bytes. Ratio = original size / compressed size.
#
# lzbench is downloaded at a fixed commit, and built with its own copies of the compressors, and its own options.
# On x86, lz4 also gets the JCC erratum mitigation of its own build (-J for all compressors or none).
# -l measures the lz4 library of a lz4 source tree, instead of lzbench's copy.
# Results : directory given by -o : lzbench.csv, env.txt (system, versions, options), readme-table.md, log.txt.
# Meant for Linux and macOS (bash 3.2) ; needs curl or wget, make (GNU), a C and a C++ compiler.

set -u
set -o pipefail

LZBENCH_COMMIT=83899784e083d011ebcf6ee9a9021f98707e1fe6   # lzbench 2.4.2
LZBENCH_URL=https://github.com/inikep/lzbench/archive/$LZBENCH_COMMIT.tar.gz
SILESIA_URL="https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip"
SILESIA_FILES="dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml"
CACHE_DIR=${XDG_CACHE_HOME:-$HOME/.cache}/lz4-bench
CODECS="LZ4 ZSTD ZLIB ZLIB_NG LZO SNAPPY LZF QUICKLZ"   # lzbench build : these compressors only

INPUT=""
LZ4_DIR=""
LZBENCH_DIR=""
MACHINE=""
TIMES="1,2"
JCC=lz4
OUTDIR=""
CC=${CC:-cc}
CXX=${CXX:-c++}

usage() {
    cat <<EOF
usage : $0 [options]
  -i FILE   input file (default : Silesia corpus as a tar, downloaded once into $CACHE_DIR)
  -m NAME   machine name, for the results (default : the cpu model)
  -l DIR    lz4 source tree whose library is measured (default : lzbench's copy of lz4)
  -L DIR    lzbench source tree to use (default : lzbench at commit ${LZBENCH_COMMIT:0:12}, downloaded once)
  -t C,D    minimum seconds of compression and decompression per measurement (default : $TIMES)
  -J MODE   JCC erratum mitigation (x86) : lz4 (as lz4's own build), all (all compressors), none (default : $JCC)
  -o DIR    output directory (default : ./bench-lzbench-HOST-DATE)
  -h        this help
Compilers : CC and CXX from the environment (default : cc and c++).
EOF
}

msg()  { printf '%s\n' "$*" >&2; }
warn() { printf 'warning : %s\n' "$*" >&2; }
die()  { printf 'error : %s\n' "$*" >&2; exit 1; }

while getopts "i:m:l:L:t:J:o:h" opt; do
    case $opt in
        i) INPUT=$OPTARG ;;
        m) MACHINE=$OPTARG ;;
        l) LZ4_DIR=$OPTARG ;;
        L) LZBENCH_DIR=$OPTARG ;;
        t) TIMES=$OPTARG ;;
        J) JCC=$OPTARG ;;
        o) OUTDIR=$OPTARG ;;
        h) usage; exit 0 ;;
        *) usage; exit 1 ;;
    esac
done
case $TIMES in [0-9]*,[0-9]*) ;; *) die "-t expects C,D, for instance 1,2" ;; esac
case $JCC in lz4|all|none) ;; *) die "-J expects lz4, all or none" ;; esac
[ -z "$LZ4_DIR" ] || [ -f "$LZ4_DIR/lib/lz4.h" ] || die "-l $LZ4_DIR : no lib/lz4.h, not a lz4 source tree"
[ -z "$LZBENCH_DIR" ] || [ -f "$LZBENCH_DIR/bench/lzbench.h" ] || die "-L $LZBENCH_DIR : not a lzbench source tree"

have() { command -v "$1" >/dev/null 2>&1; }
have "$CC" || die "C compiler $CC not found (set CC)"
have "$CXX" || die "C++ compiler $CXX not found (set CXX)"
MAKE=make; have gmake && MAKE=gmake
have "$MAKE" || die "make not found"
NPROC=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
JOBS=$(( NPROC < 16 ? NPROC : 16 ))   # build jobs : enough for this small build

[ -n "$OUTDIR" ] || OUTDIR=./bench-lzbench-$(hostname -s 2>/dev/null || echo host)-$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUTDIR" || die "cannot create $OUTDIR"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/bench-lzbench.XXXXXX") || die "cannot create a temporary directory"
trap 'rm -rf "$WORK"' EXIT
LOG=$WORK/log.txt
: > "$LOG"

download() {   # download URL FILE
    if have curl; then curl -fsSL --retry 3 -o "$2.part" "$1"
    elif have wget; then wget -q -O "$2.part" "$1"
    else warn "neither curl nor wget"; return 1
    fi && mv "$2.part" "$2"
}


# ===== input =====

get_silesia() {   # same tar as bench-pipeline.sh
    local tar=$CACHE_DIR/silesia.tar zip=$CACHE_DIR/silesia.zip dir=$CACHE_DIR/silesia sub
    INPUT=$tar
    [ -s "$tar" ] && return 0
    mkdir -p "$CACHE_DIR" || return 1
    if [ ! -s "$zip" ]; then
        msg "downloading $SILESIA_URL ..."
        download "$SILESIA_URL" "$zip" || return 1
    fi
    rm -rf "$dir" && mkdir -p "$dir" || return 1
    if have unzip; then unzip -q -o "$zip" -d "$dir"
    elif have bsdtar; then bsdtar -xf "$zip" -C "$dir"
    else warn "cannot unzip : no unzip nor bsdtar"; return 1
    fi || return 1
    sub=$(dirname "$(find "$dir" -name dickens -type f | head -1)")   # the files may be within a directory
    (cd "$sub" && tar -cf "$tar.part" $SILESIA_FILES) && mv "$tar.part" "$tar" && rm -rf "$dir"
}

if [ -z "$INPUT" ]; then
    get_silesia || die "cannot prepare the Silesia corpus : download $SILESIA_URL, make a tar of its files, and pass it with -i"
fi
[ -r "$INPUT" ] || die "$INPUT : cannot read"
SIZE=$(wc -c < "$INPUT" | tr -d ' ')
[ "$SIZE" -gt 0 ] || die "$INPUT : empty"


# ===== lzbench =====

SRC=$WORK/lzbench
mkdir -p "$SRC" || die "cannot create $SRC"
if [ -n "$LZBENCH_DIR" ]; then
    (cd "$LZBENCH_DIR" && tar -cf - --exclude=.git .) | (cd "$SRC" && tar -xf -) || die "cannot copy $LZBENCH_DIR"
    LZBENCH_SOURCE="$LZBENCH_DIR$(cd "$LZBENCH_DIR" && git rev-parse --short=12 HEAD 2>/dev/null | sed 's/^/, commit /')"
else
    TGZ=$CACHE_DIR/lzbench-$LZBENCH_COMMIT.tar.gz
    if [ ! -s "$TGZ" ]; then
        mkdir -p "$CACHE_DIR" || die "cannot create $CACHE_DIR"
        msg "downloading lzbench ${LZBENCH_COMMIT:0:12} ..."
        download "$LZBENCH_URL" "$TGZ" || die "cannot download $LZBENCH_URL"
    fi
    tar -xzf "$TGZ" -C "$SRC" --strip-components=1 || die "cannot extract $TGZ"
    LZBENCH_SOURCE="commit $LZBENCH_COMMIT"
fi

LZ4_SOURCE="lzbench's copy"
if [ -n "$LZ4_DIR" ]; then   # replace lzbench's copy of the lz4 library, file by file
    for f in "$SRC"/lz/lz4/lib/*.[ch]; do
        [ -f "$LZ4_DIR/lib/${f##*/}" ] || die "-l $LZ4_DIR : no lib/${f##*/}"
        cp "$LZ4_DIR/lib/${f##*/}" "$f" || die "cannot copy lib/${f##*/}"
    done
    LZ4_SOURCE="$LZ4_DIR$(cd "$LZ4_DIR" && git describe --tags --always --dirty 2>/dev/null | sed 's/^/, /')"
fi
LZ4_VERSION=$(awk '/#define LZ4_VERSION_(MAJOR|MINOR|RELEASE)[ \t]/ { v[$2] = $3 }
                   END { printf "%s.%s.%s", v["LZ4_VERSION_MAJOR"], v["LZ4_VERSION_MINOR"], v["LZ4_VERSION_RELEASE"] }' \
              "$SRC/lz/lz4/lib/lz4.h")

jcc_flag() {   # jcc_flag COMPILER LANGUAGE : option keeping branches within 32-byte boundaries, as lz4's build (x86 only)
    local f
    printf '#if !defined(__x86_64__) && !defined(__i386__)\n#error not x86\n#endif\nint f(int x) { return x ? 1 : 2; }\n' > "$WORK/jcc.c"
    for f in -mbranches-within-32B-boundaries -Wa,-mbranches-within-32B-boundaries; do
        "$1" -x "$2" $f -Werror -c "$WORK/jcc.c" -o "$WORK/jcc.o" 2>/dev/null && { echo "$f"; return 0; }
    done
}
JCC_C=""; JCC_CXX=""; JCC_LZ4=""
case $JCC in
    all) JCC_C=$(jcc_flag "$CC" c); JCC_CXX=$(jcc_flag "$CXX" c++) ;;
    lz4) JCC_LZ4=$(jcc_flag "$CC" c)
         [ -z "$JCC_LZ4" ] || echo "LZ4_FLAGS += $JCC_LZ4" >> "$SRC/mk/lz4.mk" ;;   # lz4 objects only
esac
case $JCC in
    all) JCC_INFO="all compressors : ${JCC_C:-none} (C), ${JCC_CXX:-none} (C++)" ;;
    lz4) JCC_INFO="lz4 only : ${JCC_LZ4:-none}" ;;
    *)   JCC_INFO="none" ;;
esac

msg "building lzbench ..."
"$MAKE" -C "$SRC" -j"$JOBS" CC="$CC" CXX="$CXX" BUILD_ONLY="$CODECS" \
        USER_CFLAGS="$JCC_C" USER_CXXFLAGS="$JCC_CXX" >>"$LOG" 2>&1 \
    || { cp "$LOG" "$OUTDIR/log.txt"; die "cannot build lzbench, see $OUTDIR/log.txt"; }
LZBENCH=$SRC/lzbench
[ -x "$LZBENCH" ] || die "lzbench not built"


# ===== compressors : family | lzbench codec | lzbench level | level | name =====
# level orders a family on one axis : fast modes get negative levels, as zstd's --fast.

ENTRIES=()
add() { ENTRIES+=("$1|$2|$3|$4|$5"); }
add memcpy memcpy "" 0 memcpy
for a in 64 32 24 17 12 8 6 4 3 2; do add LZ4 lz4fast $a -$a "lz4 --fast=$a"; done
add LZ4 lz4 "" 1 "lz4 -1"
for l in 2 3 4 5 6 7 8 9 10 11 12; do add LZ4 lz4hc $l $l "lz4 -$l"; done
for a in 5 4 3 2 1; do add Zstandard zstd_fast -$a -$a "zstd --fast=$a"; done
for l in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19; do add Zstandard zstd $l $l "zstd -$l"; done
for l in 1 2 3 4 5 6 7 8 9; do add zlib zlib $l $l "zlib -$l"; done
for l in 1 2 3 4 5 6 7 8 9; do add zlib-ng zlib-ng $l $l "zlib-ng -$l"; done
add LZO lzo1x 1 1 "lzo1x -1"
add LZO lzo1x 999 999 "lzo1x -999"
add Snappy snappy "" 0 snappy
add LZF lzf 0 0 "lzf -0"
add LZF lzf 1 1 "lzf -1"
for l in 1 2 3; do add QuickLZ quicklz $l $l "quicklz -$l"; done


# ===== environment =====

case $(uname -s) in
    Darwin)
        CPU=$(sysctl -n machdep.cpu.brand_string 2>/dev/null)
        MEMORY="$(( $(sysctl -n hw.memsize) / 1073741824 )) GB"
        SYSTEM="macOS $(sw_vers -productVersion 2>/dev/null), $(uname -m)"
        GOVERNOR="" ;;
    *)
        CPU=$( (lscpu 2>/dev/null | sed -n 's/^Model name: *//p'; sed -n 's/^model name[^:]*: //p' /proc/cpuinfo 2>/dev/null) | head -1)
        MEMORY=$(awk '/^MemTotal:/ { printf "%d GB", $2 / 1048576 + 0.5 }' /proc/meminfo 2>/dev/null)
        SYSTEM="$( (. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME") || uname -s), kernel $(uname -r), $(uname -m)"
        GOVERNOR=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null) ;;
esac
[ -n "$MACHINE" ] || MACHINE=${CPU:-unknown}
DATE=$(date -u '+%Y-%m-%d')

{
    echo "machine   : $MACHINE"
    echo "date      : $DATE"
    echo "cpu       : ${CPU:-unknown}, $NPROC threads"
    echo "memory    : ${MEMORY:-unknown}"
    echo "system    : $SYSTEM"
    [ -n "$GOVERNOR" ] && echo "governor  : $GOVERNOR"
    echo "compilers : $("$CC" --version 2>&1 | head -1) ; $("$CXX" --version 2>&1 | head -1)"
    echo "jcc       : $JCC_INFO"
    echo "lzbench   : $("$LZBENCH" -V 2>&1 | head -1), $LZBENCH_SOURCE"
    echo "lz4       : $LZ4_VERSION, $LZ4_SOURCE"
    echo "input     : ${INPUT##*/}, $SIZE bytes"
    echo "times     : $TIMES (minimum seconds of compression, decompression), fastest iteration, single thread"
} | tee "$OUTDIR/env.txt" >&2


# ===== measurements =====

echo "family,name,codec,level,version,ratio,compression_MBps,decompression_MBps,compressed_bytes" > "$OUTDIR/lzbench.csv"
n=${#ENTRIES[@]}; i=0
for entry in "${ENTRIES[@]}"; do
    i=$((i + 1))
    IFS='|' read -r family codec lzlevel level name <<EOF
$entry
EOF
    # lzbench -o4 row : name with version and level, compression MB/s, decompression MB/s, original size, compressed size, ...
    row=$("$LZBENCH" -q -o4 -t"$TIMES" -e"$codec${lzlevel:+,$lzlevel}" "$INPUT" 2>"$WORK/err" | sed -n 2p)
    cat "$WORK/err" >> "$LOG"
    IFS=, read -r lzname cspeed dspeed orig csize rest <<EOF
$row
EOF
    err=$(tr '\r' '\n' < "$WORK/err" | grep . | head -1)   # lzbench -q is silent, unless something goes wrong
    if grep -q ERROR "$WORK/err" || [ -z "$row" ] || [ "${orig:-0}" != "$SIZE" ] || [ "${csize:-0}" -le 0 ] 2>/dev/null; then
        # for instance "ERROR in zstd: decompressed bytes common=..." : the decompressed data differ from the input
        warn "$name : skipped${err:+ : $err}"
        echo "$name : $row" >> "$LOG"; continue
    fi
    [ -z "$err" ] || warn "$name : $err"
    if [ "$family" = LZ4 ]; then version=$LZ4_VERSION
    else version=$(printf '%s\n' "$lzname" | sed -E 's/^[^ ]+ ?//; s/ (--fast )?-+[0-9]+$//')
    fi
    ratio=$(awk -v o="$orig" -v c="$csize" 'BEGIN { printf "%.3f", o / c }')
    echo "$family,$name,$codec,$level,$version,$ratio,$cspeed,$dspeed,$csize" >> "$OUTDIR/lzbench.csv"
    msg "[$i/$n] $name : ratio $ratio, compression $cspeed MB/s, decompression $dspeed MB/s"
done
cp "$LOG" "$OUTDIR/log.txt"


# ===== README table : rows of lz4's README, from these results =====

awk -F, '
    function sig3(x,   e) { if (x < 100) return sprintf("%d", x + 0.5)   # 3 significant digits
                            e = 10 ^ (int(log(x) / log(10)) - 2); return sprintf("%d", int(x / e + 0.5) * e) }
    NR == 1 { next }
    { label = ""; bold = 0 }
    $2 == "memcpy"     { label = "memcpy" }
    $2 == "lz4 -1"     { label = "**LZ4 default (v" $5 ")**"; bold = 1 }
    $2 == "lzo1x -1"   { label = "LZO " $5 " -1" }
    $2 == "quicklz -1" { v = $5; sub(/ beta /, "b", v); label = "QuickLZ " v " -1" }
    $2 == "snappy"     { label = "Snappy " $5 }
    $2 == "zstd -1"    { label = "[Zstandard] " $5 " -1" }
    $2 == "lzf -1"     { label = "LZF v" $5 }
    $2 == "zlib-ng -1" { label = "zlib-ng " $5 " -1" }
    $2 == "zlib -1"    { label = "[zlib] deflate " $5 " -1" }
    $2 == "lz4 -9"     { label = "**LZ4 HC -9 (v" $5 ")**"; bold = 2 }
    $2 == "zlib -6"    { label = "[zlib] deflate " $5 " -6" }
    label != "" {
        r = $6; c = sig3($7) " MB/s"; d = sig3($8) " MB/s"
        if (bold == 1) { r = "**" r "**"; c = "**" c "**"; d = "**" d "**" }
        if (bold == 2) { d = "**" d "**" }
        printf "%s|%s|%s|%s|%s\n", ($2 == "memcpy") ? 1e9 : $7, label, r, c, d
    }' "$OUTDIR/lzbench.csv" | sort -t'|' -k1,1gr |
awk -F'|' 'BEGIN { print "|  Compressor             | Factor  | Compression | Decompression |"
                   print "|  ----------             | -----   | ----------- | ------------- |" }
           { printf "| %-23s | %7s | %11s | %13s |\n", $2, $3, $4, $5 }' > "$OUTDIR/readme-table.md"

echo
column -s, -t < "$OUTDIR/lzbench.csv" 2>/dev/null || cat "$OUTDIR/lzbench.csv"
echo
cat "$OUTDIR/readme-table.md"
echo
echo "results in $OUTDIR"
