// coact monitor (M6) host tests.
// SPDX-License-Identifier: MIT
#include "coact/fault.hpp"
#include "coact/monitor.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "test/test_harness.hpp"

namespace {

using coact::Breaker;
using coact::BreakerBank;
using coact::BreakerLevel;
using coact::DefaultConfig;
using coact::FaultPriority;
using coact::FaultReporter;
using coact::kInvalidTarget;
using coact::Monitor;
using coact::PriorityClass;
using coact::RejectReason;
using coact::SubmitDisposition;
using coact::TargetId;
using coact::TraceOps;

constexpr TargetId kSlowAo(1U);
constexpr TargetId kOtherAo(2U);

struct DefaultBreaker final : Breaker<> {
    DefaultBreaker() noexcept : Breaker<>(DefaultConfig{}) {}
};

static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "Breaker state requires lock-free 32-bit atomics");
static_assert(sizeof(Breaker<>) <= 8U,
              "Breaker mutable state must fit in one packed 32-bit word");

// Drive a breaker into Recovering via the L1 -> cooldown -> low-water path.
void drive_to_recovering(Breaker<>& b, const DefaultConfig& cfg) {
    for (int32_t i = 0; i < static_cast<int32_t>(Breaker<>::kDirectTimeoutThreshold); ++i) {
        b.on_direct_timeout();
    }
    REQUIRE_EQ(b.level(), BreakerLevel::BrokenL1);
    for (int32_t i = 0; i < static_cast<int32_t>(cfg.kCooldownCycles); ++i) {
        b.on_dispatch_cycle();
    }
    for (int32_t i = 0; i < static_cast<int32_t>(Breaker<>::kLowWatermarkPersist); ++i) {
        b.on_watermark(45U);
    }
    REQUIRE_EQ(b.level(), BreakerLevel::Recovering);
}

// ---------------------------------------------------------------------------
// State transitions
// ---------------------------------------------------------------------------

COACT_TEST(breaker_three_direct_timeouts_to_l1) {
    DefaultConfig cfg;
    Breaker<> b(cfg);
    REQUIRE_EQ(b.level(), BreakerLevel::Normal);
    CHECK(b.direct_allowed(kSlowAo));

    b.on_direct_timeout();
    b.on_direct_timeout();
    CHECK_EQ(b.level(), BreakerLevel::Normal);  // 2 consecutive timeouts is not enough
    b.on_direct_timeout();
    CHECK_EQ(b.level(), BreakerLevel::BrokenL1);
    CHECK(!b.direct_allowed(kSlowAo));          // L1 revokes that AO's direct
}

COACT_TEST(breaker_l1_to_l2_via_sustained_high_watermark) {
    DefaultConfig cfg;
    Breaker<> b(cfg);
    b.on_direct_timeout();
    b.on_direct_timeout();
    b.on_direct_timeout();
    REQUIRE_EQ(b.level(), BreakerLevel::BrokenL1);

    b.on_watermark(85U);
    b.on_watermark(85U);
    CHECK_EQ(b.level(), BreakerLevel::BrokenL1);  // not yet persistent
    b.on_watermark(85U);                          // backlog keeps growing
    CHECK_EQ(b.level(), BreakerLevel::BrokenL2);
    CHECK(b.drop_non_critical());
    CHECK(!b.safe_events_only());                 // safety events still flow
}

COACT_TEST(breaker_three_rtc_timeouts_to_l2) {
    DefaultConfig cfg;
    Breaker<> b(cfg);
    b.on_dispatcher_rtc_timeout();
    b.on_dispatcher_rtc_timeout();
    CHECK_EQ(b.level(), BreakerLevel::Normal);
    b.on_dispatcher_rtc_timeout();
    CHECK_EQ(b.level(), BreakerLevel::BrokenL2);
}

COACT_TEST(breaker_overflow_to_l2) {
    DefaultConfig cfg;
    Breaker<> b(cfg);
    b.on_overflow();
    CHECK_EQ(b.level(), BreakerLevel::BrokenL2);
}

COACT_TEST(breaker_persistent_watermark_violation_to_l2) {
    DefaultConfig cfg;
    Breaker<> b(cfg);
    b.on_watermark_violation();
    b.on_watermark_violation();
    CHECK_EQ(b.level(), BreakerLevel::Normal);
    b.on_watermark_violation();
    CHECK_EQ(b.level(), BreakerLevel::BrokenL2);
}

COACT_TEST(breaker_key_reserve_exhausted_to_safe) {
    DefaultConfig cfg;
    Breaker<> b(cfg);
    b.on_key_reserve_exhausted();
    CHECK_EQ(b.level(), BreakerLevel::Safe);
    CHECK(b.safe_events_only());
    CHECK(b.drop_non_critical());

    // From L2 as well.
    Breaker<> b2(cfg);
    b2.on_overflow();
    REQUIRE_EQ(b2.level(), BreakerLevel::BrokenL2);
    b2.on_key_reserve_exhausted();
    CHECK_EQ(b2.level(), BreakerLevel::Safe);

    // Safe is the most degraded level: further violations do not escalate.
    b2.on_overflow();
    b2.on_watchdog();
    CHECK_EQ(b2.level(), BreakerLevel::Safe);
}

COACT_TEST(breaker_watchdog_to_safe) {
    DefaultConfig cfg;
    Breaker<> b(cfg);
    b.on_watchdog();
    CHECK_EQ(b.level(), BreakerLevel::Safe);
}

COACT_TEST(breaker_safe_external_restore_to_recovering) {
    DefaultConfig cfg;
    Breaker<> b(cfg);
    b.on_key_reserve_exhausted();
    REQUIRE_EQ(b.level(), BreakerLevel::Safe);
    b.on_external_safe_restore();
    CHECK_EQ(b.level(), BreakerLevel::Recovering);
    CHECK(!b.healthy_window_passed());

    // Still needs cooldown + sustained low watermark + consecutive healthy
    // probe windows before the breaker returns to Normal.
    for (int32_t i = 0; i < static_cast<int32_t>(cfg.kCooldownCycles); ++i) {
        b.on_dispatch_cycle();
    }
    for (int32_t i = 0; i < static_cast<int32_t>(Breaker<>::kLowWatermarkPersist); ++i) {
        b.on_watermark(45U);
    }
    for (int32_t i = 0; i < static_cast<int32_t>(Breaker<>::kHealthyWindowsRequired); ++i) {
        b.on_probe_success();
        b.on_dispatch_cycle();
    }
    CHECK_EQ(b.level(), BreakerLevel::Normal);
    CHECK(b.healthy_window_passed());
    CHECK(b.direct_allowed(kSlowAo));
}

COACT_TEST(breaker_recovering_healthy_probes_to_normal) {
    DefaultConfig cfg;
    Breaker<> b(cfg);
    drive_to_recovering(b, cfg);
    CHECK_EQ(b.level(), BreakerLevel::Recovering);

    // One successful dispatch is not enough to recover.
    b.on_probe_success();
    b.on_dispatch_cycle();
    CHECK_EQ(b.level(), BreakerLevel::Recovering);
    CHECK(!b.healthy_window_passed());

    b.on_probe_success();
    b.on_dispatch_cycle();
    CHECK_EQ(b.level(), BreakerLevel::Recovering);  // 2 healthy windows

    b.on_probe_success();
    b.on_dispatch_cycle();
    CHECK_EQ(b.level(), BreakerLevel::Normal);      // 3 healthy windows
    CHECK(b.healthy_window_passed());
    CHECK(b.direct_allowed(kSlowAo));
}

COACT_TEST(breaker_probe_failure_back_to_l2) {
    DefaultConfig cfg;
    Breaker<> b(cfg);
    drive_to_recovering(b, cfg);
    b.on_probe_failure();
    CHECK_EQ(b.level(), BreakerLevel::BrokenL2);
    CHECK(!b.healthy_window_passed());
}

COACT_TEST(breaker_rtc_timeout_in_recovering_back_to_l2) {
    DefaultConfig cfg;
    Breaker<> b(cfg);
    drive_to_recovering(b, cfg);
    b.on_dispatcher_rtc_timeout();
    CHECK_EQ(b.level(), BreakerLevel::BrokenL2);
}

COACT_TEST(breaker_overflow_in_recovering_back_to_l2) {
    DefaultConfig cfg;
    Breaker<> b(cfg);
    drive_to_recovering(b, cfg);
    b.on_overflow();
    CHECK_EQ(b.level(), BreakerLevel::BrokenL2);
}

// ---------------------------------------------------------------------------
// Cooldown and qualifying-call semantics (12.3/12.4)
// ---------------------------------------------------------------------------

COACT_TEST(breaker_cooldown_not_done_blocks_recovery) {
    DefaultConfig cfg;
    Breaker<> b(cfg);
    b.on_direct_timeout();
    b.on_direct_timeout();
    b.on_direct_timeout();
    REQUIRE_EQ(b.level(), BreakerLevel::BrokenL1);

    // Sustained low watermark while the cooldown is incomplete: no recovery.
    for (int32_t i = 0; i < static_cast<int32_t>(Breaker<>::kLowWatermarkPersist); ++i) {
        b.on_watermark(45U);
    }
    CHECK_EQ(b.level(), BreakerLevel::BrokenL1);

    // One cooldown cycle short is still not enough.
    for (int32_t i = 0; i < static_cast<int32_t>(cfg.kCooldownCycles) - 1; ++i) {
        b.on_dispatch_cycle();
    }
    b.on_watermark(45U);
    CHECK_EQ(b.level(), BreakerLevel::BrokenL1);

    // The final cooldown cycle unlocks recovery.
    b.on_dispatch_cycle();
    b.on_watermark(45U);
    CHECK_EQ(b.level(), BreakerLevel::Recovering);
}

COACT_TEST(breaker_rtc_ok_clears_timeout_count_but_not_cooldown) {
    DefaultConfig cfg;
    Breaker<> b(cfg);

    // Two consecutive direct timeouts, then a qualifying call resets the
    // consecutive counter so two more timeouts do NOT trip L1.
    b.on_direct_timeout();
    b.on_direct_timeout();
    b.on_rtc_ok();
    b.on_direct_timeout();
    b.on_direct_timeout();
    CHECK_EQ(b.level(), BreakerLevel::Normal);  // would be L1 if not cleared
    b.on_direct_timeout();
    CHECK_EQ(b.level(), BreakerLevel::BrokenL1);

    // The qualifying call must not skip the cooldown window.
    b.on_rtc_ok();
    for (int32_t i = 0; i < static_cast<int32_t>(Breaker<>::kLowWatermarkPersist); ++i) {
        b.on_watermark(45U);
    }
    CHECK_EQ(b.level(), BreakerLevel::BrokenL1);  // cooldown still running

    for (int32_t i = 0; i < static_cast<int32_t>(cfg.kCooldownCycles); ++i) {
        b.on_dispatch_cycle();
    }
    b.on_watermark(45U);
    CHECK_EQ(b.level(), BreakerLevel::Recovering);
}

COACT_TEST(breaker_recovery_hysteresis_low_watermark) {
    DefaultConfig cfg;
    Breaker<> b(cfg);
    drive_to_recovering(b, cfg);

    // A brief rise to 60% (still below the 80% violation band) aborts
    // recovery immediately and drops the breaker back to L2.
    b.on_watermark(60U);
    CHECK_EQ(b.level(), BreakerLevel::BrokenL2);
    CHECK(b.drop_non_critical());
    CHECK(!b.safe_events_only());
}

// ---------------------------------------------------------------------------
// direct_allowed and graded actions
// ---------------------------------------------------------------------------

COACT_TEST(breaker_direct_allowed_per_ao) {
    DefaultConfig cfg;
    BreakerBank<> breakers(cfg);

    // The offending AO's breaker enters L1 and revokes its direct...
    breakers.on_direct_timeout(kSlowAo);
    breakers.on_direct_timeout(kSlowAo);
    breakers.on_direct_timeout(kSlowAo);
    REQUIRE_EQ(breakers.level(kSlowAo), BreakerLevel::BrokenL1);
    CHECK(!breakers.direct_allowed(kSlowAo));

    // ...while another AO's breaker stays Normal and keeps its direct.
    CHECK_EQ(breakers.level(kOtherAo), BreakerLevel::Normal);
    CHECK(breakers.direct_allowed(kOtherAo));
}

COACT_TEST(breaker_bank_global_watchdog_is_explicit_broadcast) {
    BreakerBank<> breakers(DefaultConfig{});

    breakers.broadcast_watchdog();

    CHECK_EQ(breakers.level(kSlowAo), BreakerLevel::Safe);
    CHECK_EQ(breakers.level(kOtherAo), BreakerLevel::Safe);
}

COACT_TEST(breaker_bank_invalid_targets_are_fail_safe) {
    BreakerBank<> breakers(DefaultConfig{});
    constexpr TargetId kOutOfRange(
        static_cast<uint8_t>(DefaultConfig::kMaxAo + 1U));

    const auto exercise_invalid_target = [&breakers](TargetId target) {
        breakers.on_direct_timeout(target);
        breakers.on_dispatcher_rtc_timeout(target);
        breakers.on_overflow(target);
        breakers.on_dispatch_cycle(target);
        breakers.on_probe_success(target);
        breakers.on_probe_failure(target);
        breakers.on_rtc_ok(target);

        CHECK_EQ(breakers.level(target), BreakerLevel::Safe);
        CHECK(!breakers.direct_allowed(target));
        CHECK(breakers.drop_non_critical(target));
        CHECK(breakers.safe_events_only(target));
    };

    exercise_invalid_target(kInvalidTarget);
    exercise_invalid_target(kOutOfRange);

    CHECK_EQ(breakers.level(kSlowAo), BreakerLevel::Normal);
    CHECK(breakers.direct_allowed(kSlowAo));
}

COACT_TEST(breaker_l2_drops_non_critical_keeps_safety) {
    DefaultConfig cfg;
    Breaker<> b(cfg);
    b.on_overflow();
    REQUIRE_EQ(b.level(), BreakerLevel::BrokenL2);
    CHECK(!b.direct_allowed(kSlowAo));
    CHECK(b.drop_non_critical());     // non-critical inputs are dropped
    CHECK(!b.safe_events_only());     // safety events are still admitted
}

COACT_TEST(breaker_safe_allows_only_safety_events) {
    DefaultConfig cfg;
    Breaker<> b(cfg);
    b.on_watchdog();
    REQUIRE_EQ(b.level(), BreakerLevel::Safe);
    CHECK(b.safe_events_only());
    CHECK(b.drop_non_critical());
    CHECK(!b.direct_allowed(kSlowAo));
}

COACT_TEST(breaker_concurrent_safe_and_overflow_stays_safe) {
    constexpr uint32_t kRounds = 20000U;
    std::unique_ptr<DefaultBreaker[]> breakers(new DefaultBreaker[kRounds]);
    std::atomic<uint32_t> round{0U};
    std::atomic<uint32_t> completed{0U};

    std::thread watchdog([&]() {
        for (uint32_t i = 0U; i < kRounds; ++i) {
            while (round.load(std::memory_order_acquire) <= i) {
            }
            breakers[i].on_watchdog();
            completed.fetch_add(1U, std::memory_order_release);
        }
    });
    std::thread overflow([&]() {
        for (uint32_t i = 0U; i < kRounds; ++i) {
            while (round.load(std::memory_order_acquire) <= i) {
            }
            breakers[i].on_overflow();
            completed.fetch_add(1U, std::memory_order_release);
        }
    });

    for (uint32_t i = 0U; i < kRounds; ++i) {
        round.store(i + 1U, std::memory_order_release);
        while (completed.load(std::memory_order_acquire) < ((i + 1U) * 2U)) {
        }
        CHECK_EQ(breakers[i].level(), BreakerLevel::Safe);
    }
    watchdog.join();
    overflow.join();
}

// ---------------------------------------------------------------------------
// Monitor counters
// ---------------------------------------------------------------------------

COACT_TEST(monitor_counts_dispositions) {
    Monitor m;
    m.record_disposition(SubmitDisposition::Merged);
    m.record_disposition(SubmitDisposition::Merged);
    m.record_disposition(SubmitDisposition::DroppedPolicy);
    m.record_disposition(SubmitDisposition::DroppedRateLimit);
    m.record_disposition(SubmitDisposition::DroppedOverload);
    m.record_disposition(SubmitDisposition::Direct);   // not a disposition class
    m.record_disposition(SubmitDisposition::Queued);
    m.record_disposition(SubmitDisposition::RejectedFull);
    m.record_disposition(SubmitDisposition::RejectedState);

    CHECK_EQ(coact_test::relaxed(m.global().disposition_merge), 2U);
    CHECK_EQ(coact_test::relaxed(m.global().disposition_filter), 1U);
    CHECK_EQ(coact_test::relaxed(m.global().disposition_rate_limit), 1U);
    CHECK_EQ(coact_test::relaxed(m.global().disposition_overload), 1U);
}

COACT_TEST(monitor_counts_per_ao_and_global) {
    Monitor m;

    m.add_direct_duration(kSlowAo, 1000ULL);
    m.add_direct_duration(kSlowAo, 500ULL);
    m.add_dispatcher_duration(kSlowAo, 2000ULL);
    m.record_direct_timeout(kSlowAo);
    m.record_rtc_timeout(kSlowAo);

    CHECK_EQ(coact_test::relaxed(m.ao(kSlowAo).direct_duration_ns), 1500ULL);
    CHECK_EQ(coact_test::relaxed(m.ao(kSlowAo).dispatcher_duration_ns), 2000ULL);
    CHECK_EQ(coact_test::relaxed(m.ao(kSlowAo).direct_timeouts), 1U);
    CHECK_EQ(coact_test::relaxed(m.ao(kSlowAo).rtc_timeouts), 1U);

    m.record_rejection(kSlowAo, RejectReason::kC2Priority);
    m.record_rejection(kSlowAo, RejectReason::kC2Priority);
    m.record_rejection(kSlowAo, RejectReason::kC5LeaseBusy);
    CHECK_EQ(coact_test::relaxed(m.ao(kSlowAo).rejections[static_cast<size_t>(RejectReason::kC2Priority)]), 2U);
    CHECK_EQ(coact_test::relaxed(m.ao(kSlowAo).rejections[static_cast<size_t>(RejectReason::kC5LeaseBusy)]), 1U);
    CHECK_EQ(coact_test::relaxed(m.ao(kSlowAo).rejections[static_cast<size_t>(RejectReason::kC1Eligibility)]), 0U);

    m.record_lease_contention(kSlowAo);
    CHECK_EQ(coact_test::relaxed(m.ao(kSlowAo).lease_contention), 1U);

    m.record_pending(kSlowAo, 5U);
    m.record_pending(kSlowAo, 8U);
    m.record_pending(kSlowAo, 3U);
    CHECK_EQ(coact_test::relaxed(m.ao(kSlowAo).pending_max), 8U);
    CHECK_EQ(coact_test::relaxed(m.ao(kSlowAo).pending), 3U);
    CHECK_EQ(coact_test::relaxed(m.global().pending_max), 8U);

    m.sample_watermark(PriorityClass::High, 85U);
    m.sample_watermark(PriorityClass::High, 100U);
    m.sample_watermark(PriorityClass::Low, 40U);
    CHECK_EQ(coact_test::relaxed(m.global().watermark_pct[0]), 100U);    // High
    CHECK_EQ(coact_test::relaxed(m.global().high_water_count[0]), 2U);
    CHECK_EQ(coact_test::relaxed(m.global().full_count[0]), 1U);
    CHECK_EQ(coact_test::relaxed(m.global().watermark_pct[2]), 40U);     // Low
    CHECK_EQ(coact_test::relaxed(m.global().high_water_count[2]), 0U);
    CHECK_EQ(coact_test::relaxed(m.global().full_count[2]), 0U);

    m.record_overflow();
    m.heartbeat();
    m.heartbeat();
    m.record_platform_fault();
    CHECK_EQ(coact_test::relaxed(m.global().overflow), 1U);
    CHECK_EQ(coact_test::relaxed(m.global().watchdog_heartbeats), 2U);
    CHECK_EQ(coact_test::relaxed(m.global().platform_faults), 1U);
}

COACT_TEST(monitor_invalid_target_safely_ignored) {
    Monitor m;
    m.record_rejection(kInvalidTarget, RejectReason::kC1Eligibility);
    m.record_pending(kInvalidTarget, 3U);
    m.add_direct_duration(kInvalidTarget, 100ULL);
    CHECK_EQ(coact_test::relaxed(m.ao(kInvalidTarget).direct_timeouts), 0U);
    CHECK_EQ(coact_test::relaxed(m.ao(kInvalidTarget).pending_max), 0U);
    CHECK_EQ(coact_test::relaxed(m.ao(kInvalidTarget).direct_duration_ns), 0ULL);
}

// ---------------------------------------------------------------------------
// TraceOps forwarding (core-neutral trace boundary, design trace §2.1/§3.1)
// ---------------------------------------------------------------------------

struct CapturedTrace {
    uint16_t last_source{0};
    uint32_t last_target_raw{0};
    uint16_t last_signal{0};
    uint8_t last_disposition{0};
    uint32_t last_reason{0};
    uint64_t last_elapsed{0};
    uint8_t last_path{0};
    uint8_t last_timeout{0};
    uint8_t last_kind{0};
    bool last_from_isr{false};
};

static void capture_submit(void* ctx, uint16_t source_id, coact::TargetId target,
                           uint16_t signal, uint8_t disposition,
                           uint32_t reason, bool from_isr) noexcept
{
    CapturedTrace* cap = static_cast<CapturedTrace*>(ctx);
    cap->last_source = source_id;
    cap->last_target_raw = target.raw();
    cap->last_signal = signal;
    cap->last_disposition = disposition;
    cap->last_reason = reason;
    cap->last_from_isr = from_isr;
}

static void capture_dispatch(void* ctx, coact::TargetId target,
                             uint64_t elapsed_ns, uint8_t path,
                             uint8_t timeout) noexcept
{
    CapturedTrace* cap = static_cast<CapturedTrace*>(ctx);
    cap->last_target_raw = target.raw();
    cap->last_elapsed = elapsed_ns;
    cap->last_path = path;
    cap->last_timeout = timeout;
}

static void capture_lease(void* ctx, coact::TargetId target, uint8_t kind,
                          uint64_t elapsed_ns) noexcept
{
    CapturedTrace* cap = static_cast<CapturedTrace*>(ctx);
    cap->last_target_raw = target.raw();
    cap->last_kind = kind;
    cap->last_elapsed = elapsed_ns;
}

COACT_TEST(trace_ops_null_default_noop) {
    Monitor<> m;
    m.trace_submit(0U, kSlowAo, 1U, SubmitDisposition::Direct, 0U, false);
    m.trace_submit(0U, kSlowAo, 1U, SubmitDisposition::Direct, 0U, true);
    m.trace_dispatch(kSlowAo, 1000ULL, 1U, 0U);
    m.trace_lease_contention(kSlowAo, 1U, 2000ULL);
    CHECK_EQ(coact_test::relaxed(m.global().overflow), 0U);
}

COACT_TEST(trace_ops_forwards_submit) {
    Monitor<> m;
    CapturedTrace captured{};
    TraceOps ops{};
    ops.on_submit = &capture_submit;
    ops.ctx = &captured;
    m.bind_trace(ops);

    m.trace_submit(3U, kOtherAo, 42U, SubmitDisposition::DroppedRateLimit, 7U,
                   false);

    CHECK_EQ(captured.last_source, 3U);
    CHECK_EQ(captured.last_target_raw, 2U);
    CHECK_EQ(captured.last_signal, 42U);
    CHECK_EQ(captured.last_disposition,
             static_cast<uint8_t>(SubmitDisposition::DroppedRateLimit));
    CHECK_EQ(captured.last_reason, 7U);
    CHECK(captured.last_from_isr == false);
}

// The ISR flag must reach the sink so the adapter can pick the ISR-safe
// record entry (review P0-1: an ISR-context submit must never log through
// the task-only entry).
COACT_TEST(trace_ops_forwards_submit_isr_flag) {
    Monitor<> m;
    CapturedTrace captured{};
    TraceOps ops{};
    ops.on_submit = &capture_submit;
    ops.ctx = &captured;
    m.bind_trace(ops);

    m.trace_submit(3U, kOtherAo, 42U, SubmitDisposition::Queued, 0U, true);

    CHECK_EQ(captured.last_source, 3U);
    CHECK_EQ(captured.last_signal, 42U);
    CHECK_EQ(captured.last_disposition,
             static_cast<uint8_t>(SubmitDisposition::Queued));
    CHECK(captured.last_from_isr == true);
}

COACT_TEST(trace_ops_forwards_dispatch) {
    Monitor<> m;
    CapturedTrace captured{};
    TraceOps ops{};
    ops.on_dispatch = &capture_dispatch;
    ops.ctx = &captured;
    m.bind_trace(ops);

    m.trace_dispatch(kSlowAo, 123456ULL, 1U, 0U);

    CHECK_EQ(captured.last_target_raw, 1U);
    CHECK_EQ(captured.last_elapsed, 123456ULL);
    CHECK_EQ(captured.last_path, 1U);
    CHECK_EQ(captured.last_timeout, 0U);
}

COACT_TEST(trace_ops_forwards_lease) {
    Monitor<> m;
    CapturedTrace captured{};
    TraceOps ops{};
    ops.on_lease_contention = &capture_lease;
    ops.ctx = &captured;
    m.bind_trace(ops);

    m.trace_lease_contention(kOtherAo, 2U, 999ULL);

    CHECK_EQ(captured.last_target_raw, 2U);
    CHECK_EQ(captured.last_kind, 2U);
    CHECK_EQ(captured.last_elapsed, 999ULL);
}

// ---------------------------------------------------------------------------
// FaultReporter and state_crossed (fault injection boundary, R1)
// ---------------------------------------------------------------------------

COACT_TEST(state_crossed_up_and_down) {
    constexpr uint8_t kThreshold = 80U;
    // Crossing up: prev below, curr at or above the threshold.
    CHECK(coact::state_crossed<uint8_t>(79U, 80U, kThreshold));
    // Crossing down: prev at or above, curr below the threshold.
    CHECK(coact::state_crossed<uint8_t>(80U, 79U, kThreshold));
    // Steady high: both sides above, no state change.
    CHECK(coact::state_crossed<uint8_t>(85U, 90U, kThreshold) == false);
    // Both sides below: no state change.
    CHECK(coact::state_crossed<uint8_t>(50U, 60U, kThreshold) == false);

    // i32 overload carries the same edge semantics.
    CHECK(coact::state_crossed<int32_t>(79, 80, 80));
    CHECK(coact::state_crossed<int32_t>(80, 79, 80));
    CHECK(coact::state_crossed<int32_t>(85, 90, 80) == false);
    CHECK(coact::state_crossed<int32_t>(50, 60, 80) == false);
    // Negative values must work for the i32 overload.
    CHECK(coact::state_crossed<int32_t>(-1, 0, 0));
}

COACT_TEST(fault_reporter_null_is_noop) {
    FaultReporter reporter{};  // default: fn == nullptr
    reporter.report(0U, 0U, FaultPriority::kHigh);  // must not crash
    CHECK(reporter.fn == nullptr);
    CHECK_EQ(static_cast<uint8_t>(FaultPriority::kCritical), 3U);
}

struct CapturedFault {
    uint32_t calls{0};
    uint16_t last_index{0};
    uint32_t last_detail{0};
    FaultPriority last_priority{FaultPriority::kLow};
};

static void capture_fault(uint16_t fault_index, uint32_t detail,
                          FaultPriority priority, void* ctx) noexcept
{
    CapturedFault* cap = static_cast<CapturedFault*>(ctx);
    cap->calls++;
    cap->last_index = fault_index;
    cap->last_detail = detail;
    cap->last_priority = priority;
}

COACT_TEST(fault_reporter_forwards) {
    CapturedFault captured{};
    FaultReporter reporter{};
    reporter.fn = &capture_fault;
    reporter.ctx = &captured;

    reporter.report(2U, 0x1234U, FaultPriority::kCritical);

    CHECK_EQ(captured.calls, 1U);
    CHECK_EQ(captured.last_index, 2U);
    CHECK_EQ(captured.last_detail, 0x1234U);
    CHECK_EQ(captured.last_priority, FaultPriority::kCritical);
}

COACT_TEST(monitor_watermark_crossing_reports_fault) {
    Monitor<> m;
    CapturedFault captured{};
    FaultReporter reporter{};
    reporter.fn = &capture_fault;
    reporter.ctx = &captured;
    m.bind_fault(reporter);

    m.sample_watermark(PriorityClass::High, 79U);  // below, no fault
    CHECK_EQ(captured.calls, 0U);

    m.sample_watermark(PriorityClass::High, 85U);  // crossing up: 79 -> 85
    CHECK_EQ(captured.calls, 1U);
    CHECK_EQ(captured.last_index, 0U);             // High partition
    CHECK_EQ(captured.last_detail, (85U << 8) | 0U);
    CHECK_EQ(captured.last_priority, FaultPriority::kHigh);

    m.sample_watermark(PriorityClass::High, 50U);  // crossing down: 85 -> 50
    CHECK_EQ(captured.calls, 2U);
    CHECK_EQ(captured.last_priority, FaultPriority::kMedium);
    CHECK_EQ(captured.last_detail, (50U << 8) | 0U);

    // Counters keep their original semantics alongside the fault reports.
    CHECK_EQ(coact_test::relaxed(m.global().high_water_count[0]), 1U);
    CHECK_EQ(coact_test::relaxed(m.global().watermark_pct[0]), 50U);
}

COACT_TEST(monitor_watermark_steady_no_repeat) {
    Monitor<> m;
    CapturedFault captured{};
    FaultReporter reporter{};
    reporter.fn = &capture_fault;
    reporter.ctx = &captured;
    m.bind_fault(reporter);

    m.sample_watermark(PriorityClass::High, 85U);  // first sample: prev=0, up-cross
    CHECK_EQ(captured.calls, 1U);
    CHECK_EQ(captured.last_priority, FaultPriority::kHigh);

    m.sample_watermark(PriorityClass::High, 85U);  // steady high: no repeat
    m.sample_watermark(PriorityClass::High, 85U);  // steady high: no repeat
    CHECK_EQ(captured.calls, 1U);

    CHECK_EQ(coact_test::relaxed(m.global().high_water_count[0]), 3U);
}

COACT_TEST(monitor_watermark_snapshot_records_usage) {
    Monitor<> m;

    m.sample_watermark(PriorityClass::Normal, 25U, 16U, 64U);

    CHECK_EQ(coact_test::relaxed(m.global().watermark_samples[1]), 1U);
    CHECK_EQ(coact_test::relaxed(m.global().watermark_used[1]), 16U);
    CHECK_EQ(coact_test::relaxed(m.global().watermark_capacity[1]), 64U);
}

}  // namespace

COACT_TEST_MAIN()
