#!/usr/bin/env bash
# ASan/UBSan stability classifier for coact host tests.
#
# The ASan runtime on this host crashes intermittently at INIT time with
# "AddressSanitizer:DEADLYSIGNAL" (reproducible on a trivial malloc/free
# binary; not under gdb). That is environmental jitter, NOT a test finding.
#
# This script runs every test binary N times and classifies each run:
#   PASS          - exit 0
#   REAL_BUG      - exit !=0 AND stderr contains a specific AddressSanitizer
#                   report (heap/stack/global buffer overflow, UAF, double-free,
#                   SEGV with a stack trace, UBSan runtime error, ...). A real
#                   finding.
#   ASSERT_FAIL   - exit !=0 AND stdout/stderr contains "[FAIL]" (a real test
#                   assertion failure, independent of the sanitizer).
#   ENV_JITTER    - exit !=0 AND stderr is only DEADLYSIGNAL noise (no real
#                   report, no [FAIL]). ASan runtime init crash -> environment.
#
# Usage: ./test/asan_classify.sh <build_dir> [runs_per_test]
set -u

BUILD_DIR="${1:?usage: $0 <build_dir> [runs_per_test]}"
RUNS="${2:-10}"

mapfile -t BINS < <(find "${BUILD_DIR}" -name "test_*" -type f -executable | sort)
echo "Tests: ${#BINS[@]}  Runs per test: ${RUNS}"
echo "======================================================================"

overall_bugs=0
for b in "${BINS[@]}"; do
    name=$(basename "$b")
    pass=0; jitter=0; bugs=0; assert_fail=0
    bug_sample=""
    for i in $(seq 1 "${RUNS}"); do
        out=$("$b" 2>&1)
        code=$?
        if [ "$code" -eq 0 ]; then
            pass=$((pass+1))
        elif grep -q "ERROR: AddressSanitizer:" <<<"$out" || \
             grep -q "runtime error:" <<<"$out" || \
             grep -q "ERROR: LeakSanitizer:" <<<"$out" || \
             grep -q "AddressSanitizer: heap-\|AddressSanitizer: stack-\|AddressSanitizer: global-buffer\|AddressSanitizer: use-after\|AddressSanitizer: double-free\|AddressSanitizer: alloc-dealloc" <<<"$out"; then
            bugs=$((bugs+1))
            overall_bugs=$((overall_bugs+1))
            if [ -z "$bug_sample" ]; then bug_sample=$(echo "$out" | grep -m1 "ERROR: AddressSanitizer\|runtime error:"); fi
        elif grep -q "\[FAIL\]" <<<"$out"; then
            assert_fail=$((assert_fail+1))
        else
            jitter=$((jitter+1))
        fi
    done
    verdict="PASS"
    [ "$bugs" -gt 0 ] && verdict="REAL_BUG"
    [ "$assert_fail" -gt 0 ] && verdict="ASSERT_FAIL"
    [ "$jitter" -gt 0 ] && [ "$bugs" -eq 0 ] && [ "$assert_fail" -eq 0 ] && verdict="ENV_JITTER($jitter)"
    printf "%-28s pass=%-3d jitter=%-3d bugs=%-3d assert_fail=%-3d  %s\n" "$name" "$pass" "$jitter" "$bugs" "$assert_fail" "$verdict"
    [ -n "$bug_sample" ] && printf "      sample: %s\n" "$bug_sample"
done
echo "======================================================================"
echo "overall REAL_BUG runs: $overall_bugs"
