#!/usr/bin/env bash
# Build two Cortex-M4 FreeRTOS images and report flash/RAM deltas:
#   baseline: scheduler + one static task + one counting semaphore + UART
#   coact   : same boot task, one AO/HSM, default Config, one dispatcher
#
# Both images use the vendored FreeRTOS kernel, the same startup code, linker
# script and compiler flags, so the delta is the coact runtime cost.
#
# Exit codes: 0 both images built and ran, 1 harness error.
set -u

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
BUILD_DIR=${COACT_FOOTPRINT_BUILD_DIR:-"$SCRIPT_DIR/build-footprint"}
FREERTOS_DIR="$SCRIPT_DIR/freertos"
RENODE=${RENODE:-/home/dgliu/renode/renode}
CC=${CC:-arm-none-eabi-gcc}
CXX=${CXX:-arm-none-eabi-g++}
COACT_INCLUDE_DIR=${COACT_INCLUDE_DIR:-$REPO_ROOT/include}

COMMON_FLAGS=(
    -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
    -ffreestanding -fno-unwind-tables -fno-asynchronous-unwind-tables
    -ffunction-sections -fdata-sections
    ${COACT_FOOTPRINT_OPT:--Os} -g -Wall -I"$FREERTOS_DIR" -I"$FREERTOS_DIR/include"
    -I"$FREERTOS_DIR/port" -I"$SCRIPT_DIR" -I"$COACT_INCLUDE_DIR"
)
CXX_FLAGS=(
    -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics
    -fno-use-cxa-atexit -DCOACT_ATOMIC64_SHIM
)
if [ -n "${COACT_FOOTPRINT_LTO:-}" ]; then
    CXX_FLAGS+=(-flto)
fi
if [ -n "${COACT_FOOTPRINT_EXTRA_FLAGS:-}" ]; then
    COMMON_FLAGS+=(${COACT_FOOTPRINT_EXTRA_FLAGS})
fi

fail() {
    echo "FOOTPRINT HARNESS ERROR: $*" >&2
    exit 1
}

mkdir -p "$BUILD_DIR" || fail "cannot create $BUILD_DIR"
exec 9> "$BUILD_DIR/.lock" || fail "cannot open lock"
flock 9 || fail "cannot lock $BUILD_DIR"

echo "== build kernel =="
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
    -c "$SCRIPT_DIR/footprint_hooks.cpp" -o "$BUILD_DIR/footprint_hooks.o" \
    || fail "footprint hooks compile"

KERNEL_OBJS=("$BUILD_DIR/start_freertos.o" "$BUILD_DIR/runtime.o"
             "$BUILD_DIR/tasks.o" "$BUILD_DIR/queue.o" "$BUILD_DIR/list.o"
             "$BUILD_DIR/port.o" "$BUILD_DIR/footprint_hooks.o")

build_image() {
    local name=$1
    local main_src=$2
    local extra=${3:-}
    local obj="$BUILD_DIR/${name}_main.o"
    "$CXX" "${COMMON_FLAGS[@]}" "${CXX_FLAGS[@]}" $extra \
        -c "$main_src" -o "$obj" || fail "$name main compile"
    local objs=("${KERNEL_OBJS[@]}" "$BUILD_DIR/pal_freertos.o" "$obj")
    if [ "$name" = "baseline" ]; then
        objs=("${KERNEL_OBJS[@]}" "$obj")
    fi
    "$CXX" "${COMMON_FLAGS[@]}" "${CXX_FLAGS[@]}" -nostdlib -nostartfiles \
        -Wl,--gc-sections -Wl,-T,"$SCRIPT_DIR/link.ld" \
        "${objs[@]}" -lgcc -o "$BUILD_DIR/${name}.elf" || fail "$name link"
}

echo "== build coact PAL =="
"$CXX" "${COMMON_FLAGS[@]}" "${CXX_FLAGS[@]}" \
    -c "$REPO_ROOT/src/core/pal_freertos.cpp" -o "$BUILD_DIR/pal_freertos.o" \
    || fail "pal_freertos compile"

echo "== build images =="
build_image baseline "$SCRIPT_DIR/footprint_baseline_main.cpp"
build_image coact "$SCRIPT_DIR/footprint_coact_main.cpp"
build_image coact_small "$SCRIPT_DIR/footprint_coact_main.cpp" "-DCOACT_FOOTPRINT_SMALL"

echo
echo "== arm-none-eabi-size (bytes) =="
printf '%-12s %8s %8s %8s %8s\n' image text data bss total
for name in baseline coact coact_small; do
    read -r text data bss dec _ < <(arm-none-eabi-size "$BUILD_DIR/$name.elf" | tail -n 1)
    printf '%-12s %8s %8s %8s %8s\n' "$name" "$text" "$data" "$bss" "$dec"
done

echo
echo "== coact RAM consumers (largest .bss symbols) =="
arm-none-eabi-nm --size-sort -S "$BUILD_DIR/coact.elf" \
    | awk '$3 ~ /^[bB]$/ {print $2, $4}' | tail -n 12

echo
echo "== run both images (Renode, bounded) =="
for name in baseline coact coact_small; do
    uart_log="$BUILD_DIR/${name}.uart.log"
    : > "$uart_log"
    resc="$BUILD_DIR/${name}.resc"
    sed -e "s|@FREERTOS_UART@|@$uart_log|" \
        -e "s|@FREERTOS_ELF@|@$BUILD_DIR/${name}.elf|" \
        "$SCRIPT_DIR/coact_freertos.resc" > "$resc" || fail "cannot write $resc"
    timeout 100 "$RENODE" --console --disable-gui --plain \
        -e "include @$resc" > "$BUILD_DIR/${name}.renode.log" 2>&1 \
        || fail "$name Renode run failed"
    echo "-- $name --"
    cat "$uart_log"
done
