#!/usr/bin/env bash
# Mutation check for the Renode harness itself.
#
# Each mutation copies include/coact to a temporary directory, breaks one
# admission invariant there, rebuilds against the copy and requires run.sh to
# fail. The real headers are never modified. A harness that passes against a
# broken gate or a broken rollback is a harness that proves nothing, so both
# mutations must be caught.
set -u

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/coact-renode-mutate.XXXXXX")

cleanup() {
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

run_mutation() {
    local name="$1" file="$2" from="$3" to="$4"
    local inc="$WORK_DIR/$name"

    mkdir -p "$inc" || return 1
    cp -R "$REPO_ROOT/include/coact" "$inc/coact" || return 1
    python3 - "$inc/coact/$file" "$from" "$to" <<'PY'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(path).read()
if text.count(old) != 1:
    sys.stderr.write("pattern matched %d times in %s\n" % (text.count(old), path))
    sys.exit(1)
open(path, "w").write(text.replace(old, new))
PY
    if [ $? -ne 0 ]; then
        echo "MUTATION ERROR [$name]: source shape changed, update mutate.sh" >&2
        return 1
    fi

    echo "== mutation '$name': $file =="
    COACT_INCLUDE_DIR="$inc" \
        COACT_RENODE_BUILD_DIR="$WORK_DIR/build-$name" \
        "$SCRIPT_DIR/run.sh" > "$WORK_DIR/$name.log" 2>&1
    local rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "MUTATION NOT CAUGHT [$name]: run.sh passed against the broken build" >&2
        return 1
    fi
    # A build error also exits non-zero. Require the mutant to fail an
    # assertion, otherwise "caught" would mean "did not compile".
    if ! grep -q '^FAIL ' "$WORK_DIR/$name.log"; then
        echo "MUTATION ERROR [$name]: run.sh failed without a target FAIL line" >&2
        tail -n 10 "$WORK_DIR/$name.log" >&2
        return 1
    fi
    grep -E '^(FAIL |RESULT |HARNESS )' "$WORK_DIR/$name.log" | head -n 12
    echo "MUTATION CAUGHT [$name]: run.sh exited rc=$rc"
    return 0
}

STATUS=0

# 1. Interval check disabled: every reservation succeeds regardless of the
#    window, so S1's t=999 submission is admitted instead of rejected.
run_mutation gate_interval admission.hpp \
    'if ((now < barrier) || ((now - barrier) < min_ns)) {' \
    'if (false) {' || STATUS=1

# 2. Rollback disabled: a submission that fails after reserving the window
#    keeps it, so S3 and S10 see an advanced window and their retry at the same
#    timestamp is rejected.
run_mutation reservation_rollback admission.hpp \
    '    ~ArrivalReservation() noexcept
    {
        rollback();
    }' \
    '    ~ArrivalReservation() noexcept
    {
    }' || STATUS=1

# 3. commit() turned into a rollback: a successful submission gives the window
#    back, so no interval is ever enforced across two admissions.
run_mutation commit_rolls_back admission.hpp \
    '            gate_->commit(*cs_, now_);' \
    '            gate_->rollback(*cs_);' || STATUS=1


# 4. Budget overrun no longer escalates: the coordinator reports every direct
#    dispatch as inside budget, so S12 never reaches BrokenL1 and keeps
#    dispatching directly instead of queueing.
run_mutation direct_timeout_ignored coordinator.hpp \
    'target_breaker.on_direct_timeout();' \
    'target_breaker.on_rtc_ok();' || STATUS=1

# 5. The overload guard drops critical traffic too: S13's critical submission
#    to a BrokenL2 target must fail on lane capacity, not on the guard.
run_mutation overload_drops_critical coordinator.hpp \
    '            if (!qos.critical) {' \
    '            if (true) {' || STATUS=1

# 6. The coordinator's breaker lookup stops being per AO: every target reads
#    and trips slot 1, so S13's healthy neighbour inherits the broken target's
#    L2 level and loses its direct path. (BreakerBank::on_overflow is not on
#    this path - the coordinator resolves the breaker once, per submission.)
run_mutation breaker_lookup_not_per_ao monitor.hpp \
    'return breakers.breaker(target);' \
    'return breakers.breaker(TargetId(1U));' || STATUS=1

# 7. Every target shares one arrival gate: S11's second target is rejected at
#    the same timestamp the first one was admitted.
run_mutation shared_arrival_gate admission.hpp \
    '    ArrivalGate& gate_for(TargetId target) noexcept
    {
        const uint8_t raw = target.raw();
        return gates_[(raw <= kMaxAo) ? raw : 0U];
    }' \
    '    ArrivalGate& gate_for(TargetId target) noexcept
    {
        const uint8_t raw = target.raw();
        return gates_[(raw <= kMaxAo) ? 1U : 0U];
    }' || STATUS=1

# 8. Reclaim stops decrementing used: S14's post-drain accounting never returns
#    to zero, which is exactly the leak the pool profile is supposed to prevent.
run_mutation pool_used_not_released pool.hpp \
    '            rec->used.store(static_cast<uint16_t>(
                                rec->used.load(std::memory_order_relaxed) - 1U),
                            std::memory_order_relaxed);' \
    '            /* mutated: used is never decremented */' || STATUS=1

# 9. Rollback restores the reserved timestamp instead of only dropping the
#    hold count. The old model let an interleaved failure write a timestamp
#    that was never admitted; S2's rollback at 2000 must leave the window at
#    1000.
run_mutation rollback_restores_timestamp admission.hpp \
    '    void rollback(const CriticalSection& cs) noexcept
    {
        const CriticalSectionGuard guard(cs);
        release_hold();
    }' \
    '    void rollback(const CriticalSection& cs) noexcept
    {
        const CriticalSectionGuard guard(cs);
        last_admit_ns_ = hold_ns_;
        ever_ = true;
        release_hold();
    }' || STATUS=1

# 10. A failed submission keeps its hold: the gate stays permanently busy and
#     every later arrival sees the stale in-flight timestamp as the barrier.
run_mutation hold_never_released admission.hpp \
    '        if (0U != holds_) {
            --holds_;
            if (0U == holds_) {
                hold_ns_ = 0U;
            }
        }' \
    '        /* mutated: the hold is never released */' || STATUS=1

# 11. The policy reports an unlimited backlog, so S10's capped submission is
#     queued instead of rejected and the rule set stops enforcing its cap.
run_mutation backlog_cap_ignored admission.hpp \
    '    constexpr uint16_t backlog_cap(TargetId target) const noexcept
    {
        return limits_for(target).max_backlog.value;
    }' \
    '    constexpr uint16_t backlog_cap(TargetId) const noexcept
    {
        return 0xFFFFU;
    }' || STATUS=1

# 12. The snapshot ring stops being opt-in: a Config without
#     kAdmissionSnapshotSlots still pays for eight slots, so S9's
#     "disabled costs nothing" pair collapses.
run_mutation snapshot_ring_ignores_config monitor.hpp \
    'template <typename C, typename = void>
struct admission_snapshot_slots {
    static constexpr uint8_t value = 0U;
};' \
    'template <typename C, typename = void>
struct admission_snapshot_slots {
    static constexpr uint8_t value = 8U;
};' || STATUS=1

# 13. Eviction is no longer reported: a wrapped sequence reads the slot that
#     now holds a newer record, so S7's evicted lookup degrades to kTorn.
run_mutation snapshot_eviction_ignored monitor.hpp \
    '        if ((total - seq) >= static_cast<uint64_t>(kSlots)) {
            return Result::error(SnapshotError::kEvicted);
        }' \
    '        if (false) {
            return Result::error(SnapshotError::kEvicted);
        }' || STATUS=1

if [ "$STATUS" -ne 0 ]; then
    exit 1
fi
echo "ALL MUTATIONS CAUGHT"
