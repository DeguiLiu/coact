#!/usr/bin/env bash
# Build and run the coact admission harness on the Renode Cortex-M4 model.
#
# Usage:
#   test/renode/run.sh                 # baseline + admission scenarios
#   test/renode/run.sh --baseline-only # pipeline smoke test only
#
# Exit codes: 0 all assertions PASS, 1 assertion/run failure, 2 admission API
# not available in the headers being compiled.
set -u

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
# Overridable so mutate.sh can build each mutant elsewhere and keep the main
# fixture's ELF and uart.log intact.
BUILD_DIR=${COACT_RENODE_BUILD_DIR:-"$SCRIPT_DIR/build"}
RENODE=${RENODE:-/home/dgliu/renode/renode}
CC=${CC:-arm-none-eabi-gcc}
CXX=${CXX:-arm-none-eabi-g++}
COACT_INCLUDE_DIR=${COACT_INCLUDE_DIR:-$REPO_ROOT/include}

BASELINE_ONLY=0
if [ "${1:-}" = "--baseline-only" ]; then
    BASELINE_ONLY=1
fi

COMMON_FLAGS=(
    -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
    -ffreestanding -fno-unwind-tables -fno-asynchronous-unwind-tables
    -Os -g -Wall -I"$COACT_INCLUDE_DIR" -I"$SCRIPT_DIR"
)

CXX_FLAGS=(
    -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics
    -fno-use-cxa-atexit
)

fail() {
    echo "HARNESS ERROR: $*" >&2
    exit 1
}

mkdir -p "$BUILD_DIR" || fail "cannot create $BUILD_DIR"
# Serialize concurrent invocations: both the objects and the UART log live in
# one build directory.
exec 9> "$BUILD_DIR/.lock" || fail "cannot open build lock"
flock 9 || fail "cannot lock $BUILD_DIR"

# --- Probe for the v2 admission API ----------------------------------------
# The harness only compiles the admission scenarios when this probe passes, so
# a header tree without the compile-time rule set cannot silently degrade the
# run into "baseline only" and still report PASS.
cat > "$BUILD_DIR/probe_api.cpp" <<'EOF'
#include <type_traits>
#include <utility>

#include "coact/admission.hpp"
#include "coact/coordinator.hpp"

struct ProbeCfg {
    enum : uint8_t { kMaxAo = 2U, kMaxStateDepth = 4U, kMaxDirectDepth = 2U,
                     kBatchSizeMax = 2U };
    enum : uint16_t { kHighCapacity = 2U, kNormalCapacity = 2U,
                      kLowCapacity = 2U, kCooldownCycles = 1U };
    enum : uint32_t { kBatchTimeoutMs = 1U, kLowMaxWaitMs = 1U };
    enum : uint64_t { kDirectBudgetNs = 1ULL, kRtcBudgetNs = 1ULL };
};

struct ProbePal {
    uint64_t monotonic_ns() { return 0U; }
    void signal_dispatcher_from_task() {}
    void signal_dispatcher_from_isr() {}
    void enter_direct() {}
    void leave_direct() {}
};

struct ProbeRules {
    static constexpr auto kRules = coact::make_rules(
        coact::TargetAdmission{coact::TargetId(1U), coact::MinArrivalNs{1000U},
                               coact::MaxBacklog{2U}});
};

using ProbeStage = coact::Staging<ProbeCfg, coact::SingleCoreCriticalRing>;
using ProbeRegistry = coact::AoRegistry<ProbeCfg>;
using ProbeMonitor = coact::Monitor<ProbeCfg>;
using ProbeBreaker = coact::Breaker<ProbeCfg>;
using ProbeAdm =
    coact::StaticAdmission<ProbeRules, ProbeCfg, coact::AdmissionSpinSync>;
using ProbeCoord =
    coact::DispatchCoordinator<ProbeStage, ProbePal, ProbeBreaker, ProbeAdm>;
using PlainCoord =
    coact::DispatchCoordinator<ProbeStage, ProbePal, ProbeBreaker>;

static_assert(static_cast<uint16_t>(coact::AdmissionReason::kArrivalTooSoon) == 4U, "");
static_assert(static_cast<uint16_t>(coact::AdmissionReason::kBacklogFull) == 5U, "");
static_assert(static_cast<uint16_t>(coact::AdmissionReason::kStagingFull) == 6U, "");
static_assert(ProbeAdm::kEnabled, "the static policy must report itself enabled");
static_assert(!coact::NoAdmission::kEnabled, "the default policy must be off");
static_assert(ProbeAdm::kRuleCount == 1U, "");
static_assert(std::is_same<ProbeAdm::Reservation, coact::ArrivalReservation>::value, "");
static_assert(std::is_same<coact::NoAdmission::Reservation,
                           coact::InertReservation>::value, "");
static_assert(!std::is_copy_constructible<coact::ArrivalReservation>::value,
              "the reservation must stay move-only");
static_assert(std::is_constructible<ProbeCoord, ProbeStage&, ProbeRegistry&,
                                    ProbeMonitor&, ProbeBreaker&, ProbePal&>::value,
              "the admission policy is a template parameter, not a ctor argument");
static_assert(std::is_constructible<PlainCoord, ProbeStage&, ProbeRegistry&,
                                    ProbeMonitor&, ProbeBreaker&, ProbePal&>::value, "");
static_assert(!std::is_copy_constructible<ProbeCoord>::value,
              "a copied coordinator would duplicate the admission gates");
static_assert(!std::is_copy_constructible<ProbeAdm>::value,
              "a copied policy would duplicate the admission gates");
static_assert(std::is_same<decltype(std::declval<coact::ArrivalGate&>().admitted_ever(
                               std::declval<const coact::CriticalSection&>())),
                           bool>::value, "");
static_assert(std::is_same<decltype(std::declval<coact::ArrivalGate&>().last_admit_ns(
                               std::declval<const coact::CriticalSection&>())),
                           uint64_t>::value, "");
static_assert(std::is_same<decltype(std::declval<const ProbeAdm&>()
                                        .gate_for(coact::TargetId(1U))
                                        .last_admit_ns(
                                            std::declval<const coact::CriticalSection&>())),
                           uint64_t>::value,
              "the policy must expose per-target gate state for verification");
static_assert(std::is_same<decltype(std::declval<ProbeMonitor&>()
                                        .admission_snapshot(1U)),
                           coact::Expected<ProbeMonitor::AdmissionSnapshot,
                                           ProbeMonitor::SnapshotError>>::value,
              "snapshot reads must report why they failed");
EOF

HAVE_ADMISSION=0
if "$CXX" "${COMMON_FLAGS[@]}" "${CXX_FLAGS[@]}" -fsyntax-only "$BUILD_DIR/probe_api.cpp" \
        > "$BUILD_DIR/probe.log" 2>&1; then
    HAVE_ADMISSION=1
elif [ "$BASELINE_ONLY" -eq 0 ]; then
    sed -n '1,20p' "$BUILD_DIR/probe.log" >&2
    echo "ADMISSION API NOT PRESENT in $COACT_INCLUDE_DIR" >&2
    echo "StaticAdmission/AdmissionReason/ArrivalGate/Expected probe failed." >&2
    exit 2
fi

BUILD_DEFS=()
EXPECTED_GROUPS="baseline"
EXPECTED_TOTAL=15
# Exact per-group counts. A group that silently loses assertions still prints
# "RESULT PASS n/n" because n and m come from the same counter, so the gate
# below pins both the total and each group to the numbers in README.md.
declare -A GROUP_COUNT=([baseline]=15)
if [ "$HAVE_ADMISSION" -eq 1 ] && [ "$BASELINE_ONLY" -eq 0 ]; then
    BUILD_DEFS+=(-DCOACT_HAS_ADMISSION=1)
    EXPECTED_GROUPS="baseline S1 S2 S3 S4 S5 S6 S7 S8 S9 S10 S11 S12 S13 S14"
    EXPECTED_TOTAL=231
    GROUP_COUNT=([baseline]=15 [S1]=7 [S2]=20 [S3]=13 [S4]=11 [S5]=9
                 [S6]=15 [S7]=15 [S8]=22 [S9]=12 [S10]=11 [S11]=22
                 [S12]=19 [S13]=19 [S14]=21)
fi

echo "== build =="
"$CC" "${COMMON_FLAGS[@]}" -c "$SCRIPT_DIR/start.c" -o "$BUILD_DIR/start.o" \
    || fail "startup compile failed"
"$CC" "${COMMON_FLAGS[@]}" -c "$SCRIPT_DIR/runtime.c" -o "$BUILD_DIR/runtime.o" \
    || fail "runtime compile failed"
"$CXX" "${COMMON_FLAGS[@]}" "${CXX_FLAGS[@]}" "${BUILD_DEFS[@]}" -c "$SCRIPT_DIR/main.cpp" \
    -o "$BUILD_DIR/main.o" || fail "harness compile failed"
"$CXX" "${COMMON_FLAGS[@]}" "${CXX_FLAGS[@]}" -nostdlib -nostartfiles -Wl,--gc-sections \
    -Wl,-T,"$SCRIPT_DIR/link.ld" \
    "$BUILD_DIR/start.o" "$BUILD_DIR/runtime.o" "$BUILD_DIR/main.o" -lgcc \
    -o "$BUILD_DIR/coact_renode.elf" || fail "link failed"
arm-none-eabi-size "$BUILD_DIR/coact_renode.elf" || true

# --- Run Renode headless ---------------------------------------------------
UART_LOG="$BUILD_DIR/uart.log"
: > "$UART_LOG"
# The .resc template hardcodes test/renode/build for the ELF and the UART
# file; rewrite both so an alternate COACT_RENODE_BUILD_DIR (used by
# mutate.sh) reads and writes only inside that directory.
RESC="$BUILD_DIR/coact_admission.resc"
sed -e "s|@test/renode/build/uart.log|@$UART_LOG|" \
    -e "s|@test/renode/build/coact_renode.elf|@$BUILD_DIR/coact_renode.elf|" \
    "$SCRIPT_DIR/coact_admission.resc" > "$RESC" || fail "cannot write $RESC"

echo "== run (Renode, bounded 1 s virtual time) =="
cd "$REPO_ROOT" || fail "cannot cd to $REPO_ROOT"
timeout 100 "$RENODE" --console --disable-gui --plain \
    -e "include @$RESC" > "$BUILD_DIR/renode.log" 2>&1
RENODE_RC=$?
if [ "$RENODE_RC" -ne 0 ]; then
    tail -n 30 "$BUILD_DIR/renode.log" >&2
    fail "Renode exited with rc=$RENODE_RC"
fi
if [ ! -s "$UART_LOG" ]; then
    tail -n 30 "$BUILD_DIR/renode.log" >&2
    fail "no target output captured in $UART_LOG"
fi

echo "== target output =="
cat "$UART_LOG"

# --- Gate on the target's own verdict -------------------------------------
if grep -q '^FAIL ' "$UART_LOG"; then
    echo "HARNESS RESULT: FAIL (target reported FAIL)" >&2
    exit 1
fi

RESULT_LINE=$(tr -d '\r' < "$UART_LOG" | grep '^RESULT PASS ' | tail -n 1)
if [ -z "$RESULT_LINE" ]; then
    echo "HARNESS RESULT: FAIL (missing 'RESULT PASS' line)" >&2
    exit 1
fi

COUNTS=${RESULT_LINE#RESULT PASS }
PASSED=${COUNTS%%/*}
TOTAL=${COUNTS##*/}
if [ "$PASSED" != "$TOTAL" ]; then
    echo "HARNESS RESULT: FAIL ($PASSED/$TOTAL)" >&2
    exit 1
fi
if [ "$TOTAL" -ne "$EXPECTED_TOTAL" ]; then
    echo "HARNESS RESULT: FAIL (ran $TOTAL assertions, expected $EXPECTED_TOTAL)" >&2
    exit 1
fi

for group in $EXPECTED_GROUPS; do
    count=$(grep -c "^PASS ${group}\." "$UART_LOG")
    want=${GROUP_COUNT[$group]}
    if [ "$count" -ne "$want" ]; then
        echo "HARNESS RESULT: FAIL (group '$group' ran $count assertions, expected $want)" >&2
        exit 1
    fi
done

echo "HARNESS RESULT: PASS ($PASSED/$TOTAL)"
