#!/usr/bin/env bash
# Build and run the watchdog fixture on the Renode Cortex-M4 model.
#
# What this proves, and how: a real AO action dispatched through the
# coordinator never returns, so no software budget can report it. The
# independent watchdog (IWDG) must terminate it. Boot 1 samples a free-running
# counter into .noinit from inside the runaway loop; boot 2 reads that sample
# back as the measured termination latency and then runs a control loop that
# kicks the same watchdog for 100 ms without being reset.
#
# The verdict comes from the target, not from a wall-clock timeout: using
# "the script gave up" as evidence of bounded termination is exactly the
# mistake design 8.1 forbids.
#
# Exit codes: 0 all assertions PASS, 1 build/run/assertion failure.
set -u

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
BUILD_DIR="$SCRIPT_DIR/build"
RENODE=${RENODE:-/home/dgliu/renode/renode}
CC=${CC:-arm-none-eabi-gcc}
CXX=${CXX:-arm-none-eabi-g++}

COMMON_FLAGS=(
    -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
    -ffreestanding -fno-unwind-tables -fno-asynchronous-unwind-tables
    -Os -g -Wall -Wextra -I"$REPO_ROOT/include" -I"$SCRIPT_DIR"
)
CXX_FLAGS=(
    -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics
    -fno-use-cxa-atexit -DCOACT_ATOMIC64_SHIM
)

fail() {
    echo "WDG HARNESS ERROR: $*" >&2
    exit 1
}

mkdir -p "$BUILD_DIR" || fail "cannot create $BUILD_DIR"
exec 9> "$BUILD_DIR/.wdglock" || fail "cannot open build lock"
flock 9 || fail "cannot lock $BUILD_DIR"

echo "== build (watchdog fixture) =="
"$CC" "${COMMON_FLAGS[@]}" -c "$SCRIPT_DIR/start.c" -o "$BUILD_DIR/wdg_start.o" \
    || fail "startup compile failed"
"$CC" "${COMMON_FLAGS[@]}" -c "$SCRIPT_DIR/runtime.c" \
    -o "$BUILD_DIR/wdg_runtime.o" || fail "runtime compile failed"
"$CXX" "${COMMON_FLAGS[@]}" "${CXX_FLAGS[@]}" -c "$SCRIPT_DIR/wdg_main.cpp" \
    -o "$BUILD_DIR/wdg_main.o" || fail "fixture compile failed"
"$CXX" "${COMMON_FLAGS[@]}" "${CXX_FLAGS[@]}" -nostdlib -nostartfiles \
    -Wl,--gc-sections -Wl,-T,"$SCRIPT_DIR/link.ld" \
    "$BUILD_DIR/wdg_start.o" "$BUILD_DIR/wdg_runtime.o" "$BUILD_DIR/wdg_main.o" \
    -lgcc -o "$BUILD_DIR/coact_wdg.elf" || fail "link failed"
arm-none-eabi-size "$BUILD_DIR/coact_wdg.elf" || true

# .noinit must be NOLOAD, or LoadELF would rewrite it on the reset macro and
# the boot counter could never reach 2 for the right reason.
if arm-none-eabi-objdump -h "$BUILD_DIR/coact_wdg.elf" \
        | awk '$2 == ".noinit" { print $3 }' | grep -qv '^0*0$'; then
    :
else
    fail ".noinit section missing or empty; the linker script lost it"
fi

WDG_LOG="$BUILD_DIR/wdg.log"
: > "$WDG_LOG"
RENODE_LOG="$BUILD_DIR/wdg_renode.log"

echo "== run (Renode, bounded 300 ms virtual time) =="
cd "$REPO_ROOT" || fail "cannot cd to $REPO_ROOT"
timeout 120 "$RENODE" --console --disable-gui --plain \
    -e "include @$SCRIPT_DIR/coact_wdg.resc" > "$RENODE_LOG" 2>&1
RC=$?
if [ "$RC" -ne 0 ]; then
    tail -n 30 "$RENODE_LOG" >&2
    fail "Renode exited with rc=$RC"
fi
if [ ! -s "$WDG_LOG" ]; then
    tail -n 30 "$RENODE_LOG" >&2
    fail "no target output captured in $WDG_LOG"
fi

echo "== target output =="
cat "$WDG_LOG"
echo "== renode watchdog evidence =="
grep -i "watchdog reset" "$RENODE_LOG" | head -n 3

# --- Gate on the target's own verdict --------------------------------------
if grep -q '^FAIL ' "$WDG_LOG"; then
    echo "WDG RESULT: FAIL (target reported FAIL)" >&2
    exit 1
fi
RESULT_LINE=$(tr -d '\r' < "$WDG_LOG" | grep '^RESULT WDG ' | tail -n 1)
if [ -z "$RESULT_LINE" ]; then
    echo "WDG RESULT: FAIL (missing 'RESULT WDG' line)" >&2
    exit 1
fi
COUNTS=${RESULT_LINE#RESULT WDG PASS }
if [ "$COUNTS" = "$RESULT_LINE" ]; then
    echo "WDG RESULT: FAIL ($RESULT_LINE)" >&2
    exit 1
fi
PASSED=${COUNTS%%/*}
TOTAL=${COUNTS##*/}
if [ "$PASSED" != "$TOTAL" ]; then
    echo "WDG RESULT: FAIL ($PASSED/$TOTAL)" >&2
    exit 1
fi
# Exactly one reset: a second one would mean the control loop was reset too,
# and a third boot line would prove the healthy path is not actually healthy.
if tr -d '\r' < "$WDG_LOG" | grep -q '^WDG BOOT n=3'; then
    echo "WDG RESULT: FAIL (target reset more than once)" >&2
    exit 1
fi
if ! grep -qi "watchdog reset" "$RENODE_LOG"; then
    echo "WDG RESULT: FAIL (Renode never reported a watchdog reset)" >&2
    exit 1
fi

echo "WDG RESULT: PASS ($PASSED/$TOTAL)"
