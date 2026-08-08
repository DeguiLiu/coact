#!/bin/bash
# .ai/run-tsan.sh -- Run a TSan test binary with ASLR disabled.
# GCC libtsan crashes with "unexpected memory mapping" under kernel 6.5
# high-entropy ASLR (vm.mmap_rnd_bits=32). setarch -R disables ASLR for the
# process, which libtsan requires. Usage: .ai/run-tsan.sh [build-dir] [binary]
# Note: GCC 11 libtsan falsely flags condvar-based semaphores ("double lock",
# data race at the count++ under the mutex) and exits 66; the clang TSan gate
# in CI is authoritative for pass/fail.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${1:-$PROJECT_ROOT/build_tsan}"
BIN_NAME="${2:-osp_tests}"
if [ $# -gt 0 ]; then shift; fi
if [ $# -gt 0 ]; then shift; fi

BIN="$BUILD_DIR/tests/$BIN_NAME"
if [ ! -x "$BIN" ]; then
  echo "TSan test binary not found: $BIN"
  echo "Build it first: cmake -B $BUILD_DIR -DOSP_BUILD_TESTS=ON -DOSP_BUILD_EXAMPLES=OFF -DCMAKE_CXX_FLAGS='-fsanitize=thread -g -fno-omit-frame-pointer' && cmake --build $BUILD_DIR -j"
  exit 1
fi

exec setarch "$(uname -m)" -R "$BIN" "$@"
