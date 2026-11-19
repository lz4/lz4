#!/usr/bin/env bash
# profile-decompress-checksum.sh : where decompression with content checksums spends its time :
# speed of lz4's XXH32 (lib/xxhash.c, compiled as lz4 compiles it), speed and perf profile per thread
# of `lz4 -t`, for the reference (origin/dev) and HEAD.
# Builds every program from the git repository of this script, in a temporary directory. Results : on stdout, and in a file.

set -u
set -o pipefail

INPUT=""
REF=origin/dev
CACHE_DIR=${XDG_CACHE_HOME:-$HOME/.cache}/lz4-bench-pipeline

usage() {
    cat <<EOF
usage : $0 [options]
  -i FILE   input file (default : $CACHE_DIR/silesia.tar)
  -r REF    reference version (default : $REF)
  -h        this help
EOF
}

die() { printf 'error : %s\n' "$*" >&2; exit 1; }

while getopts "i:r:h" opt; do
    case $opt in
        i) INPUT=$OPTARG ;;
        r) REF=$OPTARG ;;
        h) usage; exit 0 ;;
        *) usage; exit 1 ;;
    esac
done

[ -n "${EPOCHREALTIME:-}" ] || die "bash 5 required (EPOCHREALTIME)"
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel) || die "$0 : not within a git repository"
git -C "$REPO" rev-parse --verify -q "$REF^{commit}" >/dev/null || die "$REF : unknown (git fetch origin ?)"
[ -n "$INPUT" ] || INPUT=$CACHE_DIR/silesia.tar
[ -r "$INPUT" ] || die "$INPUT : cannot read, pass an input file with -i"
CC=${CC:-cc}
command -v "$CC" >/dev/null || die "$CC : no C compiler"

if [ -d /dev/shm ] && [ -z "${TMPDIR:-}" ]; then TMPDIR=/dev/shm; fi
WORK=$(mktemp -d "${TMPDIR:-/tmp}/profile-dcs.XXXXXX") || die "cannot create a temporary directory"
trap 'rm -rf "$WORK"' EXIT
NBJOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4); [ "$NBJOBS" -le 16 ] || NBJOBS=16

build() {   # build NAME REV : program $WORK/bin/NAME, and its sources in $WORK/src-NAME
    local dir=$WORK/src-$1
    mkdir -p "$dir" "$WORK/bin"
    git -C "$REPO" archive "$2" | tar -x -C "$dir" || die "git archive $2 failed"
    make -s -j"$NBJOBS" -C "$dir/programs" lz4 >"$WORK/build.log" 2>&1 \
        || { tail -20 "$WORK/build.log" >&2; die "build of $1 failed"; }
    cp -L "$dir/programs/lz4" "$WORK/bin/$1"
}

printf 'building %s and HEAD ...\n' "$REF" >&2
build ref "$REF"
build head HEAD

# XXH32 of lz4 (lib/xxhash.c of HEAD), compiled with the options of programs/Makefile, and without vectorization :
# single pass (XXH32()), and streaming by 4 MB pieces (XXH32_update(), as lz4frame.c)
cat > "$WORK/xxb.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "xxhash.h"
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
#define N (4 << 20)
#define LOOPS 256
int main(void)
{
    unsigned char* const b = (unsigned char*)malloc(N);
    XXH32_state_t* const st = XXH32_createState();
    unsigned h = 0; int i; double t1, t2;
    if (b == NULL || st == NULL) return 1;
    for (i = 0; i < N; i++) b[i] = (unsigned char)(i * 2654435761u >> 13);
    t1 = now(); for (i = 0; i < LOOPS; i++) h += XXH32(b, N, (unsigned)i); t1 = now() - t1;
    XXH32_reset(st, 0);
    t2 = now(); for (i = 0; i < LOOPS; i++) XXH32_update(st, b, N); t2 = now() - t2;
    h += XXH32_digest(st);
    printf("XXH32 of lz4 (lib/xxhash.c, %-38s), 4 MB : single pass %5.1f GB/s, streaming %5.1f GB/s (%08X)\n",
           FLAGS, (double)LOOPS * N / t1 / 1e9, (double)LOOPS * N / t2 / 1e9, h);
    return 0;
}
EOF
XXB=""   # benchmarks built : compiler options of each one, and the guard of xxHash dev (cd33964) in XXH32_round()
i=0
while IFS= read -r f; do
    i=$((i + 1))
    "$CC" $f -DXXH_NAMESPACE=LZ4_ -DFLAGS="\"$CC $f\"" -I"$WORK/src-head/lib" "$WORK/xxb.c" "$WORK/src-head/lib/xxhash.c" \
        -o "$WORK/bin/xxb$i" 2>/dev/null && XXB="$XXB xxb$i"
done <<EOF
-O3
-O3 -fno-tree-vectorize
-O3 -fno-tree-slp-vectorize
-O3 -fno-tree-loop-vectorize
-O3 -mprefer-vector-width=128
-O3 -march=x86-64-v2
EOF
[ -n "$XXB" ] || die "cannot build the XXH32 benchmark"
mkdir "$WORK/guard" && cp "$WORK/src-head/lib/xxhash.h" "$WORK/guard/"
awk '{ print } /^    seed \*= PRIME32_1;$/ { print "#if defined(__GNUC__) && (defined(__SSE2__) || defined(__aarch64__) || defined(__wasm_simd128__))"; print "    __asm__ (\"\" : \"+r\" (seed));"; print "#endif" }' \
    "$WORK/src-head/lib/xxhash.c" > "$WORK/guard/xxhash.c"
grep -q '__asm__' "$WORK/guard/xxhash.c" || die "cannot add the guard to XXH32_round()"
"$CC" -O3 -DXXH_NAMESPACE=LZ4_ -DFLAGS="\"$CC -O3, guard of xxHash dev (SSE2)\"" -I"$WORK/guard" "$WORK/xxb.c" "$WORK/guard/xxhash.c" -o "$WORK/bin/xxb-guard" \
    && XXB="$XXB xxb-guard"
# assembly of XXH32_update(), as compiled into lz4
ASM="xxh32-update-$(uname -n).s"
"$CC" -O3 -S -DXXH_NAMESPACE=LZ4_ -I"$WORK/src-head/lib" "$WORK/src-head/lib/xxhash.c" -o "$WORK/xxhash.s" \
    && awk '/^LZ4_XXH32_update:/, /\.size[ \t]+LZ4_XXH32_update/' "$WORK/xxhash.s" > "$ASM"

"$WORK/bin/ref" -q -1 -T1 "$INPUT" -c > "$WORK/crc.lz4" || die "compression failed"
"$WORK/bin/ref" -q -1 -T1 --no-frame-crc "$INPUT" -c > "$WORK/nocrc.lz4" || die "compression failed"
SIZE=$(wc -c < "$INPUT" | tr -d ' ')

speed() {   # speed PROGRAM FILE : MB/s of `lz4 -t`, median of 7
    local r t0 t1 d=""
    for r in 1 2 3 4 5 6 7; do
        t0=$EPOCHREALTIME; "$WORK/bin/$1" -q -t "$2" || die "$1 -t $2 failed"; t1=$EPOCHREALTIME
        d="$d ${t0/,/.} ${t1/,/.};"
    done
    printf '%s' "$d" | tr ';' '\n' | awk 'NF == 2 { print $2 - $1 }' | sort -n \
        | awk -v size="$SIZE" '{ v[NR] = $1 } END { printf "%.0f MB/s", size / v[int((NR + 1) / 2)] / 1e6 }'
}

{
    echo "host : $(uname -n), $(uname -sr), $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | sed 's/.*: //')"
    echo "$($CC --version | head -1), enabled by default : $($CC -dM -E -x c /dev/null | grep -oE '__(SSE4_1|AVX2|AVX512F)__' | tr '\n' ' ')"
    echo "ref = $REF ($(git -C "$REPO" rev-parse --short "$REF")), head = HEAD ($(git -C "$REPO" rev-parse --short HEAD)), input : $INPUT"
    echo
    for x in $XXB; do "$WORK/bin/$x"; done
    [ -s "$ASM" ] && echo "assembly of XXH32_update() (lz4's options) : $ASM"
    echo
    for p in ref head; do
        echo "$p -t : with content checksum $(speed $p "$WORK/crc.lz4"), without $(speed $p "$WORK/nocrc.lz4")"
    done
    if command -v perf >/dev/null; then
        for p in ref head; do
            echo
            echo "perf profile of '$p -t', content checksum, per thread (2nd column : thread id) :"
            perf record -q -o "$WORK/perf.data" -- "$WORK/bin/$p" -q -t "$WORK/crc.lz4" >/dev/null 2>&1 \
                || { echo "perf record failed (see /proc/sys/kernel/perf_event_paranoid)"; break; }
            perf report -i "$WORK/perf.data" --sort pid,sym --stdio 2>/dev/null | grep -v '^#' | grep -v '^ *$' | head -15
        done
    else
        echo
        echo "perf not found (Fedora : sudo dnf install perf, Ubuntu : sudo apt install linux-tools-generic) : no profile"
    fi
} | tee "profile-decompress-checksum-$(uname -n)-$(date +%Y%m%d-%H%M).txt"
