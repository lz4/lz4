#!/usr/bin/env bash
# bench-decompress-checksum.sh : decompression speed of lz4, with the content checksum verified
# by the writing thread (this branch, several LZ4IO_HASH_SEGMENT sizes) or by liblz4 (reference, origin/dev).
# Builds every program from the git repository of this script, in a temporary directory. Results : on stdout, and in a file.
# Effective speed = decompressed size / elapsed time (1 MB = 1000000 bytes), median of runs interleaved between programs.
# Scenarios : towards /dev/null, a pipe, a file (in TMPDIR, tmpfs preferably), from a pipe, and test mode (-t).

set -u
set -o pipefail

RUNS=9
INPUT=""
REF=origin/dev
SEGMENTS="16 32 64 128 256 1024 4096"   # KB
CPUS=""
CACHE_DIR=${XDG_CACHE_HOME:-$HOME/.cache}/lz4-bench-pipeline

usage() {
    cat <<EOF
usage : $0 [options]
  -i FILE   input file (default : $CACHE_DIR/silesia.tar, from tests/bench/pipeline)
  -n #      runs per measurement, the median is kept (default : $RUNS)
  -r REF    reference version (default : $REF)
  -s LIST   segment sizes in KB (default : "$SEGMENTS")
  -c CPUS   run pinned to CPUS (taskset -c), ex: 0-7 (default : not pinned)
  -h        this help
EOF
}

die() { printf 'error : %s\n' "$*" >&2; exit 1; }

while getopts "i:n:r:s:c:h" opt; do
    case $opt in
        i) INPUT=$OPTARG ;;
        n) RUNS=$OPTARG ;;
        r) REF=$OPTARG ;;
        s) SEGMENTS=$OPTARG ;;
        c) CPUS=$OPTARG ;;
        h) usage; exit 0 ;;
        *) usage; exit 1 ;;
    esac
done

[ -n "${EPOCHREALTIME:-}" ] || die "bash 5 required (EPOCHREALTIME)"
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel) || die "$0 : not within a git repository"
git -C "$REPO" rev-parse --verify -q "$REF^{commit}" >/dev/null || die "$REF : unknown (git fetch origin ?)"
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

printf 'building %s and %s ...\n' "$REF" "HEAD (segments : $SEGMENTS KB)" >&2
build ref "$REF" ""
PROGS="ref"
for s in $SEGMENTS; do
    build "seg$s" HEAD "-DLZ4IO_HASH_SEGMENT=$((s * 1024))"
    PROGS="$PROGS seg$s"
done

# compressed inputs : with content checksum (default), and without
"$WORK/bin/ref" -q -1 -T1 "$INPUT" -c > "$WORK/crc.lz4" || die "compression failed"
"$WORK/bin/ref" -q -1 -T1 --no-frame-crc "$INPUT" -c > "$WORK/nocrc.lz4" || die "compression failed"
SIZE=$(wc -c < "$INPUT" | tr -d ' ')
OUT=$WORK/out

run() {   # run SCENARIO PROGRAM FILE
    local p=$WORK/bin/$2
    case $1 in
        devnull) $PIN "$p" -q -d -c "$3" > /dev/null ;;
        pipe)    $PIN "$p" -q -d -c "$3" | $PIN cat > /dev/null ;;
        file)    $PIN "$p" -q -d -f "$3" "$OUT" ;;
        stdin)   $PIN cat "$3" | $PIN "$p" -q -d -c > /dev/null ;;
        test)    $PIN "$p" -q -t "$3" ;;
    esac
}

SCENARIOS="devnull pipe file stdin test"
NOCRC_PROG=seg${SEGMENTS%% *}   # files without checksum : reference, and one segment size (64 KB, the default, if listed)
case " $SEGMENTS " in *" 64 "*) NOCRC_PROG=seg64 ;; esac
measure() {   # measure FILE PROGRAMS... : table of MB/s
    local f=$1; shift
    declare -A T
    local s r p t0 t1
    for s in $SCENARIOS; do
        for r in $(seq "$RUNS"); do
            for p in "$@"; do
                t0=$EPOCHREALTIME
                run "$s" "$p" "$f" || die "$p failed ($s)"
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
    echo "lz4 decompression, content checksum : reference $REF ($(git -C "$REPO" rev-parse --short "$REF")), HEAD ($(git -C "$REPO" rev-parse --short HEAD)) with segments of N KB (segN)"
    echo "host : $(uname -n), $(uname -sr), $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/.*: //')"
    command -v lscpu >/dev/null && lscpu | grep -E '^(L2|L3) cache' | sed 's/  */ /g'
    echo "input : $INPUT ($SIZE bytes), $RUNS runs, MB/s, $([ -n "$CPUS" ] && echo "pinned to CPUs $CPUS" || echo "not pinned")"
    echo
    echo "with content checksum :"
    measure "$WORK/crc.lz4" $PROGS
    echo
    echo "without content checksum (--no-frame-crc) :"
    measure "$WORK/nocrc.lz4" ref "$NOCRC_PROG"
} | tee "bench-decompress-checksum-$(uname -n)-$(date +%Y%m%d-%H%M).txt"
