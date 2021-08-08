#!/bin/bash

# `unicode_lint.sh' determines whether source files under the ./lib/ and ./programs/ directories
# contain Unicode characters, and fails if any do.
#
# See https://github.com/lz4/lz4/issues/1018

pass=true

# Scan ./lib/ for Unicode in source (*.c, *.h) files
result=$(
	find ./lib/ -regex '.*\.\(c\|h\)$' -exec grep -P -n "[^\x00-\x7F]" {} \; -exec echo "FAIL: {}" \;
)
if [[ $result ]]; then
	echo "$result"
	pass=false
fi

# Scan ./programs/ for Unicode in source (*.c, *.h) files
result=$(
	find ./programs/ -regex '.*\.\(c\|h\)$' -exec grep -P -n "[^\x00-\x7F]" {} \; -exec echo "{}: FAIL" \;
)
if [[ $result ]]; then
	echo "$result"
	pass=false
fi

if [ "$pass" = true ]; then
	echo "All tests successful."
	echo "Result: PASS"
	exit 0
else
	echo "Result: FAIL"
	exit 1
fi
