#!/bin/bash
# test-lz4-rvv.sh: Test script specifically for RVV optimizations
# Usage: ./test-lz4-rvv.sh

set -e

LZ4="../programs/lz4"
DATAGEN="./datagen"

# Check if RVV is available
if ! cat /proc/cpuinfo | grep -q "isa.*v" 2>/dev/null; then
    echo "Warning: RISC-V Vector Extension not detected in /proc/cpuinfo"
    echo "Continuing with compilation test only..."
fi

# Test compilation with RVV flags
echo "Testing RVV compilation..."
CFLAGS="-march=rv64gcv -DLZ4_RVV_ENABLED=1" make -C ../lib clean all
CFLAGS="-march=rv64gcv -DLZ4_RVV_ENABLED=1" make -C ../programs clean lz4

# Basic functionality tests
echo "Testing RVV-optimized LZ4 functionality..."

# Test with various data sizes to trigger different RVV code paths
for size in 31 32 33 63 64 65 127 128 129 255 256 257 1023 1024 1025 4095 4096 4097; do
    echo "Testing size: $size bytes"
    $DATAGEN -g$size > tmp_test_$size
    $LZ4 -f tmp_test_$size tmp_test_${size}.lz4
    $LZ4 -df tmp_test_${size}.lz4 tmp_test_${size}.dec
    diff tmp_test_$size tmp_test_${size}.dec
    rm -f tmp_test_$size tmp_test_${size}.lz4 tmp_test_${size}.dec
done

# Test with highly repetitive data (triggers pattern optimization)
echo "Testing RVV pattern optimization..."
printf '%*s' 1000 '' | tr ' ' 'A' > tmp_pattern_test
$LZ4 -f tmp_pattern_test tmp_pattern_test.lz4
$LZ4 -df tmp_pattern_test.lz4 tmp_pattern_test.dec
diff tmp_pattern_test tmp_pattern_test.dec
rm -f tmp_pattern_test tmp_pattern_test.lz4 tmp_pattern_test.dec

# Test HC compression
echo "Testing RVV HC compression..."
$DATAGEN -g8192 > tmp_hc_test
$LZ4 -f -9 tmp_hc_test tmp_hc_test.lz4
$LZ4 -df tmp_hc_test.lz4 tmp_hc_test.dec  
diff tmp_hc_test tmp_hc_test.dec
rm -f tmp_hc_test tmp_hc_test.lz4 tmp_hc_test.dec

echo "RVV optimization tests completed successfully!"