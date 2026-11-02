#!/usr/bin/env bash
# Build and run the coact FreeRTOS PAL fixture on the Renode Cortex-M4 model.
#
# Uses the real FreeRTOS Kernel V11.1.0 (Cortex-M3 port) vendored under
# test/renode/freertos, so the evidence covers SVC/PendSV scheduling, SysTick,
# a real TIM2 interrupt entering the ISR branch, and the PAL's static objects
# on the target ABI.
#
# Exit codes: 0 all assertions PASS, 1 failure/harness error.
set -u

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
BUILD_DIR=${COACT_FREERTOS_RENODE_BUILD_DIR:-"$SCRIPT_DIR/build-freertos"}
FREERTOS_DIR="$SCRIPT_DIR/freertos"
RENODE=${RENODE:-/home/dgliu/renode/renode}
CC=${CC:-arm-none-eabi-gcc}
CXX=${CXX:-arm-none-eabi-g++}
COACT_INCLUDE_DIR=${COACT_INCLUDE_DIR:-$REPO_ROOT/include}
EXPECTED_TOTAL=32

COMMON_FLAGS=(
    -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
    -ffreestanding -fno-unwind-tables -fno-asynchronous-unwind-tables
    -Os -g -Wall -I"$FREERTOS_DIR" -I"$FREERTOS_DIR/include"
    -I"$FREERTOS_DIR/port" -I"$SCRIPT_DIR" -I"$COACT_INCLUDE_DIR"
)
CXX_FLAGS=(
    -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics
    -fno-use-cxa-atexit
)

fail() {
    echo "FREERTOS HARNESS ERROR: $*" >&2
    exit 1
}

mkdir -p "$BUILD_DIR" || fail "cannot create $BUILD_DIR"
exec 9> "$BUILD_DIR/.lock" || fail "cannot open lock"
flock 9 || fail "cannot lock $BUILD_DIR"

echo "== build (FreeRTOS V11.1.0, Cortex-M3 port on Cortex-M4) =="
for src in kernel/tasks.c kernel/queue.c kernel/list.c port/port.c; do
    obj="$BUILD_DIR/$(basename "${src%.c}").o"
    "$CC" "${COMMON_FLAGS[@]}" -c "$FREERTOS_DIR/$src" -o "$obj" \
        || fail "compile $src"
done
"$CC" "${COMMON_FLAGS[@]}" -c "$SCRIPT_DIR/start_freertos.c" \
    -o "$BUILD_DIR/start_freertos.o" || fail "startup compile"
"$CC" "${COMMON_FLAGS[@]}" -c "$SCRIPT_DIR/runtime.c" \
    -o "$BUILD_DIR/runtime.o" || fail "runtime compile"
"$CXX" "${COMMON_FLAGS[@]}" "${CXX_FLAGS[@]}" \
    -c "$REPO_ROOT/src/core/pal_freertos.cpp" -o "$BUILD_DIR/pal_freertos.o" \
    || fail "pal_freertos compile"
"$CXX" "${COMMON_FLAGS[@]}" "${CXX_FLAGS[@]}" \
    -c "$SCRIPT_DIR/freertos_main.cpp" -o "$BUILD_DIR/freertos_main.o" \
    || fail "harness compile"
"$CXX" "${COMMON_FLAGS[@]}" "${CXX_FLAGS[@]}" -nostdlib -nostartfiles \
    -Wl,--gc-sections -Wl,-T,"$SCRIPT_DIR/link.ld" \
    "$BUILD_DIR/start_freertos.o" "$BUILD_DIR/runtime.o" \
    "$BUILD_DIR/tasks.o" "$BUILD_DIR/queue.o" "$BUILD_DIR/list.o" \
    "$BUILD_DIR/port.o" "$BUILD_DIR/pal_freertos.o" \
    "$BUILD_DIR/freertos_main.o" -lgcc \
    -o "$BUILD_DIR/coact_freertos.elf" || fail "link failed"
arm-none-eabi-size "$BUILD_DIR/coact_freertos.elf" || true

# The admission path must not pull in 64-bit compare-exchange from libatomic.
CAS_CALLS=$(arm-none-eabi-objdump -d "$BUILD_DIR/coact_freertos.elf" \
    | grep -c "bl.*__atomic_compare_exchange_8")
echo "FR.info cas8_calls=$CAS_CALLS"

UART_LOG="$BUILD_DIR/uart.log"
: > "$UART_LOG"
RESC="$BUILD_DIR/freertos.resc"
sed -e "s|@FREERTOS_UART@|@$UART_LOG|" \
    -e "s|@FREERTOS_ELF@|@$BUILD_DIR/coact_freertos.elf|" \
    "$SCRIPT_DIR/coact_freertos.resc" > "$RESC" || fail "cannot write $RESC"

echo "== run (Renode, bounded 1 s virtual time) =="
cd "$REPO_ROOT" || fail "cannot cd to $REPO_ROOT"
timeout 100 "$RENODE" --console --disable-gui --plain \
    -e "include @$RESC" > "$BUILD_DIR/renode.log" 2>&1
RENODE_RC=$?
if [ "$RENODE_RC" -ne 0 ]; then
    tail -n 30 "$BUILD_DIR/renode.log" >&2
    fail "Renode exited with rc=$RENODE_RC"
fi
if [ ! -s "$UART_LOG" ]; then
    tail -n 30 "$BUILD_DIR/renode.log" >&2
    fail "no target output captured"
fi

echo "== target output =="
cat "$UART_LOG"

if [ "$CAS_CALLS" -ne 0 ]; then
    echo "FREERTOS HARNESS ERROR: 64-bit compare-exchange call site present" >&2
    exit 1
fi
if grep -q '^FAIL ' "$UART_LOG"; then
    echo "FREERTOS RESULT: FAIL (target reported FAIL)" >&2
    exit 1
fi
RESULT_LINE=$(tr -d '\r' < "$UART_LOG" | grep '^RESULT PASS ' | tail -n 1)
if [ -z "$RESULT_LINE" ]; then
    echo "FREERTOS RESULT: FAIL (missing RESULT PASS)" >&2
    exit 1
fi
COUNTS=${RESULT_LINE#RESULT PASS }
PASSED=${COUNTS%%/*}
TOTAL=${COUNTS##*/}
if [ "$PASSED" != "$TOTAL" ] || [ "$TOTAL" -ne "$EXPECTED_TOTAL" ]; then
    echo "FREERTOS RESULT: FAIL ($PASSED/$TOTAL, expected $EXPECTED_TOTAL)" >&2
    exit 1
fi
echo "FREERTOS RESULT: PASS ($PASSED/$TOTAL)"
