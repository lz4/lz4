#!/usr/bin/env bash
# bench-decompress-checksum.sh : decompression speed of lz4 with content checksums : reference (origin/dev),
# other revisions (-x), and variants of HEAD (-s : LZ4IO_HASH_SEGMENT sizes, -q : OUTBUFF_QUEUE depths).
# Builds every program from the git repository of this script, in a temporary directory. Results : on stdout, and in a file.
# Effective speed = decompressed size / elapsed time (1 MB = 1000000 bytes), median of runs interleaved between programs.
# Scenarios : towards /dev/null, a pipe, a new file (in TMPDIR, tmpfs preferably), from a pipe, and test mode (-t).
# -C : compression speed instead (content checksum of the source, hashed by the main thread), same speed definition.
# -S : other scenarios : towards a new file without sparse mode, or over the existing file (its release is then measured too).

set -u
set -o pipefail

MODE=decompress
RUNS=9
INPUT=""
REF=origin/dev
SEGMENTS=""      # KB
QUEUES="1 2 3"
EXTRAS="pr=origin/decompress-checksum"
CPUS=""
SCEN=""
CACHE_DIR=${XDG_CACHE_HOME:-$HOME/.cache}/lz4-bench-pipeline

usage() {
    cat <<EOF
usage : $0 [options]
  -i FILE   input file (default : $CACHE_DIR/silesia.tar, from tests/bench/pipeline)
  -n #      runs per measurement, the median is kept (default : $RUNS)
  -r REF    reference version (default : $REF)
  -s LIST   LZ4IO_HASH_SEGMENT sizes in KB, variants of HEAD named segN (default : "$SEGMENTS")
  -q LIST   OUTBUFF_QUEUE depths, variants of HEAD named qN (default : "$QUEUES")
  -x N=REV  program N built from REV, repeatable (default : "$EXTRAS")
  -c CPUS   run pinned to CPUS (taskset -c), ex: 0-7 (default : not pinned)
  -S LIST   scenarios (default : "devnull pipe file stdin test", with -C : "devnull file stdin T1 fast9"), also :
            nosparse (file, --no-sparse), overwrite (file, over the existing one) ; files : in TMPDIR (default : /dev/shm),
            removed before each run, except with overwrite
  -C        compression instead, no variant of HEAD : -1 towards /dev/null (devnull), a file (file), from a pipe (stdin),
            -1 -T1 (T1), --fast=9 (fast9) ; threads : LZ4_NBWORKERS, or automatic (one per core)
  -h        this help
EOF
}

die() { printf 'error : %s\n' "$*" >&2; exit 1; }

XSET=0
while getopts "i:n:r:s:q:x:c:S:Ch" opt; do
    case $opt in
        i) INPUT=$OPTARG ;;
        n) RUNS=$OPTARG ;;
        r) REF=$OPTARG ;;
        s) SEGMENTS=$OPTARG ;;
        q) QUEUES=$OPTARG ;;
        x) [ $XSET = 1 ] || EXTRAS=""; XSET=1; EXTRAS="$EXTRAS $OPTARG" ;;
        c) CPUS=$OPTARG ;;
        S) SCEN=$OPTARG ;;
        C) MODE=compress ;;
        h) usage; exit 0 ;;
        *) usage; exit 1 ;;
    esac
done

[ -n "${EPOCHREALTIME:-}" ] || die "bash 5 required (EPOCHREALTIME)"
[ $MODE = decompress ] || { SEGMENTS=""; QUEUES=""; }
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel) || die "$0 : not within a git repository"
for r in "$REF" $(for x in $EXTRAS; do echo "${x#*=}"; done); do
    git -C "$REPO" rev-parse --verify -q "$r^{commit}" >/dev/null || die "$r : unknown (git fetch origin ?)"
done
[ -n "$INPUT" ] || INPUT=$CACHE_DIR/silesia.tar
[ -r "$INPUT" ] || die "$INPUT : cannot read, pass an input file with -i"
[ -z "$CPUS" ] || command -v taskset >/dev/null || die "taskset not found"
PIN=""; [ -z "$CPUS" ] || PIN="taskset -c $CPUS"

if [ -d /dev/shm ] && [ -z "${TMPDIR:-}" ]; then TMPDIR=/dev/shm; fi
WORK=$(mktemp -d "${TMPDIR:-/tmp}/bench-dcs.XXXXXX") || die "cannot create a temporary directory"
trap 'rm -rf "$WORK"' EXIT
NBJOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4); [ "$NBJOBS" -le 16 ] || NBJOBS=16

build() {   # build NAME REV CPPFLAGS : program $WORK/bin/NAME
    local dir=$WORK/src-$1
    mkdir -p "$dir" "$WORK/bin"
    git -C "$REPO" archive "$2" | tar -x -C "$dir" || die "git archive $2 failed"
    make -s -j"$NBJOBS" -C "$dir/programs" lz4 CPPFLAGS="$3" >"$WORK/build.log" 2>&1 \
        || { tail -20 "$WORK/build.log" >&2; die "build of $1 failed"; }
    cp -L "$dir/programs/lz4" "$WORK/bin/$1"
    rm -rf "$dir"
}

printf 'building %s, %s and variants of HEAD ...\n' "$REF" "${EXTRAS:-no other revision}" >&2
build ref "$REF" ""
PROGS="ref"
for x in $EXTRAS; do
    build "${x%%=*}" "${x#*=}" ""
    PROGS="$PROGS ${x%%=*}"
done
HEADPROGS=""
for s in $SEGMENTS; do
    build "seg$s" HEAD "-DLZ4IO_HASH_SEGMENT=$((s * 1024))"
    HEADPROGS="$HEADPROGS seg$s"
done
for q in $QUEUES; do
    build "q$q" HEAD "-DOUTBUFF_QUEUE=$q"
    HEADPROGS="$HEADPROGS q$q"
done
if [ -z "$HEADPROGS" ]; then build head HEAD ""; HEADPROGS=" head"; fi
PROGS="$PROGS$HEADPROGS"

SIZE=$(wc -c < "$INPUT" | tr -d ' ')
OUT=$WORK/out
if [ $MODE = decompress ]; then
    # compressed inputs : with content checksum (default), and without
    "$WORK/bin/ref" -q -1 -T1 "$INPUT" -c > "$WORK/crc.lz4" || die "compression failed"
    "$WORK/bin/ref" -q -1 -T1 --no-frame-crc "$INPUT" -c > "$WORK/nocrc.lz4" || die "compression failed"
    CRC_IN=$WORK/crc.lz4;  NOCRC_IN=$WORK/nocrc.lz4;  NOCRC_OPT=""
    SCENARIOS="devnull pipe file stdin test"
else
    CRC_IN=$INPUT;  NOCRC_IN=$INPUT;  NOCRC_OPT=--no-frame-crc
    SCENARIOS="devnull file stdin T1 fast9"
fi
[ -z "$SCEN" ] || SCENARIOS=$SCEN

run() {   # run SCENARIO PROGRAM FILE OPTIONS
    local p=$WORK/bin/$2
    case $MODE,$1 in
        decompress,devnull) $PIN "$p" -q -d -c "$3" > /dev/null ;;
        decompress,pipe)    $PIN "$p" -q -d -c "$3" | $PIN cat > /dev/null ;;
        decompress,file)    $PIN "$p" -q -d -f "$3" "$OUT" ;;
        decompress,stdin)   $PIN cat "$3" | $PIN "$p" -q -d -c > /dev/null ;;
        decompress,test)    $PIN "$p" -q -t "$3" ;;
        decompress,nosparse) $PIN "$p" -q -d -f --no-sparse "$3" "$OUT" ;;
        decompress,overwrite) $PIN "$p" -q -d -f "$3" "$OUT" ;;
        compress,devnull)   $PIN "$p" -q -1 $4 -c "$3" > /dev/null ;;
        compress,file)      $PIN "$p" -q -1 $4 -f "$3" "$OUT" ;;
        compress,stdin)     $PIN cat "$3" | $PIN "$p" -q -1 $4 -c > /dev/null ;;
        compress,T1)        $PIN "$p" -q -1 -T1 $4 -c "$3" > /dev/null ;;
        compress,fast9)     $PIN "$p" -q --fast=9 $4 -c "$3" > /dev/null ;;
        compress,overwrite) $PIN "$p" -q -1 $4 -f "$3" "$OUT" ;;
        *)                  die "$1 : unknown scenario" ;;
    esac
}

NOCRC_PROGS="${PROGS%%$HEADPROGS}$(set -- $HEADPROGS; echo " $1")"   # files without checksum : all but the other variants of HEAD
case " $HEADPROGS " in *" seg64 "*) NOCRC_PROGS="${PROGS%%$HEADPROGS} seg64" ;; esac
measure() {   # measure FILE OPTIONS PROGRAMS... : table of MB/s
    local f=$1 o=$2; shift 2
    declare -A T
    local s r p t0 t1
    for s in $SCENARIOS; do
        for r in $(seq "$RUNS"); do
            for p in "$@"; do
                [ "$s" = overwrite ] || rm -f "$OUT"
                t0=$EPOCHREALTIME
                run "$s" "$p" "$f" "$o" || die "$p failed ($s)"
                t1=$EPOCHREALTIME
                T[$s,$p]+="${t0/,/.} ${t1/,/.};"
            done
        done
    done
    printf '%-10s' ""; for s in $SCENARIOS; do printf '%9s' "$s"; done; echo
    for p in "$@"; do
        printf '%-10s' "$p"
        for s in $SCENARIOS; do
            printf '%s' "${T[$s,$p]}" | tr ';' '\n' | awk 'NF == 2 { print $2 - $1 }' | sort -n \
                | awk -v size="$SIZE" '{ d[NR] = $1 } END { printf "%9.0f", size / d[int((NR + 1) / 2)] / 1e6 }'
        done
        echo
    done
}

{
    echo "lz4 ${MODE}ion, content checksum : ref = $REF ($(git -C "$REPO" rev-parse --short "$REF"))$(for x in $EXTRAS; do printf ', %s = %s (%s)' "${x%%=*}" "${x#*=}" "$(git -C "$REPO" rev-parse --short "${x#*=}")"; done)"
    echo "variants of HEAD ($(git -C "$REPO" rev-parse --short HEAD)) : segN = LZ4IO_HASH_SEGMENT of N KB, qN = OUTBUFF_QUEUE of N"
    echo "host : $(uname -n), $(uname -sr), $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/.*: //')"
    command -v lscpu >/dev/null && lscpu | grep -E '^(L2|L3) cache' | sed 's/  */ /g'
    echo "input : $INPUT ($SIZE bytes), $RUNS runs, MB/s, $([ -n "$CPUS" ] && echo "pinned to CPUs $CPUS" || echo "not pinned")"
    [ $MODE = decompress ] || echo "compression threads : ${LZ4_NBWORKERS:-automatic}"
    echo
    echo "with content checksum :"
    measure "$CRC_IN" "" $PROGS
    echo
    echo "without content checksum (--no-frame-crc) :"
    measure "$NOCRC_IN" "$NOCRC_OPT" $NOCRC_PROGS
} | tee "bench-$MODE-checksum-$(uname -n)-$(date +%Y%m%d-%H%M).txt"
