#!/bin/bash
# .ai/tidy.sh -- Run clang-tidy static analysis on include/ headers
# Usage: .ai/tidy.sh [--fix] [file...]
#   --fix     Apply suggested fixes (use with caution)
#   file...   Specific .cpp files to analyze (default: all of src/ and test/)
#
# Env:
#   COACT_BUILD_DIR   Build dir holding compile_commands.json (default: build/)
#
# Prerequisites:
#   - clang-tidy (pip install clang-tidy, or apt install clang-tidy)
#   - compile_commands.json (cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
#
# Only warnings from include/coact/ are reported (controlled by .clang-tidy HeaderFilterRegex).

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
CONFIG="$SCRIPT_DIR/.clang-tidy"
BUILD_DIR="${COACT_BUILD_DIR:-$PROJECT_ROOT/build}"

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

# --- Ensure compile_commands.json ---
# Regenerate when it is missing OR stale. A compile_commands.json produced by a
# checkout at a different path still parses, but every -I it carries points at
# the old tree, so clang-tidy silently analyses headers that no longer exist.
# Comparing the recorded source root catches that, which a bare -f test does not.
COMPILE_DB="$BUILD_DIR/compile_commands.json"
NEED_CONFIGURE=false
if [ ! -f "$COMPILE_DB" ]; then
  NEED_CONFIGURE=true
elif ! grep -q "\"$PROJECT_ROOT/" "$COMPILE_DB"; then
  echo "compile_commands.json records a different source root; regenerating."
  NEED_CONFIGURE=true
fi

if $NEED_CONFIGURE; then
  echo "Configuring $BUILD_DIR ..."
  CONFIGURE_LOG="/tmp/coact-tidy-configure.log"
  if ! cmake -B "$BUILD_DIR" -S "$PROJECT_ROOT" \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      > "$CONFIGURE_LOG" 2>&1; then
    # The usual cause is a build dir left over from a checkout at a different
    # path: its CMakeCache.txt pins the old source dir and cmake refuses to
    # reuse it. Say so instead of dumping cmake's own confusing error.
    if grep -q "different than the directory" "$CONFIGURE_LOG" 2>/dev/null; then
      echo "FAIL: $BUILD_DIR holds a CMakeCache.txt from another checkout." >&2
      echo "      Remove $BUILD_DIR, or pick a fresh dir with COACT_BUILD_DIR=<dir>." >&2
    else
      echo "FAIL: cmake configure failed; see $CONFIGURE_LOG" >&2
    fi
    exit 1
  fi
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
  # Parentheses are required: without them -o binds looser than the implicit
  # AND, and the expression would also match every file in include/.
  mapfile -t TARGETS < <(find "$PROJECT_ROOT/src" "$PROJECT_ROOT/test" \
    -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) 2>/dev/null | sort)
fi

if [ ${#TARGETS[@]} -eq 0 ]; then
  echo "No .cpp files found to analyze."
  exit 0
fi

echo "Running clang-tidy on ${#TARGETS[@]} files (reporting include/coact/ only)..."
echo "Config: $CONFIG"
echo ""

# --- Run clang-tidy ---
TIDY_OUTPUT="/tmp/coact-tidy-output.txt"
TIDY_STDERR="/tmp/coact-tidy-stderr.txt"

"$CLANG_TIDY" --config-file="$CONFIG" -p "$BUILD_DIR" $FIX_FLAG "${TARGETS[@]}" \
  > "$TIDY_OUTPUT" 2> "$TIDY_STDERR" || true

# Filter for include/coact/ warnings only
grep -E 'include/coact/' "$TIDY_OUTPUT" | sort -u || true

# --- Summary ---
# grep -c already prints 0 and exits 1 when there is no match, so appending
# `|| echo 0` would yield "0\n0" and break the integer test below. Swallow the
# exit status instead.
WARNING_COUNT=$(grep -cE 'include/coact/.*warning:' "$TIDY_OUTPUT" 2>/dev/null || true)
ERROR_COUNT=$(grep -cE 'include/coact/.*error:' "$TIDY_OUTPUT" 2>/dev/null || true)
UNIQUE_WARNINGS=$(grep -E 'include/coact/.*warning:' "$TIDY_OUTPUT" 2>/dev/null | sort -u | wc -l)
WARNING_COUNT="${WARNING_COUNT:-0}"
ERROR_COUNT="${ERROR_COUNT:-0}"
UNIQUE_WARNINGS="${UNIQUE_WARNINGS:-0}"

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
