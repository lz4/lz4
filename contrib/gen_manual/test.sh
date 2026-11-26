#!/bin/sh

set -eu
GEN=$1
HEADER=$2
OUT=tmp-gen-manual-$$.html
trap 'rm -f "$OUT"' EXIT

"$GEN" test-version "$HEADER" "$OUT"
test -s "$OUT"
grep -q 'test-version Manual' "$OUT"

# Check delayed write errors on Linux, where /dev/full has these semantics.
if [ "$(uname)" = Linux ] && [ -w /dev/full ]; then
    if "$GEN" test-version "$HEADER" /dev/full; then
        echo 'Writing to a full device must fail'
        exit 1
    fi
fi
