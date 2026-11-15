#!/usr/bin/env bash
# bench-pipeline.sh : effective speed of compression command lines, within pipelines
# where a link of limited speed (storage device, network) separates stages running at the same time :
#   read     : throttle LINK < file.compressed | decompress > /dev/null
#   transfer : compress < file | throttle LINK | decompress > /dev/null
# Effective speed = size of the original file / elapsed time, 1 MB = 1000000 bytes.
# Compression uses a single worker thread (lz4 -T1, zstd -T1).
#
# Missing compressors are skipped. Results : directory given by -o, summary on stdout.

set -u
set -o pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_DIR=$(cd "$SCRIPT_DIR/../../.." && pwd)

RUNS=5
MAX_TIME=20   # seconds of runs per measurement : fewer runs when they are long
READ_LINKS="HDD:200 SATA_SSD:550 NVMe_PCIe_3.0_x4:3500"
TRANSFER_LINKS="Internet_100_Mbit/s:11.7 1_GbE:117 2.5_GbE:295 5_GbE:590 10_GbE:1170"
INPUT=""
LZ4=""
OUTDIR=""
KEEP=0
CACHE_DIR=${XDG_CACHE_HOME:-$HOME/.cache}/lz4-bench-pipeline
SILESIA_URL="https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip"
SILESIA_FILES="dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml"

usage() {
    cat <<EOF
usage : $0 [options]
  -i FILE   input file (default : Silesia corpus as a tar, downloaded once into $CACHE_DIR)
  -n #      runs per measurement, the median is kept (default : $RUNS, fewer beyond ${MAX_TIME}s of runs)
  -r LIST   read scenario links, name:MB/s (default : "$READ_LINKS")
  -t LIST   transfer scenario links, name:MB/s (default : "$TRANSFER_LINKS")
  -l PATH   lz4 binary to use (default : built from this repository)
  -o DIR    output directory (default : ./bench-pipeline-HOST-DATE)
  -k        keep compressed files
  -h        this help
EOF
}

msg()  { printf '%s\n' "$*" >&2; }
warn() { printf 'warning : %s\n' "$*" >&2; }
die()  { printf 'error : %s\n' "$*" >&2; exit 1; }

while getopts "i:n:r:t:l:o:kh" opt; do
    case $opt in
        i) INPUT=$OPTARG ;;
        n) RUNS=$OPTARG ;;
        r) READ_LINKS=$OPTARG ;;
        t) TRANSFER_LINKS=$OPTARG ;;
        l) LZ4=$OPTARG ;;
        o) OUTDIR=$OPTARG ;;
        k) KEEP=1 ;;
        h) usage; exit 0 ;;
        *) usage; exit 1 ;;
    esac
done

have() { command -v "$1" >/dev/null 2>&1; }

# install hints, per distribution
. /etc/os-release 2>/dev/null
case " ${ID:-} ${ID_LIKE:-} " in
    *" debian "*|*" ubuntu "*) INSTALL="sudo apt install" ; BUILD_PKGS="build-essential" ;;
    *" fedora "*|*" rhel "*)   INSTALL="sudo dnf install" ; BUILD_PKGS="gcc make" ;;
    *" arch "*)                INSTALL="sudo pacman -S"   ; BUILD_PKGS="base-devel" ;;
    *" suse "*|*" opensuse "*) INSTALL="sudo zypper install" ; BUILD_PKGS="gcc make" ;;
    *)                         INSTALL="install"          ; BUILD_PKGS="a C compiler and make" ;;
esac

# clock : elapsed times from bash's EPOCHREALTIME (bash 5), or date
if [ -n "${EPOCHREALTIME:-}" ]; then
    clock() { CLOCK=${EPOCHREALTIME/,/.}; }
else
    clock() { CLOCK=$(date +%s.%N); }
fi
calc() { awk "BEGIN { printf \"%.${2:-3}f\", $1 }"; }


# ===== tools : build tools, throttle, lz4 =====

have cc || have gcc || die "no C compiler, needed to build throttle and lz4 : $INSTALL $BUILD_PKGS"
have make || die "make is needed to build lz4 : $INSTALL $BUILD_PKGS"
CC=$(have cc && echo cc || echo gcc)

WORK=$(mktemp -d "${TMPDIR:-/tmp}/bench-pipeline.XXXXXX") || die "cannot create a temporary directory"
trap '[ "$KEEP" = 1 ] || rm -rf "$WORK"' EXIT
LOG=$WORK/log.txt

$CC -O2 -o "$WORK/throttle" "$SCRIPT_DIR/throttle.c" 2>>"$LOG" || die "cannot build throttle, see $LOG"
THROTTLE=$WORK/throttle

if [ -z "$LZ4" ]; then
    JOBS=$(nproc 2>/dev/null || echo 4); [ "$JOBS" -gt 16 ] && JOBS=16
    msg "building lz4 from $REPO_DIR ..."
    if make -C "$REPO_DIR/programs" -j"$JOBS" lz4 >>"$LOG" 2>&1; then
        LZ4=$REPO_DIR/programs/lz4
    elif have lz4; then
        LZ4=$(command -v lz4); warn "cannot build lz4 (see $LOG), using $LZ4"
    else
        die "cannot build lz4, see $LOG"
    fi
fi
[ -x "$LZ4" ] || die "$LZ4 : not an executable"
LZ4_T1="-T1"
"$LZ4" -T1 -c </dev/null >/dev/null 2>&1 || { LZ4_T1=""; warn "$LZ4 doesn't support -T# : compression is single-threaded anyway"; }


# ===== input =====

get_silesia() {
    local tar=$CACHE_DIR/silesia.tar zip=$CACHE_DIR/silesia.zip dir=$CACHE_DIR/silesia
    INPUT=$tar
    [ -s "$tar" ] && return 0
    mkdir -p "$CACHE_DIR" || return 1
    if [ ! -s "$zip" ]; then
        msg "downloading $SILESIA_URL ..."
        if have curl; then curl -fsSL --retry 3 -o "$zip.part" "$SILESIA_URL"
        elif have wget; then wget -q -O "$zip.part" "$SILESIA_URL"
        else warn "neither curl nor wget : $INSTALL curl"; return 1
        fi || return 1
        mv "$zip.part" "$zip"
    fi
    rm -rf "$dir" && mkdir -p "$dir" || return 1
    if have unzip; then unzip -q -o "$zip" -d "$dir"
    elif have bsdtar; then bsdtar -xf "$zip" -C "$dir"
    elif have python3; then python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$zip" "$dir"
    else warn "cannot unzip, no unzip, bsdtar or python3 : $INSTALL unzip"; return 1
    fi || return 1
    # the archive may hold the files within a directory
    local sub
    sub=$(dirname "$(find "$dir" -name dickens -type f | head -1)")
    (cd "$sub" && tar -cf "$tar.part" $SILESIA_FILES) && mv "$tar.part" "$tar" && rm -rf "$dir"
}

if [ -z "$INPUT" ]; then
    get_silesia || die "cannot prepare the Silesia corpus : download $SILESIA_URL, make a tar of its files, and pass it with -i"
fi
[ -r "$INPUT" ] || die "$INPUT : cannot read"
SIZE=$(wc -c < "$INPUT" | tr -d ' ')
[ "$SIZE" -gt 0 ] || die "$INPUT : empty"
SIZE_MB=$(calc "$SIZE / 1e6")


# ===== compressors : name | program | compression | decompression =====

COMPRESSORS=(
    "lz4 -1|$LZ4|\"$LZ4\" -1 $LZ4_T1 -c|\"$LZ4\" -d -c"
    "lz4 -9|$LZ4|\"$LZ4\" -9 $LZ4_T1 -c|\"$LZ4\" -d -c"
    "zstd -1|zstd|zstd -1 -T1 -c|zstd -d -c"
    "lzop -1|lzop|lzop -1 -c|lzop -d -c"
    "snzip|snzip|snzip -c|snzip -d -c"
    "gzip -1|gzip|gzip -1 -c|gzip -d -c"
    "gzip -6|gzip|gzip -6 -c|gzip -d -c"
)

version() {
    "$1" --version 2>&1 | grep -m1 -oE 'v?[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1
}

same_content() {   # same_content CMD FILE : output of CMD identical to FILE
    if have cmp; then eval "$1" | cmp -s - "$2"
    elif have sha256sum; then [ "$(eval "$1" | sha256sum)" = "$(sha256sum < "$2")" ]
    else warn "no cmp nor sha256sum : decompression not verified"; return 0
    fi
}


# ===== measurements =====

time_cmd() {   # time_cmd CMD : sets ELAPSED, in seconds
    local t0
    clock; t0=$CLOCK
    eval "$1" 2>>"$LOG" || return 1
    clock
    ELAPSED=$(calc "$CLOCK - $t0" 6)
}

measure() {   # measure CMD : sets MEDIAN (seconds) and NRUNS
    local times=() spent=0
    while [ ${#times[@]} -lt "$RUNS" ]; do
        time_cmd "$1" || return 1
        times+=("$ELAPSED")
        spent=$(calc "$spent + $ELAPSED" 6)
        [ "$(calc "$spent > $MAX_TIME" 0)" = 1 ] && break
    done
    NRUNS=${#times[@]}
    MEDIAN=$(printf '%s\n' "${times[@]}" | sort -g |
             awk '{ t[NR] = $1 } END { printf "%.6f", (NR % 2) ? t[(NR + 1) / 2] : (t[NR / 2] + t[NR / 2 + 1]) / 2 }')
}

speed() { calc "$SIZE_MB / $1" 0; }   # MB/s of original data, from a duration

[ -n "$OUTDIR" ] || OUTDIR=./bench-pipeline-$(hostname -s 2>/dev/null || echo host)-$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUTDIR" || die "cannot create $OUTDIR"

{
    echo "date      : $(date -u '+%Y-%m-%d %H:%M UTC')"
    echo "system    : ${PRETTY_NAME:-unknown}, kernel $(uname -r)"
    echo "cpu       : $( (lscpu 2>/dev/null | sed -n 's/^Model name: *//p'; grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/.*: //') | head -1), $(nproc 2>/dev/null) threads"
    echo "governor  : $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
    echo "input     : $INPUT, $SIZE bytes"
    echo "lz4       : $LZ4"
    echo "runs      : $RUNS, median"
} | tee "$OUTDIR/env.txt" >&2

# codecs : ratio, speeds alone, and speed of the pipe without link limit
echo "compressor,version,ratio,compression_MBps,decompression_MBps,pipe_MBps" > "$OUTDIR/codecs.csv"
AVAILABLE=()
MISSING=()
for entry in "${COMPRESSORS[@]}"; do
    IFS='|' read -r name prog comp decomp <<<"$entry"
    if ! have "$prog"; then MISSING+=("$prog"); continue; fi
    file=$WORK/$(echo "$name" | tr ' ' '_').cmp
    msg "preparing $name ..."
    if ! eval "$comp" < "$INPUT" > "$file" 2>>"$LOG"; then warn "$name : compression failed, skipped (see $LOG)"; continue; fi
    if ! same_content "$decomp < \"$file\"" "$INPUT"; then warn "$name : decompression differs, skipped"; continue; fi
    ratio=$(calc "$SIZE / $(wc -c < "$file")")
    measure "$comp < \"$INPUT\" > /dev/null" || { warn "$name : compression failed"; continue; }
    cspeed=$(speed "$MEDIAN")
    measure "$decomp < \"$file\" > /dev/null" || { warn "$name : decompression failed"; continue; }
    dspeed=$(speed "$MEDIAN")
    measure "$comp < \"$INPUT\" | $decomp > /dev/null" || { warn "$name : pipe failed"; continue; }
    pspeed=$(speed "$MEDIAN")
    echo "$name,$(version "$prog"),$ratio,$cspeed,$dspeed,$pspeed" >> "$OUTDIR/codecs.csv"
    AVAILABLE+=("$entry")
done
if [ ${#MISSING[@]} -gt 0 ]; then
    missing=$(printf '%s\n' "${MISSING[@]}" | sort -u | tr '\n' ' ')
    warn "not installed, skipped : $missing(to include them : $INSTALL $missing)"
fi
[ ${#AVAILABLE[@]} -gt 0 ] || die "no compressor available"

# pipelines through a link
echo "scenario,link,link_MBps,compressor,runs,seconds,effective_MBps" > "$OUTDIR/pipeline.csv"
run_scenario() {   # run_scenario NAME LINKS
    local scenario=$1 link name speed entry cname prog comp decomp file cmd eff
    for link in $2; do
        name=${link%:*}; name=${name//_/ }; speed=${link##*:}
        msg "$scenario, $name ($speed MB/s) ..."
        # no compression : a process reads the data, like a decompressor
        measure "\"$THROTTLE\" $speed < \"$INPUT\" | cat > /dev/null" || die "throttle failed, see $LOG"
        eff=$(speed "$MEDIAN")
        [ "$(calc "$eff < 0.95 * $speed" 0)" = 1 ] && warn "throttle reaches only $eff MB/s out of $speed : results limited by this machine"
        echo "$scenario,$name,$speed,no compression,$NRUNS,$MEDIAN,$eff" >> "$OUTDIR/pipeline.csv"
        for entry in "${AVAILABLE[@]}"; do
            IFS='|' read -r cname prog comp decomp <<<"$entry"
            file=$WORK/$(echo "$cname" | tr ' ' '_').cmp
            if [ "$scenario" = read ]; then
                cmd="\"$THROTTLE\" $speed < \"$file\" | $decomp > /dev/null"
            else
                cmd="$comp < \"$INPUT\" | \"$THROTTLE\" $speed | $decomp > /dev/null"
            fi
            measure "$cmd" || { warn "$scenario, $name, $cname : failed"; continue; }
            echo "$scenario,$name,$speed,$cname,$NRUNS,$MEDIAN,$(speed "$MEDIAN")" >> "$OUTDIR/pipeline.csv"
        done
    done
}
run_scenario read "$READ_LINKS"
run_scenario transfer "$TRANSFER_LINKS"

cp "$LOG" "$OUTDIR/log.txt"
[ "$KEEP" = 1 ] && msg "compressed files kept in $WORK"

# summary
echo
echo "codecs (MB/s of original data) :"
column -s, -t < "$OUTDIR/codecs.csv" 2>/dev/null || cat "$OUTDIR/codecs.csv"
echo
echo "pipelines (effective MB/s) :"
awk -F, 'NR > 1 { printf "%-9s %-20s %8s  %-15s %8s\n", $1, $2, $3, $4, $7 }' "$OUTDIR/pipeline.csv"
echo
echo "results in $OUTDIR"
