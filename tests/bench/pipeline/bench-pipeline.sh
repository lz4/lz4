#!/usr/bin/env bash
# bench-pipeline.sh : effective speed of compression command lines, within pipelines
# where a link of limited speed (storage device, network) separates stages running at the same time :
#   read     : throttle LINK < file.compressed | decompress > /dev/null
#   transfer : compress < file | throttle LINK | decompress > /dev/null
# Effective speed = size of the original file / elapsed time, 1 MB = 1000000 bytes.
# Transfers are measured twice for multi-threaded compressors : with default settings,
# and with a single compression thread (lz4 -T1, zstd -T1, pigz -p 1).
#
# Programs come from the PATH, or from -b NAME=PATH, missing ones are skipped. Their versions are reported.
# throttle.c, next to this script, simulates the link. Results : directory given by -o, summary on stdout.

set -u
set -o pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

RUNS=5
MAX_TIME=20   # seconds of runs per measurement : fewer runs when they are long
READ_LINKS="HDD:200 SATA_SSD:550 NVMe_PCIe_3.0_x4:3500"
TRANSFER_LINKS="Internet_100_Mbit/s:11.7 1_GbE:117 2.5_GbE:295 5_GbE:590 10_GbE:1170"
INPUT=""
declare -A BIN=()   # programs given by -b NAME=PATH
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
  -b N=PATH program N to use, for instance -b zstd=\$HOME/zstd/zstd (default : from the PATH)
  -l PATH   lz4 program to use, same as -b lz4=PATH
  -o DIR    output directory (default : ./bench-pipeline-HOST-DATE)
  -k        keep compressed files
  -h        this help
EOF
}

msg()  { printf '%s\n' "$*" >&2; }
warn() { printf 'warning : %s\n' "$*" >&2; }
die()  { printf 'error : %s\n' "$*" >&2; exit 1; }

while getopts "i:n:r:t:b:l:o:kh" opt; do
    case $opt in
        i) INPUT=$OPTARG ;;
        n) RUNS=$OPTARG ;;
        r) READ_LINKS=$OPTARG ;;
        t) TRANSFER_LINKS=$OPTARG ;;
        b) case $OPTARG in *=?*) BIN[${OPTARG%%=*}]=${OPTARG#*=} ;; *) die "-b expects NAME=PATH" ;; esac ;;
        l) BIN[lz4]=$OPTARG ;;
        o) OUTDIR=$OPTARG ;;
        k) KEEP=1 ;;
        h) usage; exit 0 ;;
        *) usage; exit 1 ;;
    esac
done

have() { command -v "$1" >/dev/null 2>&1; }
bin() {   # bin NAME : path of program NAME, from -b, or from the PATH
    if [ -n "${BIN[$1]:-}" ]; then echo "${BIN[$1]}"; else command -v "$1"; fi
}
for prog in "${!BIN[@]}"; do
    [ -x "${BIN[$prog]}" ] && [ ! -d "${BIN[$prog]}" ] || die "-b $prog=${BIN[$prog]} : not an executable"
done

# install hints, per distribution
. /etc/os-release 2>/dev/null
case " ${ID:-} ${ID_LIKE:-} " in
    *" debian "*|*" ubuntu "*) INSTALL="sudo apt install" ; CC_PKG="gcc" ;;
    *" fedora "*|*" rhel "*)   INSTALL="sudo dnf install" ; CC_PKG="gcc" ;;
    *" arch "*)                INSTALL="sudo pacman -S"   ; CC_PKG="gcc" ;;
    *" suse "*|*" opensuse "*) INSTALL="sudo zypper install" ; CC_PKG="gcc" ;;
    *)                         INSTALL="install"          ; CC_PKG="a C compiler" ;;
esac

# clock : elapsed times from bash's EPOCHREALTIME (bash 5), or date
if [ -n "${EPOCHREALTIME:-}" ]; then
    clock() { CLOCK=${EPOCHREALTIME/,/.}; }
else
    clock() { CLOCK=$(date +%s.%N); }
fi
calc() { awk "BEGIN { printf \"%.${2:-3}f\", $1 }"; }


# ===== tools : throttle, lz4 =====

have cc || have gcc || die "no C compiler, needed to build throttle : $INSTALL $CC_PKG"
CC=$(have cc && echo cc || echo gcc)

WORK=$(mktemp -d "${TMPDIR:-/tmp}/bench-pipeline.XXXXXX") || die "cannot create a temporary directory"
trap '[ "$KEEP" = 1 ] || rm -rf "$WORK"' EXIT
LOG=$WORK/log.txt

$CC -O2 -o "$WORK/throttle" "$SCRIPT_DIR/throttle.c" 2>>"$LOG" || die "cannot build throttle, see $LOG"
THROTTLE=$WORK/throttle

LZ4=$(bin lz4) || die "lz4 not found : $INSTALL lz4, or pass a program with -l"
LZ4_MT=1
"$LZ4" -T1 -c </dev/null >/dev/null 2>&1 || { LZ4_MT=0; warn "$LZ4 doesn't support -T# (before v1.10) : compression is single-threaded"; }


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


# ===== compressors : name | program | compression options | decompression options | scenarios =====
# The name is the compression command line. Single-thread variants only matter for transfers.

COMPRESSORS=(
    "lz4 -1|lz4|-1 -c|-d -c|read transfer"
    "lz4 -1 -T1|lz4|-1 -T1 -c|-d -c|transfer"
    "lz4 -9|lz4|-9 -c|-d -c|read transfer"
    "lz4 -9 -T1|lz4|-9 -T1 -c|-d -c|transfer"
    "zstd -1|zstd|-1 -c|-d -c|read transfer"
    "zstd -1 -T1|zstd|-1 -T1 -c|-d -c|transfer"
    "lzop -1|lzop|-1 -c|-d -c|read transfer"
    "gzip -1|gzip|-1 -c|-d -c|read transfer"
    "gzip -6|gzip|-6 -c|-d -c|read transfer"
    "pigz -1|pigz|-1 -c|-d -c|read transfer"
    "pigz -1 -p 1|pigz|-1 -p 1 -c|-d -c|transfer"
    "pigz -6|pigz|-6 -c|-d -c|read transfer"
    "pigz -6 -p 1|pigz|-6 -p 1 -c|-d -c|transfer"
)
if [ "$LZ4_MT" = 0 ]; then   # without -T#, lz4 compresses on a single thread : no variant
    for i in "${!COMPRESSORS[@]}"; do
        case ${COMPRESSORS[$i]} in "lz4 "*"-T1|"*) unset "COMPRESSORS[$i]" ;; esac
    done
fi

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

# default compression threads, used by the transfers with default settings
LZ4_THREADS=$("$LZ4" -v -1 -c </dev/null 2>&1 >/dev/null | tr '\r' '\n' | sed -n 's/.*Using up to \([0-9]*\) threads.*/\1/p' | head -1)
ZSTD_THREADS=$("$(bin zstd)" -H 2>&1 | sed -n 's/.*-T#.*\[Default: \([0-9]*\).*/\1/p' | head -1)
PIGZ_THREADS=$(nproc 2>/dev/null)

{
    echo "date      : $(date -u '+%Y-%m-%d %H:%M UTC')"
    echo "system    : ${PRETTY_NAME:-unknown}, kernel $(uname -r)"
    echo "cpu       : $( (lscpu 2>/dev/null | sed -n 's/^Model name: *//p'; grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/.*: //') | head -1), $(nproc 2>/dev/null) threads"
    echo "governor  : $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
    echo "input     : $INPUT, $SIZE bytes"
    echo "lz4       : $LZ4"
    echo "threads   : lz4 $LZ4_THREADS, zstd $ZSTD_THREADS, pigz $PIGZ_THREADS (default compression threads)"
    echo "runs      : $RUNS, median"
} | tee "$OUTDIR/env.txt" >&2

# codecs : ratio, speeds alone, and speed of the pipe without link limit
echo "compressor,version,ratio,compression_MBps,decompression_MBps,pipe_MBps" > "$OUTDIR/codecs.csv"
AVAILABLE=()   # name | program | compression command | decompression command | scenarios | version
MISSING=()
declare -A VERSIONS=()
for entry in "${COMPRESSORS[@]}"; do
    IFS='|' read -r name prog copts dopts scenarios <<<"$entry"
    path=$(bin "$prog") || { MISSING+=("$prog"); continue; }
    comp="\"$path\" $copts"
    decomp="\"$path\" $dopts"
    ver=$(version "$path")
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
    echo "$name,$ver,$ratio,$cspeed,$dspeed,$pspeed" >> "$OUTDIR/codecs.csv"
    AVAILABLE+=("$name|$prog|$comp|$decomp|$scenarios|$ver")
    VERSIONS[$prog]="$ver : $path"
done
for prog in "${!VERSIONS[@]}"; do
    printf 'version   : %-6s %s\n' "$prog" "${VERSIONS[$prog]}"
done | sort | tee -a "$OUTDIR/env.txt" >&2
if [ ${#MISSING[@]} -gt 0 ]; then
    missing=$(printf '%s\n' "${MISSING[@]}" | sort -u | tr '\n' ' ')
    warn "not installed, skipped : $missing(to include them : $INSTALL $missing)"
fi
[ ${#AVAILABLE[@]} -gt 0 ] || die "no compressor available"

# pipelines through a link
echo "scenario,link,link_MBps,compressor,runs,seconds,effective_MBps,version" > "$OUTDIR/pipeline.csv"
run_scenario() {   # run_scenario NAME LINKS
    local scenario=$1 link name speed entry cname prog comp decomp scenarios ver file cmd eff
    for link in $2; do
        name=${link%:*}; name=${name//_/ }; speed=${link##*:}
        msg "$scenario, $name ($speed MB/s) ..."
        # no compression : a process reads the data, like a decompressor
        measure "\"$THROTTLE\" $speed < \"$INPUT\" | cat > /dev/null" || die "throttle failed, see $LOG"
        eff=$(speed "$MEDIAN")
        [ "$(calc "$eff < 0.95 * $speed" 0)" = 1 ] && warn "throttle reaches only $eff MB/s out of $speed : results limited by this machine"
        echo "$scenario,$name,$speed,no compression,$NRUNS,$MEDIAN,$eff," >> "$OUTDIR/pipeline.csv"
        for entry in "${AVAILABLE[@]}"; do
            IFS='|' read -r cname prog comp decomp scenarios ver <<<"$entry"
            case " $scenarios " in *" $scenario "*) ;; *) continue ;; esac
            file=$WORK/$(echo "$cname" | tr ' ' '_').cmp
            if [ "$scenario" = read ]; then
                cmd="\"$THROTTLE\" $speed < \"$file\" | $decomp > /dev/null"
            else
                cmd="$comp < \"$INPUT\" | \"$THROTTLE\" $speed | $decomp > /dev/null"
            fi
            measure "$cmd" || { warn "$scenario, $name, $cname : failed"; continue; }
            echo "$scenario,$name,$speed,$cname,$NRUNS,$MEDIAN,$(speed "$MEDIAN"),$ver" >> "$OUTDIR/pipeline.csv"
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
