#!/bin/bash
set -e

SRC_FILE="${1}"
BUILD_DIR="${2}"
PASSES_STRING="${3}"

if [ -z "$SRC_FILE" ] || [ -z "$BUILD_DIR" ] || [ -z "$PASSES_STRING" ]; then
    echo "Usage: $0 <source_file> <build_dir> <passes_string>" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR"

PASS_LIB=./build/lib/ChakravyuhaPasses.so
LLVM_IR=${BUILD_DIR}/test_program.ll
OBFUSCATED_IR=${BUILD_DIR}/obfuscated.ll
FINAL_BINARY=${BUILD_DIR}/obfuscated_program
REPORT_FILE=${BUILD_DIR}/report.json
CFF_METRICS_FILE=${BUILD_DIR}/cff_metrics.json
TEMP_LOG=${BUILD_DIR}/obfuscation_output.log

if [ ! -f "$PASS_LIB" ]; then
    echo "Error: Pass library not found at $PASS_LIB" >&2
    exit 1
fi

clang -O0 -S -emit-llvm "$SRC_FILE" -o "$LLVM_IR"

opt -load-pass-plugin="$PASS_LIB" -passes="$PASSES_STRING" \
    "$LLVM_IR" -S -o "$OBFUSCATED_IR" >"$TEMP_LOG" 2>&1

grep "CFF_METRICS" "$TEMP_LOG" | sed 's/CFF_METRICS://' >"$CFF_METRICS_FILE" || true

grep -v "CFF_METRICS" "$TEMP_LOG" >"$REPORT_FILE" || true

clang "$OBFUSCATED_IR" -o "$FINAL_BINARY"

exit 0
