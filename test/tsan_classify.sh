#!/usr/bin/env bash
# TSan classifier for coact host tests under setarch -R (disables ASLR, which
# GCC libtsan requires on kernel 6.5; see .ai/run-tsan.sh).
#
# Classifies each test binary run:
#   PASS      - exit 0 and no TSan warning
#   RACE      - exit !=0 OR stderr contains "WARNING: ThreadSanitizer:" or
#               "SUMMARY: ThreadSanitizer:"
#   FAIL      - exit !=0 with "[FAIL]" assertion failure (no TSan warning)
#   FATAL     - "FATAL: ThreadSanitizer:" (runtime mapping/env failure)
#
# Usage: ./test/tsan_classify.sh <build_dir> [runs_per_test]
set -u

BUILD_DIR="${1:?usage: $0 <build_dir> [runs_per_test]}"
RUNS="${2:-1}"

mapfile -t BINS < <(find "${BUILD_DIR}" -name "test_*" -type f -executable | sort)
echo "Tests: ${#BINS[@]}  Runs per test: ${RUNS}"
echo "======================================================================"
for b in "${BINS[@]}"; do
    name=$(basename "$b")
    pass=0; race=0; fail=0; fatal=0
    race_sig=""
    for i in $(seq 1 "${RUNS}"); do
        out=$(setarch "$(uname -m)" -R "$b" 2>&1)
        code=$?
        if grep -q "FATAL: ThreadSanitizer:" <<<"$out"; then
            fatal=$((fatal+1))
        elif grep -q "WARNING: ThreadSanitizer:\|SUMMARY: ThreadSanitizer:" <<<"$out"; then
            race=$((race+1))
            if [ -z "$race_sig" ]; then
                race_sig=$(echo "$out" | grep -m1 "SUMMARY: ThreadSanitizer:")
            fi
        elif [ "$code" -ne 0 ] && grep -q "\[FAIL\]" <<<"$out"; then
            fail=$((fail+1))
        elif [ "$code" -ne 0 ]; then
            race=$((race+1))   # non-zero without [FAIL] but with TSan output
            if [ -z "$race_sig" ]; then race_sig="(non-zero exit $code)"; fi
        else
            pass=$((pass+1))
        fi
    done
    verdict="PASS"
    [ "$fatal" -gt 0 ] && verdict="FATAL"
    [ "$race" -gt 0 ] && verdict="RACE"
    [ "$fail" -gt 0 ] && verdict="FAIL"
    printf "%-28s pass=%-3d race=%-3d fail=%-3d fatal=%-3d  %s\n" "$name" "$pass" "$race" "$fail" "$fatal" "$verdict"
    [ -n "$race_sig" ] && printf "      %s\n" "$race_sig"
done
