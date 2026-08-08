// coact monitor and circuit breaker (M6).
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "coact/config.hpp"

namespace coact {

// ---------------------------------------------------------------------------
// BreakerLevel: the external contract keeps a two-state NORMAL/BROKEN view;
// the broken state is split internally into BrokenL1 (per-AO direct revoked)
// and BrokenL2 (slow AO quarantined). Safe and Recovering are system-wide.
// See design 12.2.
// ---------------------------------------------------------------------------
enum class BreakerLevel : uint8_t {
    Normal,
    BrokenL1,
    BrokenL2,
    Safe,
    Recovering
};

// ---------------------------------------------------------------------------
// Breaker: pure-logic degradation state machine (design 12.2/12.3/12.4).
// No heap, no platform dependencies. Deploy one instance per AO: the
// coordinator routes each AO's events to that AO's breaker, which is what
// lets on_direct_timeout()/on_dispatcher_rtc_timeout() carry no AO argument
// while direct_allowed(ao) still revokes only the offending AO.
//
// Degradation chain: L1 -> L2 (backlog keeps growing) -> Safe (critical
// capacity or watchdog). L1/L2 -> Recovering when cooldown finished AND the
// watermark stays below 50%; Safe -> Recovering on an external safety
// restore; Recovering -> Normal only after consecutive healthy windows.
// Recovery requires (12.4): cooldown completed, sustained low watermark,
// no overflow/watchdog/timeout in the window, a successful controlled probe,
// and enough consecutive healthy windows. One successful dispatch is not
// enough.
// ---------------------------------------------------------------------------
template <typename Config = DefaultConfig>
class Breaker {
public:
    explicit Breaker(const Config& cfg) noexcept;

    // -- Event inputs (contract 4.4) --
    void on_direct_timeout() noexcept;          // consecutive 3 -> L1
    void on_dispatcher_rtc_timeout() noexcept;  // consecutive 3 -> L2 (quarantine slow AO)
    void on_watermark_violation() noexcept;     // persistent >80% -> L2
    void on_overflow() noexcept;                // -> L2
    void on_key_reserve_exhausted() noexcept;   // -> Safe
    void on_watchdog() noexcept;                // -> Safe
    void on_dispatch_cycle() noexcept;          // one dispatch cycle (cooldown counter)
    void on_probe_success() noexcept;
    void on_probe_failure() noexcept;           // Recovering -> L2
    void on_external_safe_restore() noexcept;   // Safe -> Recovering

    // -- Supplementary event inputs (not listed in 4.4, needed for the
    //    qualifying-call and low-watermark semantics of 12.3/12.4) --
    void on_rtc_ok() noexcept;                  // qualifying call: clears consecutive
                                                // timeout counters, does NOT skip cooldown
    void on_watermark(uint8_t percent) noexcept; // current overall watermark 0..100

    // -- Queries --
    BreakerLevel level() const noexcept;
    bool direct_allowed(TargetId ao) const noexcept;   // L1 revokes that AO's direct
    bool healthy_window_passed() const noexcept;

    // -- Supplementary graded-action queries (consumed by M4/policy) --
    bool drop_non_critical() const noexcept;   // L2/Safe: drop non-critical inputs
    bool safe_events_only() const noexcept;    // Safe: only safety events

    // -- Default thresholds --
    static constexpr uint8_t kDirectTimeoutThreshold = 3U;
    static constexpr uint8_t kRtcTimeoutThreshold = 3U;
    static constexpr uint8_t kWatermarkViolationPct = 80U;
    static constexpr uint8_t kLowWatermarkPct = 50U;
    static constexpr uint8_t kHighWatermarkPersist = 3U;  // consecutive >80% samples
    static constexpr uint8_t kLowWatermarkPersist = 3U;   // consecutive <50% samples
    static constexpr uint8_t kHealthyWindowsRequired = 3U;

private:
    void reset_metrics() noexcept;
    void enter_l1() noexcept;
    void enter_l2() noexcept;
    void enter_safe() noexcept;
    void enter_recovering() noexcept;

    const uint16_t cooldown_cycles_;
    BreakerLevel level_;
    uint16_t cooldown_remaining_;
    uint8_t direct_timeout_consec_;
    uint8_t rtc_timeout_consec_;
    uint8_t high_watermark_consec_;
    uint8_t low_watermark_consec_;
    uint8_t healthy_window_count_;
    bool probe_success_;
};

template <typename Config>
inline Breaker<Config>::Breaker(const Config& cfg) noexcept
    : cooldown_cycles_(static_cast<uint16_t>(cfg.kCooldownCycles)),
      level_(BreakerLevel::Normal),
      cooldown_remaining_(0),
      direct_timeout_consec_(0),
      rtc_timeout_consec_(0),
      high_watermark_consec_(0),
      low_watermark_consec_(0),
      healthy_window_count_(0),
      probe_success_(false) {}

template <typename Config>
inline void Breaker<Config>::reset_metrics() noexcept {
    direct_timeout_consec_ = 0;
    rtc_timeout_consec_ = 0;
    high_watermark_consec_ = 0;
    low_watermark_consec_ = 0;
    healthy_window_count_ = 0;
    probe_success_ = false;
}

template <typename Config>
inline void Breaker<Config>::enter_l1() noexcept {
    level_ = BreakerLevel::BrokenL1;
    cooldown_remaining_ = cooldown_cycles_;
    reset_metrics();
}

template <typename Config>
inline void Breaker<Config>::enter_l2() noexcept {
    level_ = BreakerLevel::BrokenL2;
    cooldown_remaining_ = cooldown_cycles_;
    reset_metrics();
}

template <typename Config>
inline void Breaker<Config>::enter_safe() noexcept {
    level_ = BreakerLevel::Safe;
    cooldown_remaining_ = cooldown_cycles_;
    reset_metrics();
}

template <typename Config>
inline void Breaker<Config>::enter_recovering() noexcept {
    level_ = BreakerLevel::Recovering;
    reset_metrics();
    // cooldown_remaining_ is left untouched: from L1/L2 it is already 0;
    // from Safe it still counts down before healthy windows may advance.
}

template <typename Config>
inline void Breaker<Config>::on_direct_timeout() noexcept {
    ++direct_timeout_consec_;
    switch (level_) {
        case BreakerLevel::Normal:
            if (direct_timeout_consec_ >= kDirectTimeoutThreshold) {
                enter_l1();
            }
            break;
        case BreakerLevel::BrokenL1:
        case BreakerLevel::BrokenL2:
        case BreakerLevel::Safe:
            break;
        case BreakerLevel::Recovering:
            enter_l2();  // re-violation inside the recovery window -> L2
            break;
    }
}

template <typename Config>
inline void Breaker<Config>::on_dispatcher_rtc_timeout() noexcept {
    ++rtc_timeout_consec_;
    switch (level_) {
        case BreakerLevel::Normal:
        case BreakerLevel::BrokenL1:
            if (rtc_timeout_consec_ >= kRtcTimeoutThreshold) {
                enter_l2();
            }
            break;
        case BreakerLevel::BrokenL2:
        case BreakerLevel::Safe:
            break;
        case BreakerLevel::Recovering:
            enter_l2();
            break;
    }
}

template <typename Config>
inline void Breaker<Config>::on_watermark_violation() noexcept {
    on_watermark(static_cast<uint8_t>(kWatermarkViolationPct + 1U));
}

template <typename Config>
inline void Breaker<Config>::on_watermark(uint8_t percent) noexcept {
    if (percent > kWatermarkViolationPct) {
        ++high_watermark_consec_;
        low_watermark_consec_ = 0;
    }
    else if (percent < kLowWatermarkPct) {
        ++low_watermark_consec_;
        high_watermark_consec_ = 0;
    }
    else {
        high_watermark_consec_ = 0;
        low_watermark_consec_ = 0;
    }

    switch (level_) {
        case BreakerLevel::Normal:
            if (high_watermark_consec_ >= kHighWatermarkPersist) {
                enter_l2();
            }
            break;
        case BreakerLevel::BrokenL1:
            if (high_watermark_consec_ >= kHighWatermarkPersist) {
                enter_l2();  // backlog keeps growing
            }
            else if ((0 == cooldown_remaining_) &&
                     (low_watermark_consec_ >= kLowWatermarkPersist)) {
                enter_recovering();
            }
            break;
        case BreakerLevel::BrokenL2:
            if ((0 == cooldown_remaining_) &&
                (low_watermark_consec_ >= kLowWatermarkPersist)) {
                enter_recovering();
            }
            break;
        case BreakerLevel::Safe:
            break;
        case BreakerLevel::Recovering:
            if (percent >= kLowWatermarkPct) {
                enter_l2();  // hysteresis: a single rise above 50% aborts recovery
            }
            break;
    }
}

template <typename Config>
inline void Breaker<Config>::on_overflow() noexcept {
    switch (level_) {
        case BreakerLevel::Normal:
        case BreakerLevel::BrokenL1:
        case BreakerLevel::Recovering:
            enter_l2();
            break;
        case BreakerLevel::BrokenL2:
        case BreakerLevel::Safe:
            break;
    }
}

template <typename Config>
inline void Breaker<Config>::on_key_reserve_exhausted() noexcept {
    if (BreakerLevel::Safe != level_) {
        enter_safe();
    }
}

template <typename Config>
inline void Breaker<Config>::on_watchdog() noexcept {
    if (BreakerLevel::Safe != level_) {
        enter_safe();
    }
}

template <typename Config>
inline void Breaker<Config>::on_dispatch_cycle() noexcept {
    if (cooldown_remaining_ > 0) {
        --cooldown_remaining_;
    }
    if (BreakerLevel::Recovering == level_) {
        if ((0 == cooldown_remaining_) && probe_success_) {
            ++healthy_window_count_;
            probe_success_ = false;
            if (healthy_window_count_ >= kHealthyWindowsRequired) {
                level_ = BreakerLevel::Normal;
                healthy_window_count_ = 0;
            }
        }
    }
}

template <typename Config>
inline void Breaker<Config>::on_probe_success() noexcept {
    if (BreakerLevel::Recovering == level_) {
        probe_success_ = true;
    }
}

template <typename Config>
inline void Breaker<Config>::on_probe_failure() noexcept {
    if (BreakerLevel::Recovering == level_) {
        enter_l2();
    }
}

template <typename Config>
inline void Breaker<Config>::on_external_safe_restore() noexcept {
    if (BreakerLevel::Safe == level_) {
        enter_recovering();
    }
}

template <typename Config>
inline void Breaker<Config>::on_rtc_ok() noexcept {
    // A qualifying call clears the consecutive-timeout counters but does NOT
    // touch cooldown_remaining_: the recovery window is never skipped.
    direct_timeout_consec_ = 0;
    rtc_timeout_consec_ = 0;
}

template <typename Config>
inline BreakerLevel Breaker<Config>::level() const noexcept {
    return level_;
}

template <typename Config>
inline bool Breaker<Config>::direct_allowed(TargetId ao) const noexcept {
    if (kInvalidTarget == ao) {
        return false;
    }
    // Per-AO deployment: the coordinator asks the owner AO's breaker. Direct
    // is granted again only once the breaker has fully recovered to Normal.
    return (BreakerLevel::Normal == level_);
}

template <typename Config>
inline bool Breaker<Config>::healthy_window_passed() const noexcept {
    if (BreakerLevel::Normal == level_) {
        return true;
    }
    if (BreakerLevel::Recovering == level_) {
        return (healthy_window_count_ >= kHealthyWindowsRequired);
    }
    return false;
}

template <typename Config>
inline bool Breaker<Config>::drop_non_critical() const noexcept {
    return (BreakerLevel::BrokenL2 == level_) || (BreakerLevel::Safe == level_);
}

template <typename Config>
inline bool Breaker<Config>::safe_events_only() const noexcept {
    return (BreakerLevel::Safe == level_);
}

// ---------------------------------------------------------------------------
// RejectReason: M1 admission gate C1..C7 rejection causes (design 9.2/9.3).
// ---------------------------------------------------------------------------
enum class RejectReason : uint8_t {
    kC1Eligibility,
    kC2Priority,
    kC3Nesting,
    kC4Watermark,
    kC5LeaseBusy,
    kC6Pending,
    kC7Context,
    kRejectCount
};

// ---------------------------------------------------------------------------
// AoCounters: fixed per-AO counters (design 12.1). Telemetry counters are
// written from producer threads and the Dispatcher thread concurrently, so they
// are relaxed atomics: no torn increment on SMP, no barrier cost on the
// single-core target (relaxed atomics compile to plain ops there).
// ---------------------------------------------------------------------------
struct AoCounters {
    std::atomic<uint64_t> direct_duration_ns{0};        // accumulated direct dispatch time
    std::atomic<uint64_t> dispatcher_duration_ns{0};    // accumulated Dispatcher RTC time
    std::atomic<uint32_t> direct_timeouts{0};
    std::atomic<uint32_t> rtc_timeouts{0};
    std::atomic<uint32_t> rejections[static_cast<size_t>(RejectReason::kRejectCount)]{};
    std::atomic<uint32_t> lease_contention{0};          // C5 execution lease contention
    std::atomic<uint16_t> pending{0};                   // current pending count
    std::atomic<uint16_t> pending_max{0};               // high-watermark of pending
};

// ---------------------------------------------------------------------------
// GlobalCounters: fixed system-wide counters (design 12.1).
// ---------------------------------------------------------------------------
struct GlobalCounters {
    std::atomic<uint8_t>  watermark_pct[3]{};
    std::atomic<uint32_t> high_water_count[3]{};
    std::atomic<uint32_t> full_count[3]{};
    std::atomic<uint32_t> disposition_filter{0};
    std::atomic<uint32_t> disposition_merge{0};
    std::atomic<uint32_t> disposition_rate_limit{0};
    std::atomic<uint32_t> disposition_overload{0};
    std::atomic<uint32_t> overflow{0};
    std::atomic<uint32_t> watchdog_heartbeats{0};
    std::atomic<uint32_t> platform_faults{0};
    std::atomic<uint16_t> pending_max{0};
};

// ---------------------------------------------------------------------------
// Monitor: fixed counters only. The hot path writes counters, never formats
// strings and never blocks. SMP may keep per-CPU instances and fold them at a
// higher layer; this module provides the per-instance accounting.
// ---------------------------------------------------------------------------
template <typename Config = DefaultConfig>
class Monitor {
public:
    static constexpr uint8_t kMaxAo = Config::kMaxAo;
    static constexpr uint8_t kHighWatermarkPct = 80U;
    static constexpr uint8_t kFullWatermarkPct = 100U;

    Monitor() noexcept = default;

    // -- Per-AO accounting --
    void add_direct_duration(TargetId ao, uint64_t ns) noexcept;
    void add_dispatcher_duration(TargetId ao, uint64_t ns) noexcept;
    void record_direct_timeout(TargetId ao) noexcept;
    void record_rtc_timeout(TargetId ao) noexcept;
    void record_rejection(TargetId ao, RejectReason reason) noexcept;
    void record_lease_contention(TargetId ao) noexcept;
    void record_pending(TargetId ao, uint16_t pending) noexcept;

    // -- Partition watermarks --
    void sample_watermark(PriorityClass p, uint8_t pct) noexcept;
    void record_overflow() noexcept;

    // -- M4 dispositions --
    void record_disposition(SubmitDisposition disposition) noexcept;

    // -- Watchdog / platform --
    void heartbeat() noexcept;
    void record_platform_fault() noexcept;

    // -- Queries --
    const AoCounters& ao(TargetId ao) const noexcept;
    const GlobalCounters& global() const noexcept;

private:
    AoCounters* slot(TargetId ao) noexcept;
    const AoCounters* slot(TargetId ao) const noexcept;
    static size_t partition_index(PriorityClass p) noexcept;

    AoCounters ao_[static_cast<size_t>(kMaxAo) + 1U];  // 1-based TargetId
    GlobalCounters global_;
};

template <typename Config>
inline AoCounters* Monitor<Config>::slot(TargetId ao) noexcept {
    if ((kInvalidTarget == ao) || (ao > kMaxAo)) {
        return nullptr;
    }
    return &ao_[static_cast<size_t>(ao)];
}

template <typename Config>
inline const AoCounters* Monitor<Config>::slot(TargetId ao) const noexcept {
    if ((kInvalidTarget == ao) || (ao > kMaxAo)) {
        return nullptr;
    }
    return &ao_[static_cast<size_t>(ao)];
}

template <typename Config>
inline size_t Monitor<Config>::partition_index(PriorityClass p) noexcept {
    if (p > PriorityClass::Low) {
        return 0U;
    }
    return static_cast<size_t>(p);
}

template <typename Config>
inline void Monitor<Config>::add_direct_duration(TargetId ao, uint64_t ns) noexcept {
    AoCounters* s = slot(ao);
    if (nullptr == s) {
        return;
    }
    s->direct_duration_ns.fetch_add(ns, std::memory_order_relaxed);
}

template <typename Config>
inline void Monitor<Config>::add_dispatcher_duration(TargetId ao, uint64_t ns) noexcept {
    AoCounters* s = slot(ao);
    if (nullptr == s) {
        return;
    }
    s->dispatcher_duration_ns.fetch_add(ns, std::memory_order_relaxed);
}

template <typename Config>
inline void Monitor<Config>::record_direct_timeout(TargetId ao) noexcept {
    AoCounters* s = slot(ao);
    if (nullptr == s) {
        return;
    }
    s->direct_timeouts.fetch_add(1U, std::memory_order_relaxed);
}

template <typename Config>
inline void Monitor<Config>::record_rtc_timeout(TargetId ao) noexcept {
    AoCounters* s = slot(ao);
    if (nullptr == s) {
        return;
    }
    s->rtc_timeouts.fetch_add(1U, std::memory_order_relaxed);
}

template <typename Config>
inline void Monitor<Config>::record_rejection(TargetId ao, RejectReason reason) noexcept {
    if (reason >= RejectReason::kRejectCount) {
        return;
    }
    AoCounters* s = slot(ao);
    if (nullptr == s) {
        return;
    }
    s->rejections[static_cast<size_t>(reason)].fetch_add(
        1U, std::memory_order_relaxed);
}

template <typename Config>
inline void Monitor<Config>::record_lease_contention(TargetId ao) noexcept {
    AoCounters* s = slot(ao);
    if (nullptr == s) {
        return;
    }
    s->lease_contention.fetch_add(1U, std::memory_order_relaxed);
}

template <typename Config>
inline void Monitor<Config>::record_pending(TargetId ao, uint16_t pending) noexcept {
    AoCounters* s = slot(ao);
    if (nullptr == s) {
        return;
    }
    s->pending.store(pending, std::memory_order_relaxed);
    if (pending > s->pending_max.load(std::memory_order_relaxed)) {
        s->pending_max.store(pending, std::memory_order_relaxed);
    }
    if (pending > global_.pending_max.load(std::memory_order_relaxed)) {
        global_.pending_max.store(pending, std::memory_order_relaxed);
    }
}

template <typename Config>
inline void Monitor<Config>::sample_watermark(PriorityClass p, uint8_t pct) noexcept {
    const size_t idx = partition_index(p);
    global_.watermark_pct[idx].store(pct, std::memory_order_relaxed);
    if (pct >= kHighWatermarkPct) {
        global_.high_water_count[idx].fetch_add(1U, std::memory_order_relaxed);
    }
    if (pct >= kFullWatermarkPct) {
        global_.full_count[idx].fetch_add(1U, std::memory_order_relaxed);
    }
}

template <typename Config>
inline void Monitor<Config>::record_overflow() noexcept {
    global_.overflow.fetch_add(1U, std::memory_order_relaxed);
}

template <typename Config>
inline void Monitor<Config>::record_disposition(SubmitDisposition disposition) noexcept {
    switch (disposition) {
        case SubmitDisposition::Merged:
            global_.disposition_merge.fetch_add(1U, std::memory_order_relaxed);
            break;
        case SubmitDisposition::DroppedPolicy:
            global_.disposition_filter.fetch_add(1U, std::memory_order_relaxed);
            break;
        case SubmitDisposition::DroppedRateLimit:
            global_.disposition_rate_limit.fetch_add(1U, std::memory_order_relaxed);
            break;
        case SubmitDisposition::DroppedOverload:
            global_.disposition_overload.fetch_add(1U, std::memory_order_relaxed);
            break;
        case SubmitDisposition::Direct:
        case SubmitDisposition::Queued:
        case SubmitDisposition::RejectedFull:
        case SubmitDisposition::RejectedState:
            break;
        default:
            break;
    }
}

template <typename Config>
inline void Monitor<Config>::heartbeat() noexcept {
    global_.watchdog_heartbeats.fetch_add(1U, std::memory_order_relaxed);
}

template <typename Config>
inline void Monitor<Config>::record_platform_fault() noexcept {
    global_.platform_faults.fetch_add(1U, std::memory_order_relaxed);
}

template <typename Config>
inline const AoCounters& Monitor<Config>::ao(TargetId ao) const noexcept {
    const AoCounters* s = slot(ao);
    if (nullptr == s) {
        return ao_[0U];  // unused slot, always zero
    }
    return *s;
}

template <typename Config>
inline const GlobalCounters& Monitor<Config>::global() const noexcept {
    return global_;
}

}  // namespace coact
