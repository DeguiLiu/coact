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
// Each batch: tick(now) -> begin_batch -> dequeue loop ->
// ao->try_dispatch_queued -> event_gc. When all queues are empty the thread
// sleeps via pal.wait_dispatcher and wakes on signal_dispatcher_from_task/isr.
//
// Key constraint (S6 definition, interface_contract 4.6): queued dispatch uses
// Ao::try_dispatch_queued(), whose CAS is the only lease acquisition. When a
// concurrent direct RTC owns the lease, the Dispatcher retains one deferred
// event, arms the wake latch, retries the CAS, then blocks through the PAL.
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
          typename Profile = coact::HostSmpProfile,
          typename BreakerRouterT = Breaker<typename StagingT::ConfigType>>
class Dispatcher
{
public:
    using RegistryT = AoRegistry<typename StagingT::ConfigType>;
    using MonitorT  = Monitor<typename StagingT::ConfigType>;
    using BatchCfg  = typename StagingT::ConfigType;

    // ReclaimBatcher per-batch distinct-pool capacity (design §7.4):
    // min(kBatchSizeMax, kMaxEventPools). A batch dequeues at most
    // kBatchSizeMax events and only kMaxEventPools pools exist, so this bounds
    // the pools one batch can touch.
    static constexpr uint16_t kBatchPools =
        coact::detail::reclaimer_pool_capacity<BatchCfg>();
    using ReclaimerT = coact::detail::select_reclaimer_t<Profile, kBatchPools>;

    Dispatcher(StagingT& staging, RegistryT& registry,
               MonitorT& monitor, BreakerRouterT& breaker, PalT& pal) noexcept
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
        StagingSlot deferred;
        bool has_deferred = false;
        while (!stop_.load(std::memory_order_acquire)) {
            /* Open a new batch; the Low aging clock is refreshed per dequeue
               below so mid-batch arrivals are judged against current time.
               The set wake latch coalesces producer signals while active. */
            staging_.begin_batch();

            /* Batched/immediate reclaim selected by the board profile. Every
               dequeued event releases its final reference here; the batched
               strategy collapses many single free_head CAS ops into one splice
               per pool, flushed at batch end. */
            ReclaimerT reclaim;

            bool any = false;
            if (has_deferred) {
                staging_.arm_dispatcher_wait();
                if (try_dispatch_slot(deferred, reclaim)) {
                    has_deferred = false;
                    any = true;
                }
                else {
                    reclaim.flush();
                    pal_.wait_dispatcher(BatchCfg::kBatchTimeoutMs);
                    continue;
                }
            }
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
                if (!try_dispatch_slot(slot, reclaim)) {
                    deferred = slot;
                    has_deferred = true;
                    break;
                }
            }
            reclaim.flush();

            if (stop_.load(std::memory_order_acquire)) {
                break;   // batch interrupted by stop: exit to the shutdown drain
            }
            if (has_deferred) {
                /* Close the release-before-wait window: clear the wake latch,
                   retry the lease CAS, and sleep only if the retry still loses.
                   Direct completion observes pending > 0 and owns the wake. */
                staging_.arm_dispatcher_wait();
                ReclaimerT deferred_reclaim;
                if (try_dispatch_slot(deferred, deferred_reclaim)) {
                    has_deferred = false;
                    deferred_reclaim.flush();
                    continue;
                }
                deferred_reclaim.flush();
                pal_.wait_dispatcher(BatchCfg::kBatchTimeoutMs);
                continue;
            }
            if (!any) {
                /* Feed watchdog on idle cycles so the breaker cooldown
                   does not stall when there is no traffic. */
                detail::breaker_idle_cycle(breaker_);
                /* Arm the wait with an acq_rel exchange, then re-check Ready
                   payloads. Publication-before-arm is acquired here; a
                   publication-after-arm owns the PAL signal. */
                staging_.arm_dispatcher_wait();
                if (staging_.any_ready()) {
                    continue;   /* something arrived before we slept */
                }
                pal_.wait_dispatcher(BatchCfg::kBatchTimeoutMs);
            }
        }
        if (has_deferred) {
            ReclaimerT reclaim;
            AoBase* ao = registry_.lookup(deferred.target);
            if (ao != nullptr) {
                ao->pending().decrement();
            }
            reclaim.release(deferred.event);
            reclaim.flush();
        }
        drain_queued_on_stop();
    }

    // Thread-safe stop request. The run() loop checks this after each batch.
    void request_stop() noexcept
    {
        staging_.close_admission();
        stop_.store(true, std::memory_order_release);
        pal_.signal_dispatcher_from_task();
    }

private:
    bool try_dispatch_slot(const StagingSlot& slot,
                           ReclaimerT& reclaim) noexcept
    {
        AoBase* ao = registry_.lookup(slot.target);
        if (ao != nullptr) {
            Breaker<typename StagingT::ConfigType>& target_breaker =
                detail::target_breaker(breaker_, slot.target);
            const uint64_t t0 = pal_.monotonic_ns();
            if (!ao->try_dispatch_queued(*slot.event)) {
                return false;
            }
            const uint64_t elapsed = pal_.monotonic_ns() - t0;
            if (elapsed > ao->rtc_budget_ns()) {
                target_breaker.on_dispatcher_rtc_timeout();
            } else {
                target_breaker.on_rtc_ok();
            }
            ao->pending().decrement();
            target_breaker.on_dispatch_cycle();
            monitor_.record_disposition(SubmitDisposition::Queued);
        }
        reclaim.release(slot.event);
        return true;
    }

    // Shutdown drain (design §7.4 / §15.4): the Dispatcher is the final
    // release-er of every queued event. On stop, whatever is still buffered is
    // drained WITHOUT dispatch and each final reference is released exactly
    // once, matching the coordinator's pending().increment() with a decrement
    // so every pool returns to used()==0 and every AO pending() to 0.
    void drain_queued_on_stop() noexcept
    {
        const uint64_t now_ns = pal_.monotonic_ns();
        while (staging_.any_buffered() || !staging_.submissions_idle()) {
            staging_.begin_batch();
            ReclaimerT reclaim;
            bool released = false;
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
                released = true;
            }
            reclaim.flush();
            if (!released && (staging_.any_buffered() || !staging_.submissions_idle())) {
                pal_.wait_dispatcher(BatchCfg::kBatchTimeoutMs);
            }
        }
    }

    StagingT&    staging_;
    RegistryT&   registry_;
    MonitorT&    monitor_;
    BreakerRouterT& breaker_;
    PalT&        pal_;
    std::atomic<bool> stop_;
};

}  // namespace coact
