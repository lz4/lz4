#!/bin/bash

# ============================================================================
# test-lz4wrapper-basic.sh
# Basic test cases for lz4wrapper streaming compression/decompression
# ============================================================================

FPREFIX="tmp-lz4wrapper"

# Exit on error
set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test counter
TEST_NUM=0
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0

# ============================================================================
# Utility functions
# ============================================================================

print_pass() {
    printf "${GREEN}PASS${NC}: %s\n" "$1"
    PASS_COUNT=$((PASS_COUNT + 1))
}

print_fail() {
    printf "${RED}FAIL${NC}: %s\n" "$1"
    FAIL_COUNT=$((FAIL_COUNT + 1))
}

print_skip() {
    printf "${YELLOW}SKIP${NC}: %s\n" "$1"
    SKIP_COUNT=$((SKIP_COUNT + 1))
}

print_test() {
    TEST_NUM=$((TEST_NUM + 1))
    printf "\n=== Test %d: %s ===\n" "$TEST_NUM" "$1"
}

# Cleanup function
cleanup() {
    rm -rf ${FPREFIX}*
}

trap cleanup EXIT

# ============================================================================
# Test helper: roundtrip test
# Usage: test_roundtrip <size> <datagen_opts> <wrapper_opts> <description>
# ============================================================================

test_roundtrip() {
    local size="$1"
    local datagen_opts="$2"
    local wrapper_opts="$3"
    local desc="$4"

    print_test "Roundtrip: $desc (size=$size)"

    # Generate test data
    ./datagen -g${size} ${datagen_opts} > ${FPREFIX}-input

    # Compress
    ./lz4wrapper -c ${wrapper_opts} ${FPREFIX}-input ${FPREFIX}-compressed

    # Decompress
    ./lz4wrapper -d ${wrapper_opts} ${FPREFIX}-compressed ${FPREFIX}-output

    # Verify
    if cmp -s ${FPREFIX}-input ${FPREFIX}-output; then
        print_pass "$desc"
        rm -f ${FPREFIX}-input ${FPREFIX}-compressed ${FPREFIX}-output
        return 0
    else
        print_fail "$desc"
        return 1
    fi
}

# ============================================================================
# Test helper: batch roundtrip test
# Usage: test_batch_roundtrip <block_sizes> <datagen_opts> <wrapper_opts> <description>
# block_sizes is a space-separated list like "1K 16K 64K"
# ============================================================================

generate_batch_input() {
    local output_file="$1"
    shift
    local datagen_opts="$1"
    shift
    local block_sizes="$@"

    # Create temporary files for each block
    local block_count=0
    for size in $block_sizes; do
        ./datagen -g${size} ${datagen_opts} > ${FPREFIX}-block-${block_count}
        block_count=$((block_count + 1))
    done

    # Build batch format: block_count (4 bytes) + (block_size + block_data) for each block
    # Use a simple C program or Python to create binary format
    # For simplicity, we'll use printf and od
    
    # Write block count (little-endian 4 bytes)
    printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' \
        $((block_count & 0xFF)) \
        $(((block_count >> 8) & 0xFF)) \
        $(((block_count >> 16) & 0xFF)) \
        $(((block_count >> 24) & 0xFF)))" > "$output_file"

    # Write each block
    for i in $(seq 0 $((block_count - 1))); do
        local block_file="${FPREFIX}-block-${i}"
        local block_size=$(wc -c < "$block_file" | tr -d ' ')
        
        # Write block size (little-endian 4 bytes)
        printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' \
            $((block_size & 0xFF)) \
            $(((block_size >> 8) & 0xFF)) \
            $(((block_size >> 16) & 0xFF)) \
            $(((block_size >> 24) & 0xFF)))" >> "$output_file"
        
        # Write block data
        cat "$block_file" >> "$output_file"
        
        rm -f "$block_file"
    done
}

test_batch_roundtrip() {
    local desc="$1"
    local datagen_opts="$2"
    local wrapper_opts="$3"
    shift 3
    local block_sizes="$@"

    print_test "Batch Roundtrip: $desc"

    # Generate batch input
    generate_batch_input ${FPREFIX}-batch-input "$datagen_opts" $block_sizes

    # Compress
    ./lz4wrapperbatch -c ${wrapper_opts} ${FPREFIX}-batch-input ${FPREFIX}-batch-compressed

    # Decompress
    ./lz4wrapperbatch -d ${wrapper_opts} ${FPREFIX}-batch-compressed ${FPREFIX}-batch-output

    # Verify
    if cmp -s ${FPREFIX}-batch-input ${FPREFIX}-batch-output; then
        print_pass "$desc"
        rm -f ${FPREFIX}-batch-input ${FPREFIX}-batch-compressed ${FPREFIX}-batch-output
        return 0
    else
        print_fail "$desc"
        return 1
    fi
}

# ============================================================================
# Test helper: batch roundtrip test with random block sizes
# Usage: generate_batch_input_random <output_file> <block_count> <min_block_size> <max_block_size> <datagen_opts>
# Usage: test_batch_roundtrip_random <description> <datagen_opts> <wrapper_opts> <block_count> <min_block_size> <max_block_size>
# block_count is the number of blocks
# ============================================================================

generate_batch_input_random() {
    local output_file="$1"
    local block_count="$2"
    local min_block_size="$3"
    local max_block_size="$4"
    local datagen_opts="$5"
    local range=$((max_block_size - min_block_size + 1))

    # Write block count (little-endian 4 bytes)
    printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' \
        $((block_count & 0xFF)) \
        $(((block_count >> 8) & 0xFF)) \
        $(((block_count >> 16) & 0xFF)) \
        $(((block_count >> 24) & 0xFF)))" > "$output_file"

    for _ in $(seq 1 $block_count); do
        # Random block size
        local block_size=$(( $(od -An -tu4 -N4 /dev/urandom | tr -d ' ') % range + min_block_size ))
        
        # Write block size (little-endian 4 bytes)
        printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' \
            $((block_size & 0xFF)) \
            $(((block_size >> 8) & 0xFF)) \
            $(((block_size >> 16) & 0xFF)) \
            $(((block_size >> 24) & 0xFF)))" >> "$output_file"

        # Write block data
        ./datagen -g${block_size} ${datagen_opts} >> "$output_file"
    done
}

test_batch_roundtrip_random() {
    local desc="$1"
    local datagen_opts="$2"
    local wrapper_opts="$3"
    local block_count="$4"
    local min_block_size="$5"
    local max_block_size="$6"

    # Print test description
    print_test "Batch Roundtrip Random: $desc"

    # Generate batch input
    generate_batch_input_random ${FPREFIX}-batch-input-random "$block_count" "$min_block_size" "$max_block_size" "$datagen_opts"

    # Compress
    ./lz4wrapperbatch -c ${wrapper_opts} ${FPREFIX}-batch-input-random ${FPREFIX}-batch-compressed-random

    # Decompress
    ./lz4wrapperbatch -d ${wrapper_opts} ${FPREFIX}-batch-compressed-random ${FPREFIX}-batch-output-random
    
    # Verify
    if cmp -s ${FPREFIX}-batch-input-random ${FPREFIX}-batch-output-random; then
        print_pass "$desc"
        rm -f ${FPREFIX}-batch-input-random ${FPREFIX}-batch-compressed-random ${FPREFIX}-batch-output-random
        return 0
    else
        print_fail "$desc"
        return 1
    fi
}

# ============================================================================
# Main test execution
# ============================================================================

# Show debug output
# set -x

echo "============================================"
echo "LZ4 Wrapper Basic Tests"
echo "============================================"

# ----------------------------------------------------------------------------
# Section 1: Basic Roundtrip Tests (various sizes)
# ----------------------------------------------------------------------------

echo ""
echo ">>> Section 1: Basic Roundtrip Tests <<<"

# Small files
test_roundtrip "1KB" "-P50" "" "1KB medium compressibility"
test_roundtrip "16KB" "-P50" "" "16KB medium compressibility"

# Medium files
test_roundtrip "64KB" "-P50" "" "64KB medium compressibility"
test_roundtrip "128KB" "-P50" "" "128KB medium compressibility"

# Large files
test_roundtrip "1MB" "-P50" "" "1MB medium compressibility"
test_roundtrip "5MB" "-P50" "" "5MB medium compressibility"
test_roundtrip "5MB" "-P50" "" "10MB medium compressibility"

# ----------------------------------------------------------------------------
# Section 2: Compressibility Tests
# ----------------------------------------------------------------------------

echo ""
echo ">>> Section 2: Compressibility Tests <<<"

# High compressibility (-P99)
test_roundtrip "64KB" "-P99" "" "64KB high compressibility"
test_roundtrip "1MB" "-P99" "" "1MB high compressibility"

# Medium compressibility (-P50)
test_roundtrip "64KB" "-P50" "" "64KB medium compressibility"
test_roundtrip "1MB" "-P50" "" "1MB medium compressibility"

# Low compressibility / random (-P0)
test_roundtrip "64KB" "-P0" "" "64KB random data"
test_roundtrip "1MB" "-P0" "" "1MB random data"

# ----------------------------------------------------------------------------
# Section 3: Parameter Tests
# ----------------------------------------------------------------------------

echo ""
echo ">>> Section 3: Parameter Tests <<<"

# Custom min-work-area
test_roundtrip "128KB" "-P50" "-v --min-work-area=128K" "128KB with min-work-area=128K"

# Custom max-work-area
test_roundtrip "5MB" "-P50" "-v --max-work-area=8M" "5MB with max-work-area=8M"

# min-work-area = max-work-area
test_roundtrip "1KB" "-P50" "-v --min-work-area=64K --max-work-area=64K" "64KB with min=max=64K"

# min-work-area < 64KB
test_roundtrip "1KB" "-P50" "-v --min-work-area=32K" "32KB with min-work-area=64K"

# min-work-area > max-work-area
test_roundtrip "1KB" "-P50" "-v --min-work-area=128K --max-work-area=127K" "128KB with min-work-area=127K"

# data size is 0
# ./lz4wrapper -c < /dev/null > ${FPREFIX}-compressed

# ----------------------------------------------------------------------------
# Section 4: Shrink Mode Tests
# ----------------------------------------------------------------------------

echo ""
echo ">>> Section 4: Shrink Mode Tests <<<"

# Manual shrink mode (default)
test_roundtrip "1MB" "-P50" "-v --shrink-mode=manual" "1MB with SHRINK_MANUAL"

# Immediate shrink mode
test_roundtrip "1MB" "-P50" "-v --shrink-mode=immediate" "1MB with SHRINK_AUTO_IMMEDIATE"

# Threshold shrink mode
test_roundtrip "1MB" "-P50" "-v --shrink-mode=threshold --shrink-threshold=256K" "1MB with SHRINK_AUTO_THRESHOLD"

# ----------------------------------------------------------------------------
# Section 5: Stdin/Stdout Tests
# ----------------------------------------------------------------------------

echo ""
echo ">>> Section 5: Stdin/Stdout Tests <<<"

print_test "Stdin/Stdout roundtrip"

./datagen -g64KB -P50 > ${FPREFIX}-stdin-input
cat ${FPREFIX}-stdin-input | ./lz4wrapper -c > ${FPREFIX}-stdin-compressed
cat ${FPREFIX}-stdin-compressed | ./lz4wrapper -d > ${FPREFIX}-stdin-output

if cmp -s ${FPREFIX}-stdin-input ${FPREFIX}-stdin-output; then
    print_pass "Stdin/Stdout roundtrip"
    rm -f ${FPREFIX}-stdin-input ${FPREFIX}-stdin-compressed ${FPREFIX}-stdin-output
else
    print_fail "Stdin/Stdout roundtrip"
fi

# ----------------------------------------------------------------------------
# Section 6: Batch Tests
# ----------------------------------------------------------------------------

echo ""
echo ">>> Section 6: Batch Tests <<<"

# Multiple small blocks
test_batch_roundtrip "Multiple small blocks" "-P50" "" "1K" "2K" "4K" "8K" "16K"

# Mixed size blocks
test_batch_roundtrip "Mixed size blocks" "-P50" "" "1K" "64K" "128K" "32K" "16K"

# Larger blocks (trigger ring buffer behavior)
test_batch_roundtrip "Large blocks" "-P50" "" "64K" "64K" "64K" "64K"

# Blocks that should trigger ring buffer wraparound
test_batch_roundtrip "Ring buffer wraparound" "-P50" "--min-work-area=64K --max-work-area=256K" "32K" "32K" "32K" "32K" "32K" "32K" "32K" "32K"

# Batch with different shrink modes
test_batch_roundtrip "Batch with immediate shrink" "-P50" "--shrink-mode=immediate" "16K" "64K" "16K" "128K" "16K"

test_batch_roundtrip "Batch with threshold shrink" "-P50" "--shrink-mode=threshold --shrink-threshold=128K" "16K" "64K" "16K" "256K" "16K"

# ----------------------------------------------------------------------------
# Section 7: Verbose Output Tests
# ----------------------------------------------------------------------------

echo ""
echo ">>> Section 7: Verbose Output Tests <<<"

print_test "Verbose compress output"
./datagen -g1MB -P50 > ${FPREFIX}-verbose-input
./lz4wrapper -c -v ${FPREFIX}-verbose-input ${FPREFIX}-verbose-compressed 2>&1 | grep -q "Compressed"
if [ $? -eq 0 ]; then
    print_pass "Verbose compress output contains expected info"
else
    print_fail "Verbose compress output missing expected info"
fi

print_test "Verbose decompress output"
./lz4wrapper -d -v ${FPREFIX}-verbose-compressed ${FPREFIX}-verbose-output 2>&1 | grep -q "Decompressed"
if [ $? -eq 0 ]; then
    print_pass "Verbose decompress output contains expected info"
else
    print_fail "Verbose decompress output missing expected info"
fi

rm -f ${FPREFIX}-verbose-input ${FPREFIX}-verbose-compressed ${FPREFIX}-verbose-output

# Batch verbose
print_test "Batch verbose output"
generate_batch_input ${FPREFIX}-batch-verbose-input "-P50" "16K" "32K" "64K"
./lz4wrapperbatch -c -v ${FPREFIX}-batch-verbose-input ${FPREFIX}-batch-verbose-compressed 2>&1 | grep -q "Block"
if [ $? -eq 0 ]; then
    print_pass "Batch verbose output contains block info"
else
    print_fail "Batch verbose output missing block info"
fi
rm -f ${FPREFIX}-batch-verbose-input ${FPREFIX}-batch-verbose-compressed

# ----------------------------------------------------------------------------
# Section 8: Help and Error Tests
# ----------------------------------------------------------------------------

echo ""
echo ">>> Section 8: Help and Error Tests <<<"

print_test "Help flag"
./lz4wrapper --help > /dev/null 2>&1
if [ $? -eq 0 ]; then
    print_pass "Help flag works"
else
    print_fail "Help flag failed"
fi

print_test "Batch help flag"
./lz4wrapperbatch --help > /dev/null 2>&1
if [ $? -eq 0 ]; then
    print_pass "Batch help flag works"
else
    print_fail "Batch help flag failed"
fi

print_test "No mode specified should fail"
./lz4wrapper /dev/null /dev/null 2>/dev/null && print_fail "Should fail without mode" || print_pass "Correctly fails without mode"

print_test "Both modes specified should fail"
./lz4wrapper -c -d /dev/null /dev/null 2>/dev/null && print_fail "Should fail with both modes" || print_pass "Correctly fails with both modes"

# ----------------------------------------------------------------------------
# Section 9: Large file test (optional, can be slow)
# ----------------------------------------------------------------------------

echo ""
echo ">>> Section 9: Large File Test <<<"

test_roundtrip "10MB" "-P50" "" "10MB medium compressibility"


# ----------------------------------------------------------------------------
# Section 10: Random Batch Tests
# ----------------------------------------------------------------------------

echo ""
echo ">>> Section 10: Random Batch Tests <<<"

# 100 percent probability of not releasing memory
test_batch_roundtrip_random "Random batch: 100 percent probability of not releasing memory" "-P50" "--shrink-mode=manual" 10 1024 1024*1024

# 50 percent probability of releasing memory
test_batch_roundtrip_random "Random batch: 50 percent probability of releasing memory" "-P50" "--shrink-mode=threshold --shrink-threshold=512K" 10 1024 1024*1024

# 100 percent probability of releasing memory immediately
test_batch_roundtrip_random "Random batch: 100 percent probability of releasing memory immediately" "-P50" "--shrink-mode=immediate" 10 1024 1024*1024

# real world test
# test_batch_roundtrip_random "Random batch: 100 percent probability of releasing memory immediately" "-P50" "-v --shrink-mode=immediate" 10000 200 1024

# ----------------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------------

echo ""
echo "============================================"
echo "Test Summary"
echo "============================================"
printf "${GREEN}PASS${NC}: %d\n" "$PASS_COUNT"
printf "${RED}FAIL${NC}: %d\n" "$FAIL_COUNT"
printf "${YELLOW}SKIP${NC}: %d\n" "$SKIP_COUNT"
echo "Total: $TEST_NUM tests"

if [ $FAIL_COUNT -gt 0 ]; then
    exit 1
fi

exit 0

