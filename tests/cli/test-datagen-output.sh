#!/bin/sh

set -eu
FPREFIX=tmp-dgo
trap 'rm -f "$FPREFIX"*' EXIT

for mode in "" "-P0" "-P100"; do
    datagen -g17 $mode > "$FPREFIX-out"
    test "$(wc -c < "$FPREFIX-out")" -eq 17
    if [ "$(uname)" = Linux ] && [ -w /dev/full ]; then
        for size in 1 65536; do
            rc=0
            datagen -g$size $mode > /dev/full 2> "$FPREFIX-err" || rc=$?
            test "$rc" -gt 0
            test "$rc" -lt 128
            grep -q 'Error writing generated data' "$FPREFIX-err"
        done
    fi
done
