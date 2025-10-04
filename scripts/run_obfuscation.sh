set -e

SRC_FILE="${1:-./test_program.c}"
BUILD_DIR="${2:-./build}"
mkdir -p "$BUILD_DIR"

PASS_LIB=./build/lib/ChakravyuhaStringEncryptionPass.so
LLVM_IR=${BUILD_DIR}/test_program.ll
OBFUSCATED_IR=${BUILD_DIR}/obfuscated.ll
FINAL_BINARY=${BUILD_DIR}/obfuscated_program
REPORT_FILE=${BUILD_DIR}/report.json

if [ ! -f "$PASS_LIB" ]; then
    echo "Error: Pass library not found at $PASS_LIB" >&2
    exit 1
fi

clang -O0 -S -emit-llvm "$SRC_FILE" -o "$LLVM_IR"

opt -load-pass-plugin="$PASS_LIB" -passes=chakravyuha-string-encrypt \
    "$LLVM_IR" -S -o "$OBFUSCATED_IR" > "$REPORT_FILE"

clang "$OBFUSCATED_IR" -o "$FINAL_BINARY"

exit 0