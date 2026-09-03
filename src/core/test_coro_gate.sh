#!/usr/bin/env bash
# Static ban-list for coact::coro sources (plan Task 8). Scoped to the coro
# landing files so isp_pipeline demo text does not trip the gate.
set -euo pipefail
root=$(git rev-parse --show-toplevel)
cd "$root"
files=(
  include/coact/coro
  src/core/test_coro_registry.cpp
  src/core/test_coro_combinators.cpp
  src/core/test_coro_posix.cpp
  src/core/test_coro_awaitable.cpp
  src/core/test_coro_scheduler.cpp
  src/core/test_coro_integration.cpp
  examples/coact_coro_demo.cpp
  examples/coact_coro_posix.cpp
)
fail=0
if rg -n --hidden -g '!*.md' '\but::|\but_|exception_ptr|#include <coroutine>|#include <format>|#include <ranges>' "${files[@]}"; then
  echo "test_coro_rg_gate: banned identifier or header found"
  fail=1
fi
# Filter: allow the <new> header include (placement-new support) and the
# standard C++ term "placement-new" in comments; ban everything else.
if rg -n --hidden -g '!*.md' -P '(?<!::)\bnew\b(?!\s*\()|\b(delete|malloc|free)\s*\(' "${files[@]}" \
    | rg -v '#include <new>|placement-new' >/dev/null 2>&1; then
  echo "test_coro_rg_gate: heap call found"
  fail=1
fi
if [ "$fail" -ne 0 ]; then
  exit 1
fi
echo "test_coro_rg_gate: PASS"
