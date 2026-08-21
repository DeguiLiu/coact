// coact three-partition staging and batch selection (M3/M5).
// SPDX-License-Identifier: MIT
//
// Staging buffers posted events into three fixed partitions (High 32 / Normal
// 64 / Low 128 by default) whose capacities are distinct types, so a single
// partition is never repurposed at a wrong capacity. Batch ordering is
// priority-first (High -> Normal -> Low) with the single explicit exception
// that a Low event which has aged past Config::kLowMaxWaitMs is force-served
// ahead of the priority order. Both ideas follow design 10 (M3/M5: staged
// partitioning, batching, watermark feedback and low-priority aging) from
// design_coact_zh.md; the reference-counted Ownership of each slot is left to
// the coordinator/dispatcher - staging only stores the reference transferred
// by the coordinator; it never changes reference counts nor allocates.
//
// Queue backend is a template-template parameter (BoundedMpscQueue for SMP or
// SingleCoreCriticalRing for a single core); it is instantiated once per
// partition at the partition's own capacity. No heap allocation.
#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

#include "coact/config.hpp"
#include "coact/event.hpp"
#include "coact/queue.hpp"

namespace coact {

// ---------------------------------------------------------------------------
// Fixed partition keys. An AO maps to exactly one partition by its
// PriorityClass; see design 3.3.
// ---------------------------------------------------------------------------
enum class Partition : uint8_t {
    High = 0U,
    Normal = 1U,
    Low = 2U
};

// Map a PriorityClass to its partition. The AO's class is the sole authority
// for partition selection; callers route through this helper.
inline Partition partition_from_class(PriorityClass cls) noexcept
{
    switch (cls) {
    case PriorityClass::High:
        return Partition::High;
    case PriorityClass::Normal:
        return Partition::Normal;
    case PriorityClass::Low:
        return Partition::Low;
    default:
        return Partition::Normal;   // unreachable: explicit default
    }
}

// ---------------------------------------------------------------------------
// One buffered event. The wrapped Event* carries the allocation reference
// transferred by the producer; the consumer (dispatcher) is responsible for
// event_gc after it finishes dispatch. enqueue_ns drives the Low aging
// deadline for batch ordering (unsigned wrap-safe comparison).
// ---------------------------------------------------------------------------
struct StagingSlot {
    TargetId target;
    Event* event;           // transferred owned reference; dispatcher event_gc
    uint64_t enqueue_ns;    // publish time, used for Low aging
};

// ---------------------------------------------------------------------------
// Pure batch-order selection (no queue storage, fully testable). Inputs are
// the per-partition published-head counts plus the aging signal; the output is
// the next partition a dequeue should serve.
//
// Ordering (design 10.3/10.4):
//   1. If Low is non-empty and has aged past LowMaxWaitMs, force-serve Low.
//      Aging is the single explicit exception to strict priority order.
//   2. Otherwise serve High, then Normal, then Low, preserving per-partition
//      FIFO.
//   3. If batch_used already reached batch_max, return false (batch full).
// ---------------------------------------------------------------------------
class BatchSelector {
public:
    BatchSelector() noexcept = default;

    bool select(Partition& out,
                uint16_t high,
                uint16_t normal,
                uint16_t low,
                bool low_aged,
                uint16_t batch_used,
                uint16_t batch_max) const noexcept
    {
        if (batch_used >= batch_max) {
            return false;   // this batch is full
        }
        if (1U <= low && low_aged) {
            out = Partition::Low;   // aging exception preempts priority order
            return true;
        }
        if (1U <= high) {
            out = Partition::High;
            return true;
        }
        if (1U <= normal) {
            out = Partition::Normal;
            return true;
        }
        if (1U <= low) {
            out = Partition::Low;
            return true;
        }
        return false;   // every partition empty
    }
};

// ---------------------------------------------------------------------------
// Unified three-partition staging view bound at compile time to a queue
// backend. Each partition is a distinct QueueBackend type at its own capacity
// (contract 4.7: never reuse one capacity for another). The injected
// CriticalSection is used only by the SingleCoreCriticalRing backend; the
// Mpsc backend ignores it (it is default-constructible).
//
// Lifetime note: enqueue stores a transferred Event* reference and never
// changes its count; dequeue hands ownership of that reference to the
// consumer. The dispatcher must event_gc each dequeued slot.
// ---------------------------------------------------------------------------
template <typename Config,
          template <typename, uint16_t> class QueueBackend>
class Staging {
public:
    using ConfigType = Config;

    static_assert(std::atomic<bool>::is_always_lock_free,
                  "Dispatcher wake latch requires lock-free atomic<bool>");
    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "Submission admission requires lock-free atomic<uint32_t>");
    using HighQueue = QueueBackend<StagingSlot, Config::kHighCapacity>;
    using NormalQueue = QueueBackend<StagingSlot, Config::kNormalCapacity>;
    using LowQueue = QueueBackend<StagingSlot, Config::kLowCapacity>;

    explicit Staging(CriticalSection cs) noexcept
        : high_q_(cs),
          normal_q_(cs),
          low_q_(cs) {}

    Staging(const Staging&) = delete;
    Staging& operator=(const Staging&) = delete;

    // Route the event to the partition of the given PriorityClass. Returns
    // false when that partition is full; the caller (coordinator) then owns
    // the decision to event_gc the Event* immediately. The reference count is
    // never touched here.
    bool enqueue(TargetId target, Event* e, PriorityClass cls, uint64_t now_ns) noexcept
    {
        StagingSlot slot{target, e, now_ns};

        if (cls == PriorityClass::Low) {
            return low_q_.try_push(std::move(slot));
        }

        if (cls == PriorityClass::Normal) {
            return normal_q_.try_push(std::move(slot));
        }

        if (cls == PriorityClass::High) {
            return high_q_.try_push(std::move(slot));
        }
        return false;   // unreachable: explicit default
    }

    // Pop one slot in batch order (priority-first with the Low aging
    // exception). now_ns is the current monotonic time (used to judge whether
    // the Low head has aged past kLowMaxWaitMs); pass it each batch so no
    // separate tick() call is strictly required. Returns false when all
    // partitions are empty, or when the current batch already reached
    // BatchSizeMax. Succeeding increments the in-progress batch counter; call
    // begin_batch() to open a new one.
    bool dequeue_one(StagingSlot& out, uint64_t now_ns) noexcept
    {
        now_ns_ = now_ns;
        now_valid_ = true;

        /* A Writing MPSC cell counts toward shutdown occupancy before it
           publishes its payload, so `size() > 0` does not imply try_pop()
           succeeds. Refuse a dequeue early unless some partition has a Ready
           cell, otherwise the caller would loop while every cell is Writing. */
        if (!any_ready()) {
            return false;
        }

        Partition part;
        const uint16_t bmax = static_cast<uint16_t>(Config::kBatchSizeMax);
        if (!selector_.select(part,
                              ready_count(Partition::High),
                              ready_count(Partition::Normal),
                              ready_count(Partition::Low),
                              aging_expired(),
                              batch_used_,
                              bmax)) {
            return false;
        }

        bool ok = false;
        switch (part) {
        case Partition::High:
            ok = high_q_.try_pop(out);
            break;
        case Partition::Normal:
            ok = normal_q_.try_pop(out);
            break;
        case Partition::Low:
            ok = low_q_.try_pop(out);
            break;
        default:
            ok = false;   // unreachable: explicit default
            break;
        }

        if (ok) {
            ++batch_used_;
        }
        return ok;
    }

    // Compat shim used by tests / callers that drive the aging clock via
    // tick() (or never need Low aging): routes through the cached now.
    bool dequeue_one(StagingSlot& out) noexcept
    {
        return dequeue_one(out, now_valid_ ? now_ns_ : 0U);
    }

    // Begin a new dispatch batch, resetting the batch-size accounting.
    void begin_batch() noexcept
    {
        batch_used_ = 0U;
    }

    // Number of slots already drawn in the current batch.
    uint8_t batch_used() const noexcept
    {
        return batch_used_;
    }

    // Refresh the aging clock base. The dispatcher calls this with the current
    // monotonic time so dequeue_one can judge whether a Low head has aged past
    // LowMaxWaitMs. Never called from an ISR producer path.
    void tick(uint64_t now_ns) noexcept
    {
        now_ns_ = now_ns;
        now_valid_ = true;
    }

    /* ---- Coalesced Dispatcher wake latch -------------------------------
       The latch starts set while the Dispatcher is active, suppressing
       redundant signals. Immediately before an idle wait the Dispatcher clears
       it with an acq_rel exchange, then re-checks Ready payloads. A producer
       publishes first and sets the latch afterwards: if it observes false it
       owns the wake signal; if it observes true, the Dispatcher's acquire side
       observes the publication before deciding whether to sleep. */
    void arm_dispatcher_wait() noexcept
    {
        wake_pending_.exchange(false, std::memory_order_acq_rel);
    }

    bool request_dispatcher_wake() noexcept
    {
        return !wake_pending_.exchange(true, std::memory_order_acq_rel);
    }

    // A submission lease linearizes Coordinator ingress with shutdown. Closing
    // admission rejects new submissions, while already admitted producers keep
    // their lease until direct dispatch or queue publication is complete.
    bool acquire_submission() noexcept
    {
        uint32_t state = admission_.load(std::memory_order_acquire);
        for (;;) {
            if (0U != (state & kAdmissionClosed)) {
                return false;
            }
            if ((state & kAdmissionCountMask) == kAdmissionCountMask) {
                return false;
            }
            if (admission_.compare_exchange_weak(
                    state, state + 1U,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
        }
    }

    // Returns true exactly when this was the last accepted submission after
    // admission closed, so its caller can wake a shutdown drain.
    bool release_submission() noexcept
    {
        const uint32_t before = admission_.fetch_sub(1U, std::memory_order_acq_rel);
        return (kAdmissionClosed | 1U) == before;
    }

    void close_admission() noexcept
    {
        admission_.fetch_or(kAdmissionClosed, std::memory_order_acq_rel);
    }

    bool submissions_idle() const noexcept
    {
        return 0U == (admission_.load(std::memory_order_acquire) & kAdmissionCountMask);
    }

    // True when at least one partition is non-empty (re-check before sleep).
    bool any_buffered() const noexcept
    {
        return (0U != size(Partition::High))
            || (0U != size(Partition::Normal))
            || (0U != size(Partition::Low));
    }

    bool any_ready() const noexcept
    {
        return high_q_.has_ready()
            || normal_q_.has_ready()
            || low_q_.has_ready();
    }

    // Usage as a 0-100 percentage: used / capacity * 100. The dispatcher maps
    // this to the 50/80/95 watermark bands (design 10.5): <50 normal batch,
    // 50-80 enlarge batch, 80-95 wake immediately, >95 hard-throttle.
    uint8_t watermark(Partition p) const noexcept
    {
        uint16_t used = 0U;
        uint16_t cap = 1U;
        switch (p) {
        case Partition::High:
            used = high_q_.size();
            cap = Config::kHighCapacity;
            break;
        case Partition::Normal:
            used = normal_q_.size();
            cap = Config::kNormalCapacity;
            break;
        case Partition::Low:
            used = low_q_.size();
            cap = Config::kLowCapacity;
            break;
        default:
            break;   // unreachable: explicit default
        }
        if (used == 0U) {
            return 0U;
        }
        uint32_t pct = (static_cast<uint32_t>(used) * 100U)
                     / static_cast<uint32_t>(cap);
        if (pct > 100U) {
            pct = 100U;
        }
        return static_cast<uint8_t>(pct);
    }

    // Current number of buffered slots in a partition.
    uint16_t size(Partition p) const noexcept
    {
        switch (p) {
        case Partition::High:
            return high_q_.size();
        case Partition::Normal:
            return normal_q_.size();
        case Partition::Low:
            return low_q_.size();
        default:
            return 0U;   // unreachable: explicit default
        }
    }

private:
    static constexpr uint32_t kAdmissionClosed = 0x80000000U;
    static constexpr uint32_t kAdmissionCountMask = ~kAdmissionClosed;

    uint16_t ready_count(Partition p) const noexcept
    {
        switch (p) {
        case Partition::High:
            return high_q_.has_ready() ? 1U : 0U;
        case Partition::Normal:
            return normal_q_.has_ready() ? 1U : 0U;
        case Partition::Low:
            return low_q_.has_ready() ? 1U : 0U;
        default:
            return 0U;
        }
    }

    // True when the Low partition has an outstanding timeout: a low event has
    // been waiting at the Low head for at least LowMaxWaitMs (unsigned
    // wrap-safe comparison). Only meaningful while Low is non-empty.
    //
    // The low head's arrival may be LATER than the cached now_ns_ when the
    // Dispatcher samples the clock once at batch start and a producer enqueues
    // the Low mid-batch. A bare now - arrival would then underflow and spuriously
    // age the event, force-serving it ahead of higher-priority events. Guard the
    // subtraction with now >= arrival so a mid-batch arrival counts as not yet
    // aged (regression: coact dispatcher underflow gap).
    bool aging_expired() const noexcept
    {
        if (!now_valid_) {
            return false;
        }
        const uint64_t wait_ns =
            static_cast<uint64_t>(Config::kLowMaxWaitMs) * 1000000ULL;
        StagingSlot head;
        if (!low_q_.front(head)) {
            return false;
        }
        const uint64_t arrival = head.enqueue_ns;
        if (now_ns_ < arrival) {
            return false;   // mid-batch arrival: now is stale before the Low came in
        }
        const uint64_t waited = now_ns_ - arrival;
        return (waited >= wait_ns);
    }

    HighQueue high_q_;
    NormalQueue normal_q_;
    LowQueue low_q_;

    BatchSelector selector_;
    std::atomic<bool> wake_pending_{true};
    std::atomic<uint32_t> admission_{0U};
    uint64_t now_ns_ = 0U;                // cached aging clock base
    bool now_valid_ = false;
    uint8_t batch_used_ = 0U;
};

}  // namespace coact
