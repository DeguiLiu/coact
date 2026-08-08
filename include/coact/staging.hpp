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
// the coordinator/dispatcher - staging only stores an already-inc'ed Event*,
// it never calls event_ref_inc/event_gc nor allocates.
//
// Queue backend is a template-template parameter (BoundedMpscQueue for SMP or
// SingleCoreCriticalRing for a single core); it is instantiated once per
// partition at the partition's own capacity. No heap allocation.
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <type_traits>
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
// One buffered event. The wrapped Event* is already ref-inc'ed by the
// producer; the consumer (dispatcher) is responsible for event_gc after it
// finishes dispatch. enqueue_ns drives the Low aging deadline for batch
// ordering (unsigned wrap-safe comparison).
// ---------------------------------------------------------------------------
struct StagingSlot {
    TargetId target;
    Event* event;           // already-inc'ed reference; dispatcher event_gc
    uint64_t enqueue_ns;    // publish time, used for Low aging
};

// ---------------------------------------------------------------------------
// Pure batch-order selection (no queue storage, fully testable). Inputs are
// the per-partition sizes plus the aging signal; the output is the next
// partition a dequeue should serve.
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
// Lifetime note: enqueue stores an already-ref-inc'ed Event* and never changes
// the reference count; dequeue hands ownership of the slot's Event* back to
// the consumer. The dispatcher must event_gc each dequeued slot.
// ---------------------------------------------------------------------------
template <typename Config,
          template <typename, uint16_t> class QueueBackend>
class Staging {
public:
    using ConfigType = Config;
    using HighQueue = QueueBackend<StagingSlot, Config::kHighCapacity>;
    using NormalQueue = QueueBackend<StagingSlot, Config::kNormalCapacity>;
    using LowQueue = QueueBackend<StagingSlot, Config::kLowCapacity>;

    explicit Staging(CriticalSection cs) noexcept
        : high_q_(),
          normal_q_(),
          low_q_()
    {
        construct_queue(high_q_, cs);
        construct_queue(normal_q_, cs);
        construct_queue(low_q_, cs);
    }

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
            if (!low_q_.has_value()) {
                return false;
            }
            const bool was_empty = (low_q_->size() == 0U);
            if (!low_q_->try_push(std::move(slot))) {
                return false;
            }
            if (was_empty) {
                low_head_arrival_ns_ = now_ns;
            }
            return true;
        }

        if (cls == PriorityClass::Normal) {
            if (normal_q_.has_value()) {
                return normal_q_->try_push(std::move(slot));
            }
            return false;
        }

        if (cls == PriorityClass::High) {
            if (high_q_.has_value()) {
                return high_q_->try_push(std::move(slot));
            }
            return false;
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

        Partition part;
        const uint16_t bmax = static_cast<uint16_t>(Config::kBatchSizeMax);
        if (!selector_.select(part,
                              size(Partition::High),
                              size(Partition::Normal),
                              size(Partition::Low),
                              aging_expired(),
                              batch_used_,
                              bmax)) {
            return false;
        }

        bool ok = false;
        switch (part) {
        case Partition::High:
            ok = high_q_.has_value() && high_q_->try_pop(out);
            break;
        case Partition::Normal:
            ok = normal_q_.has_value() && normal_q_->try_pop(out);
            break;
        case Partition::Low:
            ok = low_q_.has_value() && low_q_->try_pop(out);
            if (low_q_.has_value() && low_q_->size() == 0U) {
                low_head_arrival_ns_ = 0U;   // Low drained: reset aging clock
            }
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

    /* ---- Dispatcher-activity flag --------------------------------------
       Producers signal the Dispatcher only when it is idle; while it is
       actively draining it picks up newly enqueued events in the same batch.
       The Dispatcher clears the flag (release) immediately before sleeping,
       AFTER a final queue re-check, so an enqueue that raced with the clear is
       observed by the producer (which then signals) or by the re-check (which
       then continues). Closing the missed-wakeup window this way removes the
       per-submit condvar/semaphore wakeup (flame #1 on single-core). */
    void mark_dispatcher_active() noexcept
    {
        dispatcher_active_.store(true, std::memory_order_release);
    }

    void mark_dispatcher_idle() noexcept
    {
        dispatcher_active_.store(false, std::memory_order_release);
    }

    bool dispatcher_active() const noexcept
    {
        return dispatcher_active_.load(std::memory_order_acquire);
    }

    // True when at least one partition is non-empty (re-check before sleep).
    bool any_buffered() const noexcept
    {
        return (0U != size(Partition::High))
            || (0U != size(Partition::Normal))
            || (0U != size(Partition::Low));
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
            used = high_q_.has_value() ? high_q_->size() : 0U;
            cap = Config::kHighCapacity;
            break;
        case Partition::Normal:
            used = normal_q_.has_value() ? normal_q_->size() : 0U;
            cap = Config::kNormalCapacity;
            break;
        case Partition::Low:
            used = low_q_.has_value() ? low_q_->size() : 0U;
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
            return high_q_.has_value() ? high_q_->size() : 0U;
        case Partition::Normal:
            return normal_q_.has_value() ? normal_q_->size() : 0U;
        case Partition::Low:
            return low_q_.has_value() ? low_q_->size() : 0U;
        default:
            return 0U;   // unreachable: explicit default
        }
    }

private:
    // Instantiate a partition queue behind its optional. The Mpsc backend is
    // default-constructible and ignores CriticalSection; the single-core ring
    // requires the injected CriticalSection for its save/restore.
    template <typename Q>
    void construct_queue(std::optional<Q>& q, CriticalSection cs) noexcept
    {
        if constexpr (std::is_default_constructible<Q>::value) {
            q.emplace();
        }
        else {
            q.emplace(cs);
        }
    }

    // True when the Low partition has an outstanding timeout: a low event has
    // been waiting at the Low head for at least LowMaxWaitMs (unsigned
    // wrap-safe comparison). Only meaningful while Low is non-empty.
    bool aging_expired() const noexcept
    {
        if (!now_valid_) {
            return false;
        }
        const uint64_t wait_ns =
            static_cast<uint64_t>(Config::kLowMaxWaitMs) * 1000000ULL;
        const uint64_t waited = now_ns_ - low_head_arrival_ns_;
        return (waited >= wait_ns);
    }

    std::optional<HighQueue> high_q_;
    std::optional<NormalQueue> normal_q_;
    std::optional<LowQueue> low_q_;

    BatchSelector selector_;
    std::atomic<bool> dispatcher_active_{false};
    uint64_t low_head_arrival_ns_ = 0U;   // publish time of the Low head
    uint64_t now_ns_ = 0U;                // cached aging clock base
    bool now_valid_ = false;
    uint8_t batch_used_ = 0U;
};

}  // namespace coact
