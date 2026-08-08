#!/bin/bash
# .ai/lint.sh -- Run cpplint on all C++ source files
# Usage: .ai/lint.sh [--fix] [path...]
#   --fix     Auto-fix trivial issues (whitespace) via clang-format after lint
#   path...   Specific files/dirs to lint (default: include/ tests/ examples/)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
CPPLINT_CFG="$SCRIPT_DIR/CPPLINT.cfg"

AUTO_FIX=false
TARGETS=()

for arg in "$@"; do
  case "$arg" in
    --fix) AUTO_FIX=true ;;
    *) TARGETS+=("$arg") ;;
  esac
done

if [ ${#TARGETS[@]} -eq 0 ]; then
  TARGETS=("/include" "/src" "/test")
fi

# Locate cpplint
CPPLINT=""
for candidate in cpplint ~/.local/bin/cpplint ~/.local/venv-format/bin/cpplint; do
  if command -v "$candidate" &>/dev/null || [ -x "$candidate" ]; then
    CPPLINT="$candidate"
    break
  fi
done

if [ -z "$CPPLINT" ]; then
  echo "cpplint not found. Install: pip install cpplint"
  exit 1
fi

# Find all C++ files
FILES=$(find "${TARGETS[@]}" -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.h' \) 2>/dev/null | sort)
COUNT=$(echo "$FILES" | wc -l)

if [ -z "$FILES" ]; then
  echo "No C++ files found."
  exit 0
fi

echo "Linting $COUNT files with cpplint..."

# cpplint --config doesn't support paths, only filenames in ancestor dirs.
# Create temporary symlink in project root, clean up on exit.
NEED_CLEANUP=false
if [ ! -f "$PROJECT_ROOT/CPPLINT.cfg" ]; then
  ln -s "$CPPLINT_CFG" "$PROJECT_ROOT/CPPLINT.cfg"
  NEED_CLEANUP=true
fi
cleanup() { $NEED_CLEANUP && rm -f "$PROJECT_ROOT/CPPLINT.cfg"; }
trap cleanup EXIT

ERRORS=0
echo "$FILES" | xargs "$CPPLINT" --root="$PROJECT_ROOT/include" 2>&1 | tee /tmp/cpplint_output.txt || ERRORS=$?

ERROR_COUNT=$(grep -c "Total errors found:" /tmp/cpplint_output.txt 2>/dev/null || echo "0")

if [ $ERRORS -ne 0 ]; then
  echo ""
  echo "cpplint found issues (exit code: $ERRORS)"
  if $AUTO_FIX; then
    echo "Running clang-format to fix whitespace issues..."
    "$SCRIPT_DIR/format.sh" "${TARGETS[@]}"
    echo "Re-running cpplint..."
    echo "$FILES" | xargs "$CPPLINT" --root="$PROJECT_ROOT/include" 2>&1 || true
  fi
  exit 1
fi

echo "All files passed cpplint."
