#!/bin/bash
# .ai/check.sh -- Run all code quality checks (format + lint + build + test)
# Usage: .ai/check.sh [--quick] [--sanitizer]
#   --quick       Skip tests, only check format + lint + build
#   --sanitizer   Build with ASan+UBSan and run tests

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

QUICK=false
SANITIZER=false

for arg in "$@"; do
  case "$arg" in
    --quick) QUICK=true ;;
    --sanitizer) SANITIZER=true ;;
  esac
done

PASS=0
FAIL=0

run_step() {
  local name="$1"
  shift
  echo ""
  echo "========================================"
  echo "  $name"
  echo "========================================"
  if "$@"; then
    echo "  -> PASS"
    PASS=$((PASS + 1))
  else
    echo "  -> FAIL"
    FAIL=$((FAIL + 1))
  fi
}

# Step 1: Format check
run_step "Format Check" "$SCRIPT_DIR/format.sh" --check

# Step 2: Lint
run_step "Lint (cpplint)" "$SCRIPT_DIR/lint.sh" || true

# Step 2.5: Static Analysis (clang-tidy, optional)
if command -v clang-tidy &>/dev/null; then
  run_step "Static Analysis (clang-tidy)" "$SCRIPT_DIR/tidy.sh" || true
else
  echo "  Skipping clang-tidy (not installed)"
fi

# Step 3: Build
BUILD_DIR="$PROJECT_ROOT/build"
if $SANITIZER; then
  BUILD_DIR="$PROJECT_ROOT/build-asan"
  run_step "CMake Configure (ASan+UBSan)" \
    cmake -B "$BUILD_DIR" -S "$PROJECT_ROOT" \
      -DCMAKE_BUILD_TYPE=Debug \
      -DOSP_BUILD_TESTS=ON \
      -DOSP_BUILD_EXAMPLES=ON \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
else
  run_step "CMake Configure" \
    cmake -B "$BUILD_DIR" -S "$PROJECT_ROOT" \
      -DCMAKE_BUILD_TYPE=Debug \
      -DOSP_BUILD_TESTS=ON \
      -DOSP_BUILD_EXAMPLES=ON
fi

NPROC=$(nproc 2>/dev/null || echo 4)
run_step "Build" cmake --build "$BUILD_DIR" -j"$NPROC"

# Step 4: Tests
if ! $QUICK; then
  if $SANITIZER; then
    run_step "Tests (ASan+UBSan)" \
      cmake --build "$BUILD_DIR" --target test -- ARGS="--output-on-failure"
  else
    run_step "Tests" \
      cmake --build "$BUILD_DIR" --target test -- ARGS="--output-on-failure"
  fi
fi

# Summary
echo ""
echo "========================================"
echo "  Summary: $PASS passed, $FAIL failed"
echo "========================================"

[ $FAIL -eq 0 ] && exit 0 || exit 1
