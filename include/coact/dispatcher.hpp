// coact Dispatcher - single-thread batch dispatch loop over the three-tier
// staging queues. See design 13 and implementation contract 4.8.
//
// Model adapted from QP/C++ QF dispatcher loop (src/qf/qf_act.cpp);
// the coact API is an original re-expression. Project license: MIT.
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/event.hpp"
#include "coact/monitor.hpp"
#include "coact/pool.hpp"
#include "coact/staging.hpp"

namespace coact {

// ---------------------------------------------------------------------------
// Single-threaded Dispatcher. Runs on a dedicated thread started by the PAL.
// Each batch: tick(now) -> begin_batch -> dequeue loop -> ao->dispatch ->
// event_gc. When all queues are empty the thread sleeps via pal.wait_dispatcher
// and wakes on signal_dispatcher_from_task/isr.
//
// Key constraint (S6 definition, interface_contract 4.6):
//   Ao::dispatch() owns its own execution lease internally. The Dispatcher
//   MUST NOT try_acquire the lease before calling ao->dispatch(); doing so
//   causes a double-acquire and an immediate COACT_ASSERT abort.
//
// Reclaimer policy (design §7.4): the reclaimer strategy is selected by the
// board profile. RttSingleCoreProfile starts with ImmediateReclaimer (no batch
// chaining); HostSmpProfile uses the batched ReclaimBatcher, whose per-batch
// distinct-pool capacity is min(kBatchSizeMax, kMaxEventPools) - never a
// hard-coded 4. The Dispatcher is the final release-er of every queued event:
// every batch is flushed (including a batch interrupted by stop), the lookup-
// failure path still releases, and a stop drains the remaining queue exactly
// once each.
// ---------------------------------------------------------------------------
template <typename StagingT, typename PalT,
          typename Profile = coact::HostSmpProfile>
class Dispatcher
{
public:
    using RegistryT = AoRegistry<typename StagingT::ConfigType>;
    using MonitorT  = Monitor<typename StagingT::ConfigType>;
    using BreakerT  = Breaker<typename StagingT::ConfigType>;
    using BatchCfg  = typename StagingT::ConfigType;

    // ReclaimBatcher per-batch distinct-pool capacity (design §7.4):
    // min(kBatchSizeMax, kMaxEventPools). A batch dequeues at most
    // kBatchSizeMax events and only kMaxEventPools pools exist, so this bounds
    // the pools one batch can touch.
    static constexpr uint16_t kBatchPools =
        coact::detail::reclaimer_pool_capacity<BatchCfg>();
    using ReclaimerT = coact::detail::select_reclaimer_t<Profile, kBatchPools>;

    Dispatcher(StagingT& staging, RegistryT& registry,
               MonitorT& monitor, BreakerT& breaker, PalT& pal) noexcept
        : staging_(staging),
          registry_(registry),
          monitor_(monitor),
          breaker_(breaker),
          pal_(pal),
          stop_(false)
    {
    }

    // Real thread identity (R1): true only when the current thread is the coact
    // Dispatcher thread. Delegates to the PAL's thread-local check
    // (Posix::in_dispatcher_thread / RtThread::in_dispatcher_thread), which no
    // other thread can forge. Static so cmdfw's single-writer gate can bind it
    // as a callback without a Dispatcher instance.
    static bool in_dispatcher_thread() noexcept
    {
        return PalT::in_dispatcher_thread();
    }

    // Main loop. Called from the PAL dispatcher thread via ThreadEntry.
    // Every batch is flushed - including a batch interrupted by a stop request
    // - and a stop drains the remaining queue (no dispatch) so each queued
    // event is released exactly once.
    void run() noexcept
    {
        while (!stop_.load(std::memory_order_acquire)) {
            /* Open a new batch; the Low aging clock is refreshed per dequeue
               below so mid-batch arrivals are judged against current time.
               While active, producers skip the wakeup signal. */
            staging_.begin_batch();
            staging_.mark_dispatcher_active();

            /* Batched/immediate reclaim selected by the board profile. Every
               dequeued event releases its final reference here; the batched
               strategy collapses many single free_head CAS ops into one splice
               per pool, flushed at batch end. */
            ReclaimerT reclaim;

            bool any = false;
            while (!stop_.load(std::memory_order_acquire) &&
                   staging_.batch_used() < BatchCfg::kBatchSizeMax) {
                StagingSlot slot;
                /* Refresh now on every dequeue (not once per batch): Low events
                   enqueued mid-batch carry a later arrival than the batch-start
                   clock, so a stale batch-start now would make aging underflow
                   or mis-judge them. Using the current time lets only
                   genuinely-aged Low heads force-serve. */
                const uint64_t now_ns = pal_.monotonic_ns();
                if (!staging_.dequeue_one(slot, now_ns)) {
                    break;
                }
                any = true;

                AoBase* ao = registry_.lookup(slot.target);
                if (ao != nullptr) {
                    /* Ao::dispatch() acquires RunningDispatcher lease
                       internally; never pre-acquire here. */
                    ao->dispatch(*slot.event);
                    /* Match the coordinator's pending().increment() on enqueue
                       (design 9.3: decrement after final dispatch). Without this
                       the uint16 pending counter leaks and wraps -> underflow. */
                    ao->pending().decrement();
                    breaker_.on_dispatch_cycle();
                    monitor_.record_disposition(SubmitDisposition::Queued);
                }
                /* Release the reference regardless of lookup result. */
                reclaim.release(slot.event);
            }
            reclaim.flush();

            if (stop_.load(std::memory_order_acquire)) {
                break;   // batch interrupted by stop: exit to the shutdown drain
            }
            if (!any) {
                /* Feed watchdog on idle cycles so the breaker cooldown
                   does not stall when there is no traffic. */
                breaker_.on_dispatch_cycle();
                /* Going to sleep: clear the active flag (release), then
                   re-check so an event enqueued against the stale active bit
                   is not stranded. Producers signal when they observe idle. */
                staging_.mark_dispatcher_idle();
                if (staging_.any_buffered()) {
                    continue;   /* something arrived before we slept */
                }
                pal_.wait_dispatcher(BatchCfg::kBatchTimeoutMs);
            }
        }
        drain_queued_on_stop();
    }

    // Thread-safe stop request. The run() loop checks this after each batch.
    void request_stop() noexcept
    {
        stop_.store(true, std::memory_order_release);
        pal_.signal_dispatcher_from_task();
    }

private:
    // Shutdown drain (design §7.4 / §15.4): the Dispatcher is the final
    // release-er of every queued event. On stop, whatever is still buffered is
    // drained WITHOUT dispatch and each final reference is released exactly
    // once, matching the coordinator's pending().increment() with a decrement
    // so every pool returns to used()==0 and every AO pending() to 0.
    void drain_queued_on_stop() noexcept
    {
        const uint64_t now_ns = pal_.monotonic_ns();
        while (staging_.any_buffered()) {
            staging_.begin_batch();
            ReclaimerT reclaim;
            while (staging_.batch_used() < BatchCfg::kBatchSizeMax) {
                StagingSlot slot;
                if (!staging_.dequeue_one(slot, now_ns)) {
                    break;
                }
                AoBase* ao = registry_.lookup(slot.target);
                if (ao != nullptr) {
                    ao->pending().decrement();
                }
                reclaim.release(slot.event);
            }
            reclaim.flush();
        }
    }

    StagingT&    staging_;
    RegistryT&   registry_;
    MonitorT&    monitor_;
    BreakerT&    breaker_;
    PalT&        pal_;
    std::atomic<bool> stop_;
};

}  // namespace coact
