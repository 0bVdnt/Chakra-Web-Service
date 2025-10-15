#!/bin/bash
set -e

# Arguments from server.js
SRC_FILE="$1"
BUILD_DIR="$2"
PIPELINE="$3"
CYCLES="$4"
GENERATE_CFG="$5"
FILENAME="$6"

# --- Path setup ---
PASS_PLUGIN_PATH="/app/build/lib/ChakravyuhaPasses.so"
LL_DIR="${BUILD_DIR}/ll_files"
DOT_DIR="${BUILD_DIR}/dot_files"
ORIGINAL_IR="${LL_DIR}/test_program_original.ll"
FINAL_OBFUSCATED_IR="${LL_DIR}/test_program_obfuscated.ll"
ORIGINAL_BINARY="${BUILD_DIR}/original_program"
FINAL_BINARY="${BUILD_DIR}/obfuscated_program"
REPORT_FILE="${BUILD_DIR}/report.json"
SIZE_METRICS_FILE="${BUILD_DIR}/size_metrics.json"

mkdir -p "$LL_DIR" "$DOT_DIR/original" "$DOT_DIR/obfuscated"

# --- Pass pipeline selection ---
PIPELINE_PASSES=""
case "$PIPELINE" in
    cff)    PIPELINE_PASSES="chakravyuha-initial-metrics,chakravyuha-control-flow-flatten";;
    string) PIPELINE_PASSES="chakravyuha-initial-metrics,chakravyuha-string-encrypt";;
    fake)   PIPELINE_PASSES="chakravyuha-initial-metrics,chakravyuha-fake-code-insertion";;
    full)   PIPELINE_PASSES="chakravyuha-initial-metrics,chakravyuha-string-encrypt,chakravyuha-control-flow-flatten,chakravyuha-fake-code-insertion";;
    *)      echo "Error: Invalid pipeline '$PIPELINE' specified." >&2; exit 1;;
esac

# --- Compiler Auto-Selection ---
COMPILER="clang"
if [[ "$FILENAME" == *.cpp ]] || [[ "$FILENAME" == *.cxx ]] || [[ "$FILENAME" == *.cc ]]; then
    COMPILER="clang++"
fi
echo "Selected compiler based on '$FILENAME': $COMPILER"

# 1. Compile source to LLVM IR
"$COMPILER" -O0 -S -emit-llvm "$SRC_FILE" -o "$ORIGINAL_IR"

# 2. Run obfuscation cycles
CURRENT_IR=$ORIGINAL_IR
for i in $(seq 1 "$CYCLES"); do 
  NEXT_IR="${BUILD_DIR}/temp_ir.$i.ll"
  opt -load-pass-plugin="$PASS_PLUGIN_PATH" \
      -passes="${PIPELINE_PASSES},chakravyuha-emit-report" \
      "$CURRENT_IR" -S -o "$NEXT_IR" 2> "$REPORT_FILE"
  if [ "$CURRENT_IR" != "$ORIGINAL_IR" ]; then
    rm "$CURRENT_IR"
  fi
  CURRENT_IR=$NEXT_IR
done
mv "$CURRENT_IR" "$FINAL_OBFUSCATED_IR"

# 3. Compile BOTH original and obfuscated IR to binaries
"$COMPILER" "$ORIGINAL_IR" -o "$ORIGINAL_BINARY"
"$COMPILER" "$FINAL_OBFUSCATED_IR" -o "$FINAL_BINARY"

# 4. Get binary file sizes and write to a metrics file for the server
ORIG_SIZE=$(stat -c %s "$ORIGINAL_BINARY")
OBF_SIZE=$(stat -c %s "$FINAL_BINARY")
echo "{\"originalSize\": $ORIG_SIZE, \"obfuscatedSize\": $OBF_SIZE}" > "$SIZE_METRICS_FILE"

# 5. (Optional) Generate DOT graph files
if [ "$GENERATE_CFG" = "true" ]; then
    echo "Generating .dot files for CFG..."
    (cd "$DOT_DIR/original" && opt -passes=dot-cfg "$ORIGINAL_IR" -o /dev/null &> /dev/null)
    (cd "$DOT_DIR/obfuscated" && opt -passes=dot-cfg "$FINAL_OBFUSCATED_IR" -o /dev/null &> /dev/null)
    echo "Finished generating .dot files."
fi

exit 0