// coact monitor and circuit breaker (M6).
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "coact/assert.hpp"
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
    Breaker() noexcept : Breaker(Config{}) {}
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
    struct State {
        BreakerLevel level;
        uint16_t cooldown_remaining;
        uint8_t direct_timeout_consec;
        uint8_t rtc_timeout_consec;
        uint8_t high_watermark_consec;
        uint8_t low_watermark_consec;
        uint8_t healthy_window_count;
        bool probe_success;
    };

    static constexpr uint32_t kLevelShift = 0U;
    static constexpr uint32_t kCooldownShift = 3U;
    static constexpr uint32_t kDirectTimeoutShift = 19U;
    static constexpr uint32_t kRtcTimeoutShift = 21U;
    static constexpr uint32_t kHighWatermarkShift = 23U;
    static constexpr uint32_t kLowWatermarkShift = 25U;
    static constexpr uint32_t kHealthyWindowShift = 27U;
    static constexpr uint32_t kProbeSuccessShift = 29U;
    static constexpr uint32_t kLevelMask = 0x7U;
    static constexpr uint32_t kCooldownMask = 0xffffU;
    static constexpr uint32_t kCounterMask = 0x3U;

    static uint32_t pack(const State& state) noexcept;
    static State unpack(uint32_t packed) noexcept;
    static void increment(uint8_t& counter) noexcept;

    template <typename Transition>
    void update(const Transition& transition) noexcept;

    void reset_metrics(State& state) const noexcept;
    void enter_l1(State& state) const noexcept;
    void enter_l2(State& state) const noexcept;
    void enter_safe(State& state) const noexcept;
    void enter_recovering(State& state) const noexcept;

    const uint16_t cooldown_cycles_;
    // The packed word is the complete mutable state. Relaxed CAS linearizes
    // transitions; no Breaker event publishes or consumes external data.
    std::atomic<uint32_t> state_;

    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "coact: Breaker requires lock-free 32-bit atomics");
    static_assert(static_cast<uint64_t>(Config::kCooldownCycles) <= kCooldownMask,
                  "coact: kCooldownCycles must fit in 16 bits");
};

template <typename Config>
inline Breaker<Config>::Breaker(const Config& cfg) noexcept
    : cooldown_cycles_(static_cast<uint16_t>(cfg.kCooldownCycles)),
      state_(pack({BreakerLevel::Normal, 0U, 0U, 0U, 0U, 0U, 0U, false})) {}

template <typename Config>
inline uint32_t Breaker<Config>::pack(const State& state) noexcept {
    return (static_cast<uint32_t>(state.level) << kLevelShift) |
           (static_cast<uint32_t>(state.cooldown_remaining) << kCooldownShift) |
           (static_cast<uint32_t>(state.direct_timeout_consec) << kDirectTimeoutShift) |
           (static_cast<uint32_t>(state.rtc_timeout_consec) << kRtcTimeoutShift) |
           (static_cast<uint32_t>(state.high_watermark_consec) << kHighWatermarkShift) |
           (static_cast<uint32_t>(state.low_watermark_consec) << kLowWatermarkShift) |
           (static_cast<uint32_t>(state.healthy_window_count) << kHealthyWindowShift) |
           (static_cast<uint32_t>(state.probe_success) << kProbeSuccessShift);
}

template <typename Config>
inline typename Breaker<Config>::State Breaker<Config>::unpack(uint32_t packed) noexcept {
    return {static_cast<BreakerLevel>((packed >> kLevelShift) & kLevelMask),
            static_cast<uint16_t>((packed >> kCooldownShift) & kCooldownMask),
            static_cast<uint8_t>((packed >> kDirectTimeoutShift) & kCounterMask),
            static_cast<uint8_t>((packed >> kRtcTimeoutShift) & kCounterMask),
            static_cast<uint8_t>((packed >> kHighWatermarkShift) & kCounterMask),
            static_cast<uint8_t>((packed >> kLowWatermarkShift) & kCounterMask),
            static_cast<uint8_t>((packed >> kHealthyWindowShift) & kCounterMask),
            0U != ((packed >> kProbeSuccessShift) & 0x1U)};
}

template <typename Config>
inline void Breaker<Config>::increment(uint8_t& counter) noexcept {
    if (counter < kCounterMask) {
        ++counter;
    }
}

template <typename Config>
template <typename Transition>
inline void Breaker<Config>::update(const Transition& transition) noexcept {
    uint32_t observed = state_.load(std::memory_order_relaxed);
    bool complete = false;
    while (!complete) {
        State next = unpack(observed);
        transition(next);
        const uint32_t desired = pack(next);
        complete = (desired == observed) ||
                   state_.compare_exchange_weak(observed, desired, std::memory_order_relaxed,
                                                std::memory_order_relaxed);
    }
}

template <typename Config>
inline void Breaker<Config>::reset_metrics(State& state) const noexcept {
    state.direct_timeout_consec = 0U;
    state.rtc_timeout_consec = 0U;
    state.high_watermark_consec = 0U;
    state.low_watermark_consec = 0U;
    state.healthy_window_count = 0U;
    state.probe_success = false;
}

template <typename Config>
inline void Breaker<Config>::enter_l1(State& state) const noexcept {
    state.level = BreakerLevel::BrokenL1;
    state.cooldown_remaining = cooldown_cycles_;
    reset_metrics(state);
}

template <typename Config>
inline void Breaker<Config>::enter_l2(State& state) const noexcept {
    state.level = BreakerLevel::BrokenL2;
    state.cooldown_remaining = cooldown_cycles_;
    reset_metrics(state);
}

template <typename Config>
inline void Breaker<Config>::enter_safe(State& state) const noexcept {
    state.level = BreakerLevel::Safe;
    state.cooldown_remaining = cooldown_cycles_;
    reset_metrics(state);
}

template <typename Config>
inline void Breaker<Config>::enter_recovering(State& state) const noexcept {
    state.level = BreakerLevel::Recovering;
    reset_metrics(state);
    // cooldown_remaining is left untouched: from L1/L2 it is already 0;
    // from Safe it still counts down before healthy windows may advance.
}

template <typename Config>
inline void Breaker<Config>::on_direct_timeout() noexcept {
    update([this](State& state) {
        increment(state.direct_timeout_consec);
        switch (state.level) {
            case BreakerLevel::Normal:
                if (state.direct_timeout_consec >= kDirectTimeoutThreshold) {
                    enter_l1(state);
                }
                break;
            case BreakerLevel::BrokenL1:
            case BreakerLevel::BrokenL2:
            case BreakerLevel::Safe:
                break;
            case BreakerLevel::Recovering:
                enter_l2(state);
                break;
            default:
                break;
        }
    });
}

template <typename Config>
inline void Breaker<Config>::on_dispatcher_rtc_timeout() noexcept {
    update([this](State& state) {
        increment(state.rtc_timeout_consec);
        switch (state.level) {
            case BreakerLevel::Normal:
            case BreakerLevel::BrokenL1:
                if (state.rtc_timeout_consec >= kRtcTimeoutThreshold) {
                    enter_l2(state);
                }
                break;
            case BreakerLevel::BrokenL2:
            case BreakerLevel::Safe:
                break;
            case BreakerLevel::Recovering:
                enter_l2(state);
                break;
            default:
                break;
        }
    });
}

template <typename Config>
inline void Breaker<Config>::on_watermark_violation() noexcept {
    on_watermark(static_cast<uint8_t>(kWatermarkViolationPct + 1U));
}

template <typename Config>
inline void Breaker<Config>::on_watermark(uint8_t percent) noexcept {
    update([this, percent](State& state) {
        if (percent > kWatermarkViolationPct) {
            increment(state.high_watermark_consec);
            state.low_watermark_consec = 0U;
        } else if (percent < kLowWatermarkPct) {
            increment(state.low_watermark_consec);
            state.high_watermark_consec = 0U;
        } else {
            state.high_watermark_consec = 0U;
            state.low_watermark_consec = 0U;
        }

        switch (state.level) {
            case BreakerLevel::Normal:
                if (state.high_watermark_consec >= kHighWatermarkPersist) {
                    enter_l2(state);
                }
                break;
            case BreakerLevel::BrokenL1:
                if (state.high_watermark_consec >= kHighWatermarkPersist) {
                    enter_l2(state);
                } else if ((0U == state.cooldown_remaining) &&
                           (state.low_watermark_consec >= kLowWatermarkPersist)) {
                    enter_recovering(state);
                }
                break;
            case BreakerLevel::BrokenL2:
                if ((0U == state.cooldown_remaining) &&
                    (state.low_watermark_consec >= kLowWatermarkPersist)) {
                    enter_recovering(state);
                }
                break;
            case BreakerLevel::Safe:
                break;
            case BreakerLevel::Recovering:
                if (percent >= kLowWatermarkPct) {
                    enter_l2(state);
                }
                break;
            default:
                break;
        }
    });
}

template <typename Config>
inline void Breaker<Config>::on_overflow() noexcept {
    update([this](State& state) {
        switch (state.level) {
            case BreakerLevel::Normal:
            case BreakerLevel::BrokenL1:
            case BreakerLevel::Recovering:
                enter_l2(state);
                break;
            case BreakerLevel::BrokenL2:
            case BreakerLevel::Safe:
                break;
            default:
                break;
        }
    });
}

template <typename Config>
inline void Breaker<Config>::on_key_reserve_exhausted() noexcept {
    update([this](State& state) {
        if (BreakerLevel::Safe != state.level) {
            enter_safe(state);
        }
    });
}

template <typename Config>
inline void Breaker<Config>::on_watchdog() noexcept {
    on_key_reserve_exhausted();
}

template <typename Config>
inline void Breaker<Config>::on_dispatch_cycle() noexcept {
    update([](State& state) {
        if (state.cooldown_remaining > 0U) {
            --state.cooldown_remaining;
        }
        if ((BreakerLevel::Recovering == state.level) && (0U == state.cooldown_remaining) &&
            state.probe_success) {
            increment(state.healthy_window_count);
            state.probe_success = false;
            if (state.healthy_window_count >= kHealthyWindowsRequired) {
                state.level = BreakerLevel::Normal;
                state.healthy_window_count = 0U;
            }
        }
    });
}

template <typename Config>
inline void Breaker<Config>::on_probe_success() noexcept {
    update([](State& state) {
        if (BreakerLevel::Recovering == state.level) {
            state.probe_success = true;
        }
    });
}

template <typename Config>
inline void Breaker<Config>::on_probe_failure() noexcept {
    update([this](State& state) {
        if (BreakerLevel::Recovering == state.level) {
            enter_l2(state);
        }
    });
}

template <typename Config>
inline void Breaker<Config>::on_external_safe_restore() noexcept {
    update([this](State& state) {
        if (BreakerLevel::Safe == state.level) {
            enter_recovering(state);
        }
    });
}

template <typename Config>
inline void Breaker<Config>::on_rtc_ok() noexcept {
    // A qualifying call clears the consecutive-timeout counters but does NOT
    // touch cooldown_remaining_: the recovery window is never skipped.
    update([](State& state) {
        state.direct_timeout_consec = 0U;
        state.rtc_timeout_consec = 0U;
    });
}

template <typename Config>
inline BreakerLevel Breaker<Config>::level() const noexcept {
    return unpack(state_.load(std::memory_order_relaxed)).level;
}

template <typename Config>
inline bool Breaker<Config>::direct_allowed(TargetId ao) const noexcept {
    if (kInvalidTarget == ao) {
        return false;
    }
    // Per-AO deployment: the coordinator asks the owner AO's breaker. Direct
    // is granted again only once the breaker has fully recovered to Normal.
    return (BreakerLevel::Normal == level());
}

template <typename Config>
inline bool Breaker<Config>::healthy_window_passed() const noexcept {
    const State state = unpack(state_.load(std::memory_order_relaxed));
    if (BreakerLevel::Normal == state.level) {
        return true;
    }
    if (BreakerLevel::Recovering == state.level) {
        return (state.healthy_window_count >= kHealthyWindowsRequired);
    }
    return false;
}

template <typename Config>
inline bool Breaker<Config>::drop_non_critical() const noexcept {
    const BreakerLevel current = level();
    return (BreakerLevel::BrokenL2 == current) || (BreakerLevel::Safe == current);
}

template <typename Config>
inline bool Breaker<Config>::safe_events_only() const noexcept {
    return (BreakerLevel::Safe == level());
}

template <typename Config>
class BreakerBank;

namespace detail {

template <typename Config>
Breaker<Config>& target_breaker(BreakerBank<Config>& breakers,
                                TargetId target) noexcept;

}  // namespace detail

// ---------------------------------------------------------------------------
// Fixed-capacity per-AO breaker bank. Target-scoped inputs name their AO;
// genuinely system-wide inputs use an explicit broadcast_* entry.
// ---------------------------------------------------------------------------
template <typename Config = DefaultConfig>
class BreakerBank {
public:
    static constexpr uint8_t kCapacity = Config::kMaxAo;

    explicit BreakerBank(const Config&) noexcept : breakers_{} {}

    void on_direct_timeout(TargetId target) noexcept
    {
        if (valid(target)) {
            breaker(target).on_direct_timeout();
        }
    }

    void on_dispatcher_rtc_timeout(TargetId target) noexcept
    {
        if (valid(target)) {
            breaker(target).on_dispatcher_rtc_timeout();
        }
    }

    void on_overflow(TargetId target) noexcept
    {
        if (valid(target)) {
            breaker(target).on_overflow();
        }
    }

    void on_dispatch_cycle(TargetId target) noexcept
    {
        if (valid(target)) {
            breaker(target).on_dispatch_cycle();
        }
    }

    void on_probe_success(TargetId target) noexcept
    {
        if (valid(target)) {
            breaker(target).on_probe_success();
        }
    }

    void on_probe_failure(TargetId target) noexcept
    {
        if (valid(target)) {
            breaker(target).on_probe_failure();
        }
    }

    void on_rtc_ok(TargetId target) noexcept
    {
        if (valid(target)) {
            breaker(target).on_rtc_ok();
        }
    }

    BreakerLevel level(TargetId target) const noexcept
    {
        return valid(target) ? breaker(target).level() : BreakerLevel::Safe;
    }

    bool direct_allowed(TargetId target) const noexcept
    {
        return valid(target) && breaker(target).direct_allowed(target);
    }

    bool drop_non_critical(TargetId target) const noexcept
    {
        return !valid(target) || breaker(target).drop_non_critical();
    }

    bool safe_events_only(TargetId target) const noexcept
    {
        return !valid(target) || breaker(target).safe_events_only();
    }

    void broadcast_watermark_violation() noexcept
    {
        for_each([](Breaker<Config>& item) { item.on_watermark_violation(); });
    }

    void broadcast_watermark(uint8_t percent) noexcept
    {
        for_each([percent](Breaker<Config>& item) { item.on_watermark(percent); });
    }

    void broadcast_key_reserve_exhausted() noexcept
    {
        for_each([](Breaker<Config>& item) { item.on_key_reserve_exhausted(); });
    }

    void broadcast_watchdog() noexcept
    {
        for_each([](Breaker<Config>& item) { item.on_watchdog(); });
    }

    void broadcast_dispatch_cycle() noexcept
    {
        for_each([](Breaker<Config>& item) { item.on_dispatch_cycle(); });
    }

    void broadcast_external_safe_restore() noexcept
    {
        for_each([](Breaker<Config>& item) { item.on_external_safe_restore(); });
    }

private:
    friend Breaker<Config>& detail::target_breaker<Config>(
        BreakerBank<Config>& breakers, TargetId target) noexcept;

    static constexpr bool valid(TargetId target) noexcept
    {
        return target != kInvalidTarget && target.raw() <= kCapacity;
    }

    Breaker<Config>& breaker(TargetId target) noexcept
    {
        return breakers_[index(target)];
    }

    const Breaker<Config>& breaker(TargetId target) const noexcept
    {
        return breakers_[index(target)];
    }

    static size_t index(TargetId target) noexcept
    {
        COACT_ASSERT(valid(target));
        return static_cast<size_t>(target.raw() - 1U);
    }

    template <typename Operation>
    void for_each(const Operation& operation) noexcept
    {
        for (Breaker<Config>& item : breakers_) {
            operation(item);
        }
    }

    std::array<Breaker<Config>, kCapacity> breakers_;

    static_assert(kCapacity > 0U, "coact: BreakerBank requires at least one AO");
};

namespace detail {

template <typename Config>
inline Breaker<Config>& target_breaker(Breaker<Config>& breaker,
                                       TargetId) noexcept
{
    return breaker;
}

template <typename Config>
inline Breaker<Config>& target_breaker(BreakerBank<Config>& breakers,
                                       TargetId target) noexcept
{
    return breakers.breaker(target);
}

template <typename Config>
inline void breaker_idle_cycle(Breaker<Config>& breaker) noexcept
{
    breaker.on_dispatch_cycle();
}

template <typename Config>
inline void breaker_idle_cycle(BreakerBank<Config>& breakers) noexcept
{
    breakers.broadcast_dispatch_cycle();
}

}  // namespace detail

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
    if ((kInvalidTarget == ao) || (ao.raw() > kMaxAo)) {
        return nullptr;
    }
    return &ao_[static_cast<size_t>(ao.raw())];
}

template <typename Config>
inline const AoCounters* Monitor<Config>::slot(TargetId ao) const noexcept {
    if ((kInvalidTarget == ao) || (ao.raw() > kMaxAo)) {
        return nullptr;
    }
    return &ao_[static_cast<size_t>(ao.raw())];
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
