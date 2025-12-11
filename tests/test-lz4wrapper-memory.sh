#!/bin/bash

# ============================================================================
# test-lz4wrapperlsan-memory.sh
# Memory leak detection tests for lz4wrapperlsan
# ============================================================================

FPREFIX="tmp-lz4wrapperlsan-memory"

# Exit on error
set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test counter
TEST_NUM=0
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0

# Detect OS and available tools
OS=$(uname -s)
HAS_VALGRIND=false
HAS_ASAN=false
HAS_LEAKS=false

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
    printf "\n${BLUE}=== Test %d: %s ===${NC}\n" "$TEST_NUM" "$1"
}

print_info() {
    printf "${YELLOW}INFO${NC}: %s\n" "$1"
}

# Cleanup function
cleanup() {
    rm -rf ${FPREFIX}*
    rm -rf *.log
}

trap cleanup EXIT

# ============================================================================
# Tool detection
# ============================================================================

HAS_LLVM_LSAN=false
LLVM_CLANGXX=""

detect_tools() {
    print_test "LLVM LeakSanitizer (Homebrew Clang)"
    
    # Check for Homebrew LLVM
    LLVM_CLANG=""
    for path in /opt/homebrew/opt/llvm/bin/clang++ /usr/local/opt/llvm/bin/clang++; do
        if [ -x "$path" ]; then
            LLVM_CLANG="$path"
            break
        fi
    done
    
    if [ -z "$LLVM_CLANG" ]; then
        print_skip "Homebrew LLVM not found. Install with: brew install llvm"
        echo "  After installation, this method WILL detect memory leaks on macOS"
        return
    fi
    
    print_info "Found LLVM Clang: $LLVM_CLANG"
}

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
    ./lz4wrapperlsan -c ${wrapper_opts} ${FPREFIX}-input ${FPREFIX}-compressed

    # Decompress
    ./lz4wrapperlsan -d ${wrapper_opts} ${FPREFIX}-compressed ${FPREFIX}-output

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
    ./lz4wrapperbatchlsan -c ${wrapper_opts} ${FPREFIX}-batch-input-random ${FPREFIX}-batch-compressed-random

    # Decompress
    ./lz4wrapperbatchlsan -d ${wrapper_opts} ${FPREFIX}-batch-compressed-random ${FPREFIX}-batch-output-random
    
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

detect_tools

# ----------------------------------------------------------------------------
# Test lz4wrapperlsan
# ----------------------------------------------------------------------------

echo ""
echo ">>> Test lz4wrapperlsan <<<"

# Small files
test_roundtrip "16KB" "-P50" "" "16KB medium compressibility"

# Medium files
test_roundtrip "128KB" "-P50" "" "128KB medium compressibility"

# Large files
test_roundtrip "1MB" "-P50" "" "1MB medium compressibility"

# ----------------------------------------------------------------------------
# Test lz4wrapperbatchlsan
# ----------------------------------------------------------------------------

echo ""
echo ">>> Test lz4wrapperbatchlsan <<<"

# 100 percent probability of not releasing memory
test_batch_roundtrip_random "Random batch: 100 percent probability of not releasing memory" "-P50" "--shrink-mode=manual" 10 1024 1024*1024

# 50 percent probability of releasing memory
test_batch_roundtrip_random "Random batch: 50 percent probability of releasing memory" "-P50" "--shrink-mode=threshold --shrink-threshold=512K" 10 1024 1024*1024

# 100 percent probability of releasing memory immediately
test_batch_roundtrip_random "Random batch: 100 percent probability of releasing memory immediately" "-P50" "--shrink-mode=immediate" 10 1024 1024*1024

# ----------------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------------

echo ""
echo "============================================"
echo "Memory Test Summary"
echo "============================================"
printf "${GREEN}PASS${NC}: %d\n" "$PASS_COUNT"
printf "${RED}FAIL${NC}: %d\n" "$FAIL_COUNT"  
printf "${YELLOW}SKIP${NC}: %d\n" "$SKIP_COUNT"
echo "Total: $TEST_NUM tests"

# Restore original binaries
# print_info "Restoring original binaries..."
# make clean > /dev/null 2>&1
# make lz4wrapperlsan lz4wrapperbatchlsan > /dev/null 2>&1

if [ $FAIL_COUNT -gt 0 ]; then
    exit 1
fi

exit 0