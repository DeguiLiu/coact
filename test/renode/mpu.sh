#!/usr/bin/env bash
# Build and run the Cortex-M4 MPU capability matrix (stage-0 platform evidence).
#
# Two gates, because they answer different questions:
#
#   1. The harness itself must not regress: no FAIL line, and a
#      "RESULT MPU PASS n/n" verdict from the target.
#   2. The observed capability set must match the frozen expectation in
#      mpu_caps.expected. A model that silently stops enforcing XN, or starts
#      enforcing something new, changes what Renode may be cited for, so the
#      diff is a human decision rather than a pass.
#
# Capability lines are reported as CAP <name> ENFORCED|NOT_ENFORCED
# required|informational. "required" means design 5.1/8.1 depends on it; the
# summary counts how many of those this model actually enforces, which is the
# number the platform gate records - not a claim about real silicon.
#
# Exit codes: 0 both gates pass, 1 harness/build failure or capability drift.
set -u

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
BUILD_DIR="$SCRIPT_DIR/build"
RENODE=${RENODE:-/home/dgliu/renode/renode}
CC=${CC:-arm-none-eabi-gcc}
CXX=${CXX:-arm-none-eabi-g++}
EXPECTED="$SCRIPT_DIR/mpu_caps.expected"

COMMON_FLAGS=(
    -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
    -ffreestanding -fno-unwind-tables -fno-asynchronous-unwind-tables
    -Os -g -Wall -Wextra -I"$SCRIPT_DIR"
)
CXX_FLAGS=(
    -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics
    -fno-use-cxa-atexit -DCOACT_ATOMIC64_SHIM
)

fail() {
    echo "MPU HARNESS ERROR: $*" >&2
    exit 1
}

mkdir -p "$BUILD_DIR" || fail "cannot create $BUILD_DIR"
exec 9> "$BUILD_DIR/.mpulock" || fail "cannot open build lock"
flock 9 || fail "cannot lock $BUILD_DIR"

echo "== build (MPU capability matrix) =="
"$CC" "${COMMON_FLAGS[@]}" -c "$SCRIPT_DIR/start.c" -o "$BUILD_DIR/mpu_start.o" \
    || fail "startup compile failed"
"$CC" "${COMMON_FLAGS[@]}" -c "$SCRIPT_DIR/runtime.c" \
    -o "$BUILD_DIR/mpu_runtime.o" || fail "runtime compile failed"
"$CXX" "${COMMON_FLAGS[@]}" "${CXX_FLAGS[@]}" -c "$SCRIPT_DIR/mpu_main.cpp" \
    -o "$BUILD_DIR/mpu_main.o" || fail "fixture compile failed"
"$CXX" "${COMMON_FLAGS[@]}" "${CXX_FLAGS[@]}" -nostdlib -nostartfiles \
    -Wl,--gc-sections -Wl,-T,"$SCRIPT_DIR/link.ld" \
    "$BUILD_DIR/mpu_start.o" "$BUILD_DIR/mpu_runtime.o" "$BUILD_DIR/mpu_main.o" \
    -lgcc -o "$BUILD_DIR/coact_mpu.elf" || fail "link failed"
arm-none-eabi-size "$BUILD_DIR/coact_mpu.elf" || true

MPU_LOG="$BUILD_DIR/mpu.log"
RENODE_LOG="$BUILD_DIR/mpu_renode.log"
: > "$MPU_LOG"

echo "== run (Renode, bounded 200 ms virtual time) =="
cd "$REPO_ROOT" || fail "cannot cd to $REPO_ROOT"
timeout 120 "$RENODE" --console --disable-gui --plain \
    -e "include @$SCRIPT_DIR/coact_mpu.resc" > "$RENODE_LOG" 2>&1
RC=$?
if [ "$RC" -ne 0 ]; then
    tail -n 30 "$RENODE_LOG" >&2
    fail "Renode exited with rc=$RC"
fi
if [ ! -s "$MPU_LOG" ]; then
    tail -n 30 "$RENODE_LOG" >&2
    fail "no target output captured in $MPU_LOG"
fi

echo "== target output =="
cat "$MPU_LOG"

CLEAN=$(tr -d '\r' < "$MPU_LOG")

# --- Gate 1: the harness verdict -------------------------------------------
if printf '%s\n' "$CLEAN" | grep -q '^FAIL '; then
    echo "MPU RESULT: FAIL (target reported FAIL)" >&2
    exit 1
fi
RESULT_LINE=$(printf '%s\n' "$CLEAN" | grep '^RESULT MPU ' | tail -n 1)
if [ -z "$RESULT_LINE" ]; then
    echo "MPU RESULT: FAIL (missing 'RESULT MPU' line)" >&2
    exit 1
fi
COUNTS=${RESULT_LINE#RESULT MPU PASS }
if [ "$COUNTS" = "$RESULT_LINE" ]; then
    echo "MPU RESULT: FAIL ($RESULT_LINE)" >&2
    exit 1
fi
PASSED=${COUNTS%%/*}
TOTAL=${COUNTS##*/}
if [ "$PASSED" != "$TOTAL" ]; then
    echo "MPU RESULT: FAIL ($PASSED/$TOTAL)" >&2
    exit 1
fi

# --- Gate 2: capability drift against the frozen expectation ---------------
OBSERVED=$(mktemp "${TMPDIR:-/tmp}/coact-mpu-caps.XXXXXX")
trap 'rm -f "$OBSERVED"' EXIT
printf '%s\n' "$CLEAN" | grep '^CAP ' | sort > "$OBSERVED"

if [ ! -f "$EXPECTED" ]; then
    echo "MPU RESULT: FAIL (no frozen expectation at $EXPECTED)" >&2
    echo "Observed capabilities:" >&2
    cat "$OBSERVED" >&2
    exit 1
fi
if ! diff -u "$EXPECTED" "$OBSERVED"; then
    echo "MPU RESULT: FAIL (capability drift vs $(basename "$EXPECTED"))" >&2
    echo "Renode no longer proves the same set of access controls." >&2
    echo "Re-derive the design claim for the changed row, then freeze it." >&2
    exit 1
fi

printf '%s\n' "$CLEAN" | grep '^MPU SUMMARY'
echo "MPU RESULT: PASS ($PASSED/$TOTAL, capabilities match $(basename "$EXPECTED"))"
