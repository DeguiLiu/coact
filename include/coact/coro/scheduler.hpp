// coact::coro timer facade: thin re-export of coact::TimerScheduler for
// task timeouts and deferred work, plus the single-core poll() drive mode.
// SPDX-License-Identifier: MIT
//
// One-shot and periodic timers become signal events to a target AO (the
// coact::TimerScheduler contract: a timer is an EVENT SOURCE, never a
// direct AO mutator). Cancellation returns Expected<void, TimerError>.
//
// Single-core model (explicit acceptance rule):
//   - Production: SteadyTickSource + start() runs the timer loop on one
//     background pthread. When the whole application must stay on one
//     thread, do NOT call start(); drive the scheduler with poll():
//       TimerFacade<...>::Polling scheduler(pool, coordinator);  // ManualTickSource
//       scheduler.tick_source().advance(n);
//       scheduler.poll();
//     Tests use exactly this mode: deterministic, no sleeps, no threads.
//   - The facade adds no threads of its own.
#pragma once

#include "coact/config.hpp"
#include "coact/coordinator.hpp"
#include "coact/expected.hpp"
#include "coact/pool.hpp"
#include "coact/timer.hpp"

namespace coact {
namespace coro {

// Facade over coact::TimerScheduler. MaxTasks is the compile-time timer
// slot capacity. TickSourceT selects the clock: coact::SteadyTickSource
// (production) or coact::ManualTickSource (tests / external single-thread
// event loops).
template <typename PoolT, typename CoordinatorT,
          uint32_t MaxTasks = 16U,
          typename TickSourceT = coact::SteadyTickSource>
class TimerFacade final {
public:
    using Scheduler = coact::TimerScheduler<PoolT, CoordinatorT, MaxTasks,
                                            TickSourceT>;

    explicit TimerFacade(PoolT& pool, CoordinatorT& coordinator) noexcept
        : scheduler_(pool, coordinator)
    {
    }

    // Register a one-shot `signal` -> `target` after delay_ms. The slot
    // auto-frees after firing.
    Expected<coact::TimerTaskId, coact::TimerError>
    schedule_once(coact::TargetId target, uint16_t signal, uint32_t delay_ms,
                  const coact::EventQos& qos) noexcept
    {
        return scheduler_.schedule_once(target, signal, delay_ms, qos);
    }

    // Register a periodic `signal` -> `target` every period_ms.
    Expected<coact::TimerTaskId, coact::TimerError>
    schedule_periodic(coact::TargetId target, uint16_t signal,
                      uint32_t period_ms,
                      const coact::EventQos& qos) noexcept
    {
        return scheduler_.schedule_periodic(target, signal, period_ms, qos);
    }

    // Cancel by timer id; kNotFound for unknown / already-fired ids.
    Expected<void, coact::TimerError> cancel(coact::TimerTaskId id) noexcept
    {
        return scheduler_.cancel(id);
    }

    // Single-threaded drive pass (see class comment). No-op cost when
    // nothing is due.
    void poll() noexcept { scheduler_.poll(); }

    // Threaded drive mode (production). Applications restricted to one
    // thread must use poll() instead.
    Expected<void, coact::TimerError> start() noexcept
    {
        return scheduler_.start();
    }

    void stop() noexcept { scheduler_.stop(); }

    // Direct tick-source access (tests advance a ManualTickSource).
    TickSourceT& tick_source() noexcept { return scheduler_.tick_source(); }

    // Monitoring counters (TimerScheduler semantics).
    uint32_t fired_count() const noexcept
    {
        return scheduler_.fired_count();
    }
    uint32_t skipped_count() const noexcept
    {
        return scheduler_.skipped_count();
    }
    uint32_t task_count() const noexcept { return scheduler_.task_count(); }

private:
    Scheduler scheduler_;
};

}  // namespace coro
}  // namespace coact
