// coact M4 strategy engine: admission policy (filter / merge / rate-limit)
// expressed as a const function table plus a fixed CAS cell for merging.
// See design 11 and implementation contract 4.5.
//
// The merge slot state machine (Empty / Published / Merging / Consuming with
// std::atomic transitions and a single owning Event*) follows QP/C++ QActive
// event ownership: the cell holds one already-posted reference-counted Event
// (coact Event), the producer overwrites its payload under Merging, the
// dispatcher takes the owning handle under Consuming and event_gc()s it once.
// CAS failure never blocks: the new event falls through to normal staging.
// Project license: MIT.
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>

#include "coact/config.hpp"
#include "coact/event.hpp"

namespace coact {

// ---------------------------------------------------------------------------
// Admission verdict produced by PolicyOps::evaluate.
// ---------------------------------------------------------------------------
struct PolicyResult {
    bool accept;      // allow the event into the pipeline
    bool try_merge;   // candidate for the owning (TargetId, signal) MergeCell
    uint16_t reason;  // stable reason code, see PolicyReason
};

// M4 admission reason codes (taken from SubmitDisposition vocabulary but kept
// local to the policy module so consumers map them as needed).
enum PolicyReason : uint16_t {
    kReasonOk = 0U,         // accepted
    kReasonFiltered = 1U,   // blacklist / predicate rejected
    kReasonRateLimit = 2U,  // token bucket exhausted
    kReasonCriticalBlocked = 3U  // guarded drop of a critical event denied
};

// Fixed function table for the strategy engine. Unlike QPs, a single const
// table is bound with a caller-owned context; no closures, no dynamic tables.
struct PolicyOps {
    PolicyResult (*evaluate)(void* context, TargetId target,
                             const Event& event, const EventQos& qos,
                             uint64_t now);
    bool (*merge)(void* context, Event& queued, const Event& incoming);
};

// ---------------------------------------------------------------------------
// TokenBucketRateLimiter: fixed-capacity, configurable-per-rule limiter. On
// every evaluation it either consumes one token (accept) or reports
// exhausted (reject). The bucket refills by rate; now advances in monotonic
// ticks. Calling with now in the past does not refill.
// ---------------------------------------------------------------------------
struct RateLimitRule {
    uint64_t capacity;   // burst ceiling
    uint64_t rate;       // tokens per refill_interval (>= 1)
    uint64_t refill_interval;  // ticks between refills (>= 1)
    bool enabled;        // when false a rule always accepts
};

class TokenBucketRateLimiter {
public:
    void init(const RateLimitRule& rule, uint64_t now) noexcept
    {
        rule_ = rule;
        tokens_ = rule.capacity;
        last_ = now;
    }

    // Consume one token if available after refilling up to `now`.
    // Returns true (admitted) and deducts a token, or false (limited).
    bool acquire(uint64_t now) noexcept
    {
        if (!rule_.enabled) {
            return true;
        }
        refill(now);
        if (tokens_ > 0U) {
            --tokens_;
            return true;
        }
        return false;
    }

    uint64_t tokens() const noexcept { return tokens_; }

private:
    void refill(uint64_t now) noexcept
    {
        if (rule_.refill_interval == 0U) {
            return;
        }
        if (now <= last_) {
            return;
        }
        const uint64_t elapsed = now - last_;
        const uint64_t added = elapsed / rule_.refill_interval;
        if (added == 0U) {
            return;
        }
        last_ = now;  // consume whole elapsed window, tokens prorated by rate
        const uint64_t gain = added * rule_.rate;
        const uint64_t room = rule_.capacity - tokens_;  // no underflow: cap>=tokens
        if (gain >= room) {
            tokens_ = rule_.capacity;  // clamp to burst ceiling
        }
        else {
            tokens_ += gain;
        }
    }

    RateLimitRule rule_{};
    uint64_t tokens_ = 0U;
    uint64_t last_ = 0U;
};

// ---------------------------------------------------------------------------
// MergeCell: single owning slot keyed by (TargetId, signal). Owns one already
// posted, reference-counted Event*. Under the Merging state a producer may
// repurpose the old payload from `incoming` (overwrite semantics decided by
// the merge fn); under Consuming the dispatcher takes the handle and must
// event_gc() it exactly once before the cell returns to Empty.
//
// State transitions (std::atomic, all compare_exchange on the strong path):
//   Empty     --(try_publish claim)--> Publishing --(pointer release)--> Published
//   Published --(try_acquire_merge)--> Merging --(release_merge)--> Published
//   Published --(take_owning)--> Consuming --(release_empty)--> Empty
// Failed CAS returns false and never spins.
// ---------------------------------------------------------------------------
enum class MergeCellState : uint8_t {
    Empty,
    Publishing,
    Published,
    Merging,
    Consuming
};

class MergeCell {
public:
    void init(TargetId target, uint16_t signal) noexcept
    {
        target_ = target;
        signal_ = signal;
        event_ = nullptr;
        state_.store(MergeCellState::Empty, std::memory_order_relaxed);
    }

    TargetId target() const noexcept { return target_; }
    uint16_t signal() const noexcept { return signal_; }
    MergeCellState state() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }

    // Producer: place a new owning handle. Succeeds only from Empty.
    bool try_publish(Event* e) noexcept
    {
        if (e == nullptr) {
            return false;
        }
        MergeCellState expected = MergeCellState::Empty;
        if (!state_.compare_exchange_strong(expected, MergeCellState::Publishing,
                                            std::memory_order_acq_rel)) {
            return false;
        }
        event_ = e;
        state_.store(MergeCellState::Published, std::memory_order_release);
        return true;
    }

    // Producer: claim the cell for payload overwrite. On success the caller
    // may mutate *queued via the merge fn, must then call release_merge().
    bool try_acquire_merge(Event*& queued) noexcept
    {
        MergeCellState expected = MergeCellState::Published;
        if (!state_.compare_exchange_strong(expected, MergeCellState::Merging,
                                            std::memory_order_acq_rel)) {
            return false;
        }
        queued = event_;
        return true;
    }

    // Producer: return from Merging to Published with the repurposed payload.
    void release_merge() noexcept
    {
        MergeCellState expected = MergeCellState::Merging;
        state_.compare_exchange_strong(expected, MergeCellState::Published,
                                       std::memory_order_release);
    }

    // Dispatcher: take the owning handle for consumption. On success the
    // caller owns *queued; call release_empty() after event_gc() once.
    bool take_owning(Event*& out) noexcept
    {
        MergeCellState expected = MergeCellState::Published;
        if (!state_.compare_exchange_strong(expected, MergeCellState::Consuming,
                                            std::memory_order_acq_rel)) {
            return false;
        }
        out = event_;
        return true;
    }

    // Dispatcher: the consumer finished (event_gc done); cell returns Empty.
    void release_empty() noexcept
    {
        MergeCellState expected = MergeCellState::Consuming;
        event_ = nullptr;
        state_.compare_exchange_strong(expected, MergeCellState::Empty,
                                       std::memory_order_release);
    }

private:
    TargetId target_ = kInvalidTarget;
    uint16_t signal_ = 0U;
    std::atomic<MergeCellState> state_{MergeCellState::Empty};
    Event* event_ = nullptr;
};

}  // namespace coact
