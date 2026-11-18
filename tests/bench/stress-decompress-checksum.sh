#!/usr/bin/env bash
# stress-decompress-checksum.sh : repeated decompressions from a pipe, reference (origin/dev) and this branch,
# to investigate intermittent errors : counts failures, checks the output written before each failure,
# and checks every successful output (silent corruption).
# Builds every program from the git repository of this script, in a temporary directory.

set -u
set -o pipefail

RUNS=200
INPUT=""
REF=origin/dev
SEGMENTS="64"   # KB
CACHE_DIR=${XDG_CACHE_HOME:-$HOME/.cache}/lz4-bench-pipeline

usage() {
    cat <<EOF
usage : $0 [options]
  -i FILE   input file (default : $CACHE_DIR/silesia.tar)
  -n #      runs per program (default : $RUNS)
  -r REF    reference version (default : $REF)
  -s LIST   segment sizes in KB (default : "$SEGMENTS")
  -h        this help
EOF
}

die() { printf 'error : %s\n' "$*" >&2; exit 1; }

while getopts "i:n:r:s:h" opt; do
    case $opt in
        i) INPUT=$OPTARG ;;
        n) RUNS=$OPTARG ;;
        r) REF=$OPTARG ;;
        s) SEGMENTS=$OPTARG ;;
        h) usage; exit 0 ;;
        *) usage; exit 1 ;;
    esac
done

REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel) || die "$0 : not within a git repository"
git -C "$REPO" rev-parse --verify -q "$REF^{commit}" >/dev/null || die "$REF : unknown (git fetch origin ?)"
[ -n "$INPUT" ] || INPUT=$CACHE_DIR/silesia.tar
[ -r "$INPUT" ] || die "$INPUT : cannot read, pass an input file with -i"

if [ -d /dev/shm ] && [ -z "${TMPDIR:-}" ]; then TMPDIR=/dev/shm; fi
WORK=$(mktemp -d "${TMPDIR:-/tmp}/stress-dcs.XXXXXX") || die "cannot create a temporary directory"
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
"$WORK/bin/ref" -q -1 -T1 "$INPUT" -c > "$WORK/crc.lz4" || die "compression failed"

LOG="stress-decompress-checksum-$(uname -n)-$(date +%Y%m%d-%H%M).txt"
{
    echo "lz4 decompression from a pipe : reference $REF ($(git -C "$REPO" rev-parse --short "$REF")), HEAD ($(git -C "$REPO" rev-parse --short HEAD)) with segments of N KB (segN)"
    echo "host : $(uname -n), $(uname -sr), $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/.*: //')"
    echo "input : $INPUT, $RUNS runs per program"
} | tee "$LOG"

declare -A FAIL SILENT
for p in $PROGS; do FAIL[$p]=0; SILENT[$p]=0; done
for r in $(seq "$RUNS"); do
    for p in $PROGS; do
        if cat "$WORK/crc.lz4" | "$WORK/bin/$p" -q -d -c > "$WORK/out" 2> "$WORK/err"; then
            cmp -s "$WORK/out" "$INPUT" || {
                SILENT[$p]=$((SILENT[$p] + 1))
                echo "run $r, $p : exit 0, but output differs : $(cmp "$WORK/out" "$INPUT" 2>&1 | head -1)" | tee -a "$LOG"
            }
        else
            FAIL[$p]=$((FAIL[$p] + 1))
            n=$(wc -c < "$WORK/out" | tr -d ' ')
            if cmp -s -n "$n" "$WORK/out" "$INPUT"; then check="the $n bytes written are correct"
            else check="the $n bytes written differ : $(cmp -n "$n" "$WORK/out" "$INPUT" 2>&1 | head -1)"; fi
            echo "run $r, $p : $(tr '\n' ' ' < "$WORK/err"); $check" | tee -a "$LOG"
        fi
    done
    [ $((r % 20)) -ne 0 ] || printf 'run %u / %u\n' "$r" "$RUNS" >&2
done

{
    echo
    for p in $PROGS; do
        echo "$p : ${FAIL[$p]} failures, ${SILENT[$p]} silent corruptions, out of $RUNS runs"
    done
} | tee -a "$LOG"
