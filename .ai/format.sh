#!/bin/bash
# .ai/format.sh -- Check or apply clang-format style to the C++ sources.
# Usage: .ai/format.sh [--check | --write] [path...]
#   (no mode flag)  Check only: list the files that need formatting, exit 1.
#                   This is the DEFAULT because the alternative is destructive
#                   -- see the note on --write below.
#   --check         The same check, stated explicitly. What check.sh and CI use.
#   --write, -i     Rewrite files IN PLACE. The only mode that modifies the tree.
#                   It rewrites whole files, so run it only on a clean working
#                   tree: it silently absorbs any uncommitted edits in the files
#                   it touches, which is how a stray no-argument run used to
#                   reformat most of the repository and clobber in-flight work.
#   path...         Specific files/dirs (default: include/ src/ test/, resolved
#                   from the project root)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
STYLE_FILE="$SCRIPT_DIR/.clang-format"

# Paths are resolved from the project root, so the defaults below and any
# path a caller passes mean the same thing regardless of the current directory.
cd "$PROJECT_ROOT"

CHECK_MODE=false
WRITE_MODE=false
TARGETS=()

for arg in "$@"; do
  case "$arg" in
    --check) CHECK_MODE=true ;;
    --write|-i) WRITE_MODE=true ;;
    *) TARGETS+=("$arg") ;;
  esac
done

if $CHECK_MODE && $WRITE_MODE; then
  echo "format.sh: --check and --write are mutually exclusive." >&2
  exit 2
fi

# Default targets
if [ ${#TARGETS[@]} -eq 0 ]; then
  TARGETS=("include" "src" "test")
fi

# Find all C++ files
FILES=$(find "${TARGETS[@]}" -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.h' \) 2>/dev/null | sort)

if [ -z "$FILES" ]; then
  echo "No C++ files found."
  exit 0
fi

COUNT=$(printf '%s\n' "$FILES" | wc -l)

if $WRITE_MODE; then
  echo "Rewriting $COUNT file(s) in place (clang-format -i)..."
  printf '%s\n' "$FILES" | xargs clang-format --style="file:$STYLE_FILE" -i
  echo "Done. Review the diff before committing. Use --check to list offenders without changing anything."
else
  echo "Checking format of $COUNT files (no file is modified)..."
  FAILED=0
  while IFS= read -r f; do
    if ! clang-format --style="file:$STYLE_FILE" --dry-run --Werror "$f" 2>/dev/null; then
      echo "  needs formatting: $f"
      FAILED=$((FAILED + 1))
    fi
  done <<< "$FILES"
  if [ $FAILED -gt 0 ]; then
    echo "$FAILED file(s) need formatting. To rewrite them: .ai/format.sh --write"
    exit 1
  fi
  echo "All files formatted correctly."
fi
