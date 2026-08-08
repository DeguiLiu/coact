#!/bin/bash
# .ai/format.sh -- Format all C++ source files using clang-format
# Usage: .ai/format.sh [--check] [path...]
#   --check   Dry-run mode, exit 1 if any file needs formatting
#   path...   Specific files/dirs to format (default: include/ tests/ examples/)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
STYLE_FILE="$SCRIPT_DIR/.clang-format"

CHECK_MODE=false
TARGETS=()

for arg in "$@"; do
  case "$arg" in
    --check) CHECK_MODE=true ;;
    *) TARGETS+=("$arg") ;;
  esac
done

# Default targets
if [ ${#TARGETS[@]} -eq 0 ]; then
  TARGETS=("/include" "/src" "/test")
fi

# Find all C++ files
FILES=$(find "${TARGETS[@]}" -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.h' \) 2>/dev/null | sort)
COUNT=$(echo "$FILES" | wc -l)

if [ -z "$FILES" ]; then
  echo "No C++ files found."
  exit 0
fi

if $CHECK_MODE; then
  echo "Checking format of $COUNT files..."
  FAILED=0
  while IFS= read -r f; do
    if ! clang-format --style="file:$STYLE_FILE" --dry-run --Werror "$f" 2>/dev/null; then
      FAILED=$((FAILED + 1))
    fi
  done <<< "$FILES"
  if [ $FAILED -gt 0 ]; then
    echo "$FAILED file(s) need formatting. Run: .ai/format.sh"
    exit 1
  fi
  echo "All files formatted correctly."
else
  echo "Formatting $COUNT files..."
  echo "$FILES" | xargs clang-format --style="file:$STYLE_FILE" -i
  echo "Done."
fi
