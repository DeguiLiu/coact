#!/bin/bash
# .ai/tidy.sh -- Run clang-tidy static analysis on include/ headers
# Usage: .ai/tidy.sh [--fix] [file...]
#   --fix     Apply suggested fixes (use with caution)
#   file...   Specific .cpp files to analyze (default: tests/*.cpp)
#
# Prerequisites:
#   - clang-tidy (pip install clang-tidy, or apt install clang-tidy)
#   - compile_commands.json (cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
#
# Only warnings from include/osp/ are reported (controlled by .clang-tidy HeaderFilterRegex).

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
CONFIG="$SCRIPT_DIR/.clang-tidy"
BUILD_DIR="$PROJECT_ROOT/build"

# --- Locate clang-tidy ---
CLANG_TIDY=""
for candidate in clang-tidy ~/.local/bin/clang-tidy; do
  if command -v "$candidate" &>/dev/null || [ -x "$candidate" ]; then
    CLANG_TIDY="$candidate"
    break
  fi
done

if [ -z "$CLANG_TIDY" ]; then
  echo "clang-tidy not found. Install: pip install clang-tidy"
  exit 1
fi

# --- Check compile_commands.json ---
if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
  echo "compile_commands.json not found. Generating..."
  cmake -B "$BUILD_DIR" -S "$PROJECT_ROOT" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DOSP_BUILD_TESTS=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    > /dev/null 2>&1
fi

# --- Parse arguments ---
FIX_FLAG=""
TARGETS=()
for arg in "$@"; do
  case "$arg" in
    --fix) FIX_FLAG="--fix" ;;
    *) TARGETS+=("$arg") ;;
  esac
done

if [ ${#TARGETS[@]} -eq 0 ]; then
  mapfile -t TARGETS < <(find "$PROJECT_ROOT/src" "$PROJECT_ROOT/test" -name '*.cpp' -o '*.hpp' -o '*.h' 2>/dev/null | sort)
fi

if [ ${#TARGETS[@]} -eq 0 ]; then
  echo "No .cpp files found to analyze."
  exit 0
fi

echo "Running clang-tidy on ${#TARGETS[@]} files (reporting include/osp/ only)..."
echo "Config: $CONFIG"
echo ""

# --- Run clang-tidy ---
TIDY_OUTPUT="/tmp/osp-tidy-output.txt"
TIDY_STDERR="/tmp/osp-tidy-stderr.txt"

"$CLANG_TIDY" --config-file="$CONFIG" -p "$BUILD_DIR" $FIX_FLAG "${TARGETS[@]}" \
  > "$TIDY_OUTPUT" 2> "$TIDY_STDERR" || true

# Filter for include/osp/ warnings only
grep -E 'include/osp/' "$TIDY_OUTPUT" | sort -u || true

# --- Summary ---
WARNING_COUNT=$(grep -cE 'include/osp/.*warning:' "$TIDY_OUTPUT" 2>/dev/null || echo "0")
ERROR_COUNT=$(grep -cE 'include/osp/.*error:' "$TIDY_OUTPUT" 2>/dev/null || echo "0")
UNIQUE_WARNINGS=$(grep -E 'include/osp/.*warning:' "$TIDY_OUTPUT" 2>/dev/null | sort -u | wc -l || echo "0")

echo ""
echo "========================================"
echo "  clang-tidy: $UNIQUE_WARNINGS unique warnings, $ERROR_COUNT errors"
echo "========================================"

# WarningsAsErrors exit code
if grep -q 'treated as error' "$TIDY_STDERR" 2>/dev/null; then
  echo "FAIL: WarningsAsErrors triggered (see above)"
  exit 1
fi

if [ "$ERROR_COUNT" -gt 0 ]; then
  exit 1
fi

exit 0
