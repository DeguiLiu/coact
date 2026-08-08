// coact monitor (M6) host tests.
// SPDX-License-Identifier: MIT
#include "coact/monitor.hpp"

#include <cstdint>

#include "test/test_harness.hpp"

namespace {

using coact::Breaker;
using coact::BreakerLevel;
using coact::DefaultConfig;
using coact::kInvalidTarget;
using coact::Monitor;
using coact::PriorityClass;
using coact::RejectReason;
using coact::SubmitDisposition;
using coact::TargetId;

constexpr TargetId kSlowAo = 1U;
constexpr TargetId kOtherAo = 2U;

// Drive a breaker into Recovering via the L1 -> cooldown -> low-water path.
void drive_to_recovering(Breaker<>& b, const DefaultConfig& cfg) {
    for (int i = 0; i < static_cast<int>(Breaker<>::kDirectTimeoutThreshold); ++i) {
        b.on_direct_timeout();
    }
    REQUIRE_EQ(b.level(), BreakerLevel::BrokenL1);
    for (int i = 0; i < static_cast<int>(cfg.kCooldownCycles); ++i) {
        b.on_dispatch_cycle();
    }
    for (int i = 0; i < static_cast<int>(Breaker<>::kLowWatermarkPersist); ++i) {
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
    for (int i = 0; i < static_cast<int>(cfg.kCooldownCycles); ++i) {
        b.on_dispatch_cycle();
    }
    for (int i = 0; i < static_cast<int>(Breaker<>::kLowWatermarkPersist); ++i) {
        b.on_watermark(45U);
    }
    for (int i = 0; i < static_cast<int>(Breaker<>::kHealthyWindowsRequired); ++i) {
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
    for (int i = 0; i < static_cast<int>(Breaker<>::kLowWatermarkPersist); ++i) {
        b.on_watermark(45U);
    }
    CHECK_EQ(b.level(), BreakerLevel::BrokenL1);

    // One cooldown cycle short is still not enough.
    for (int i = 0; i < static_cast<int>(cfg.kCooldownCycles) - 1; ++i) {
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
    for (int i = 0; i < static_cast<int>(Breaker<>::kLowWatermarkPersist); ++i) {
        b.on_watermark(45U);
    }
    CHECK_EQ(b.level(), BreakerLevel::BrokenL1);  // cooldown still running

    for (int i = 0; i < static_cast<int>(cfg.kCooldownCycles); ++i) {
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
    Breaker<> slow(cfg);
    Breaker<> other(cfg);

    // The offending AO's breaker enters L1 and revokes its direct...
    slow.on_direct_timeout();
    slow.on_direct_timeout();
    slow.on_direct_timeout();
    REQUIRE_EQ(slow.level(), BreakerLevel::BrokenL1);
    CHECK(!slow.direct_allowed(kSlowAo));

    // ...while another AO's breaker stays Normal and keeps its direct.
    CHECK_EQ(other.level(), BreakerLevel::Normal);
    CHECK(other.direct_allowed(kOtherAo));
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

    CHECK_EQ(m.global().disposition_merge, 2U);
    CHECK_EQ(m.global().disposition_filter, 1U);
    CHECK_EQ(m.global().disposition_rate_limit, 1U);
    CHECK_EQ(m.global().disposition_overload, 1U);
}

COACT_TEST(monitor_counts_per_ao_and_global) {
    Monitor m;

    m.add_direct_duration(kSlowAo, 1000ULL);
    m.add_direct_duration(kSlowAo, 500ULL);
    m.add_dispatcher_duration(kSlowAo, 2000ULL);
    m.record_direct_timeout(kSlowAo);
    m.record_rtc_timeout(kSlowAo);

    CHECK_EQ(m.ao(kSlowAo).direct_duration_ns, 1500ULL);
    CHECK_EQ(m.ao(kSlowAo).dispatcher_duration_ns, 2000ULL);
    CHECK_EQ(m.ao(kSlowAo).direct_timeouts, 1U);
    CHECK_EQ(m.ao(kSlowAo).rtc_timeouts, 1U);

    m.record_rejection(kSlowAo, RejectReason::kC2Priority);
    m.record_rejection(kSlowAo, RejectReason::kC2Priority);
    m.record_rejection(kSlowAo, RejectReason::kC5LeaseBusy);
    CHECK_EQ(m.ao(kSlowAo).rejections[static_cast<size_t>(RejectReason::kC2Priority)], 2U);
    CHECK_EQ(m.ao(kSlowAo).rejections[static_cast<size_t>(RejectReason::kC5LeaseBusy)], 1U);
    CHECK_EQ(m.ao(kSlowAo).rejections[static_cast<size_t>(RejectReason::kC1Eligibility)], 0U);

    m.record_lease_contention(kSlowAo);
    CHECK_EQ(m.ao(kSlowAo).lease_contention, 1U);

    m.record_pending(kSlowAo, 5U);
    m.record_pending(kSlowAo, 8U);
    m.record_pending(kSlowAo, 3U);
    CHECK_EQ(m.ao(kSlowAo).pending_max, 8U);
    CHECK_EQ(m.ao(kSlowAo).pending, 3U);
    CHECK_EQ(m.global().pending_max, 8U);

    m.sample_watermark(PriorityClass::High, 85U);
    m.sample_watermark(PriorityClass::High, 100U);
    m.sample_watermark(PriorityClass::Low, 40U);
    CHECK_EQ(m.global().watermark_pct[0], 100U);    // High
    CHECK_EQ(m.global().high_water_count[0], 2U);
    CHECK_EQ(m.global().full_count[0], 1U);
    CHECK_EQ(m.global().watermark_pct[2], 40U);     // Low
    CHECK_EQ(m.global().high_water_count[2], 0U);
    CHECK_EQ(m.global().full_count[2], 0U);

    m.record_overflow();
    m.heartbeat();
    m.heartbeat();
    m.record_platform_fault();
    CHECK_EQ(m.global().overflow, 1U);
    CHECK_EQ(m.global().watchdog_heartbeats, 2U);
    CHECK_EQ(m.global().platform_faults, 1U);
}

COACT_TEST(monitor_invalid_target_safely_ignored) {
    Monitor m;
    m.record_rejection(kInvalidTarget, RejectReason::kC1Eligibility);
    m.record_pending(kInvalidTarget, 3U);
    m.add_direct_duration(kInvalidTarget, 100ULL);
    CHECK_EQ(m.ao(kInvalidTarget).direct_timeouts, 0U);
    CHECK_EQ(m.ao(kInvalidTarget).pending_max, 0U);
    CHECK_EQ(m.ao(kInvalidTarget).direct_duration_ns, 0ULL);
}

}  // namespace

COACT_TEST_MAIN()
