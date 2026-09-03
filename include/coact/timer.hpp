// coact TimerScheduler - fixed-capacity periodic / one-shot timer component
// that acts as an EVENT SOURCE, not a callback dispatcher.
// SPDX-License-Identifier: MIT
//
// Adapted from newosp include/osp/timer.hpp (MIT License,
// Copyright (c) 2024 liudegui). Coact-specific adaptations:
//
//   1. Callback -> event (core change): expiration never invokes a function
//      pointer. Each due task allocates a signal-only coact::Event from the
//      caller's EventPool and submits it through
//      DispatchCoordinator::submit_from_task(). The timer is just another
//      producer; target AOs mutate their own state only inside their RTC
//      step (AO discipline, design 5). This is the deliberate coact rule:
//      a timer must NEVER directly poke another AO's internals.
//
//   2. Dependency strip: osp::Thread / osp::Mutex / ThreadHeartbeat are gone.
//      The background loop runs on a bare pthread (same style as the demo
//      WorkerBase; RT-Thread builds need the pthread layer, or drive poll()
//      from a board thread). Slot state is guarded by std::mutex - the lock
//      window is only a fixed MaxTasks scan, so atomicization is a recorded
//      follow-up optimization, not a correctness need.
//
//   3. Pool-full policy: when the pool is exhausted at expiration time the
//      firing is SKIPPED and counted in skipped_count() - never blocks,
//      never faults, the task stays scheduled for its next period.
//
//   4. TickSource strategy (retained from newosp):
//        - SteadyTickSource: real CLOCK_MONOTONIC clock plus a drift-free
//          absolute sleep (clock_nanosleep on POSIX hosts; RT-Thread builds
//          fall back to a coarse rt_thread_mdelay tick delay).
//        - ManualTickSource: instance-owned virtual clock for deterministic
//          tests and ISR/board-thread-driven embedded use. advance() the
//          ticks, then call poll(); no background thread is needed (and
//          start() with a ManualTickSource would hot-spin - do not).
//
// Retained newosp design points: compile-time MaxTasks capacity, zero heap,
// collect-release-execute lock discipline (expired-task submissions run
// OUTSIDE the internal mutex), missed-period catch-up that collapses missed
// periods instead of bursting, and ns_to_next_task() for external event
// loops / power management.
//
// Compatible with -fno-exceptions -fno-rtti.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <pthread.h>

#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#include <time.h>
#define COACT_TIMER_POSIX_CLOCK 1
#else
/* RT-Thread target: coarse millisecond delay via the kernel tick. */
#include <rtthread.h>
#endif

#include "coact/config.hpp"
#include "coact/coordinator.hpp"
#include "coact/event.hpp"
#include "coact/expected.hpp"
#include "coact/pool.hpp"
#include "coact/vocabulary.hpp"

namespace coact {

// ---------------------------------------------------------------------------
// Timer task identity. NewType alias in the TargetId style: explicit
// construction keeps arbitrary integers from silently becoming a timer id;
// kInvalidTimerTaskId (0) is never handed out by schedule_*().
// ---------------------------------------------------------------------------
struct TimerTaskTag {};
using TimerTaskId = coact::NewType<uint32_t, TimerTaskTag>;
inline constexpr TimerTaskId kInvalidTimerTaskId(0U);
static_assert(sizeof(TimerTaskId) == 4U, "coact: TimerTaskId must be zero-overhead");

// Module error vocabulary for the timer component.
enum class TimerError : uint8_t {
    kInvalidPeriod = 0U,   // period_ms / delay_ms == 0
    kSlotsFull,            // all MaxTasks slots occupied
    kNotFound,             // cancel() on an unknown / already-fired task id
    kAlreadyRunning,       // start() while the scheduler thread runs
    kThreadStartFailed     // pthread_create failed
};

// ---------------------------------------------------------------------------
// TickSource strategies.
// ---------------------------------------------------------------------------

// Real monotonic clock + drift-free absolute sleep. Production default.
struct SteadyTickSource {
    uint64_t now_ns() const noexcept
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    }

    // POSIX: absolute sleep on CLOCK_MONOTONIC (drift-free across iterations).
    // RT-Thread: coarse kernel tick delay at millisecond resolution.
    void sleep_ns(uint64_t ns) const noexcept
    {
#if defined(COACT_TIMER_POSIX_CLOCK)
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        /* Split ns before adding so tv_nsec never overflows. */
        ts.tv_sec += static_cast<time_t>(ns / 1000000000ULL);
        ts.tv_nsec += static_cast<long>(ns % 1000000000ULL);
        if (1000000000L <= ts.tv_nsec) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
#else
        rt_thread_mdelay(static_cast<rt_int32_t>((ns + 999999ULL) / 1000000ULL));
#endif
    }
};

// Instance-owned virtual clock: the test driver (or a hardware timer ISR /
// board thread) advances ticks and calls TimerScheduler::poll(). Unlike the
// newosp static-state original, the counters live in the instance so several
// schedulers in one test binary never share clock state.
class ManualTickSource {
public:
    explicit ManualTickSource(uint64_t tick_period_ns = 1000000ULL) noexcept
        : tick_period_ns_(tick_period_ns)
    {
    }

    uint64_t now_ns() const noexcept
    {
        return ticks_.load(std::memory_order_acquire) *
               tick_period_ns_.load(std::memory_order_relaxed);
    }

    void tick() noexcept { ticks_.fetch_add(1U, std::memory_order_release); }

    void advance(uint32_t n) noexcept
    {
        ticks_.fetch_add(static_cast<uint64_t>(n), std::memory_order_release);
    }

    void reset() noexcept { ticks_.store(0U, std::memory_order_release); }

    uint64_t ticks() const noexcept
    {
        return ticks_.load(std::memory_order_acquire);
    }

    uint64_t tick_period_ns() const noexcept
    {
        return tick_period_ns_.load(std::memory_order_relaxed);
    }

    // Manual mode never sleeps: the driver decides when the next poll() runs.
    void sleep_ns(uint64_t /*ns*/) const noexcept {}

private:
    std::atomic<uint64_t> ticks_{0U};
    std::atomic<uint64_t> tick_period_ns_;
};

// ---------------------------------------------------------------------------
// TimerScheduler: fixed-capacity periodic / one-shot timer event source.
//
// Template parameters:
//   PoolT        - the board's coact::EventPool instantiation. Expiration
//                  allocates a signal-only Event from it (pool.alloc); a
//                  payload-carrying timer would switch to alloc_typed with
//                  a composed EventBlockLayout.
//   CoordinatorT - coact::DispatchCoordinator instantiation (obtained from
//                  Runtime::coordinator()).
//   MaxTasks     - compile-time task-slot capacity. Zero heap.
//   TickSourceT  - SteadyTickSource (default) or ManualTickSource.
//
// Two drive modes:
//   - Threaded: start() spawns a bare pthread running the collect-release-
//     execute loop; stop() joins it. Idempotent-safe from any thread.
//   - Polled: call poll() directly (ManualTickSource tests, ISR-driven or
//     board-thread-driven embedded use). One poll() is one loop pass.
//
// Expiration action (always): pool alloc signal Event -> coordinator
// submit_from_task(target, e, qos). The coordinator consumes the reference
// on every path (direct / staged / drop), so the timer never calls
// event_gc itself. AO discipline: the timer is an event source, never a
// direct mutator of another AO's state.
//
// Non-copyable, non-movable.
// ---------------------------------------------------------------------------
template <typename PoolT, typename CoordinatorT,
          uint32_t MaxTasks = 16U, typename TickSourceT = SteadyTickSource>
class TimerScheduler final {
public:
    TimerScheduler(PoolT& pool, CoordinatorT& coordinator) noexcept
        : pool_(pool), coordinator_(coordinator)
    {
    }

    ~TimerScheduler() { stop(); }

    TimerScheduler(const TimerScheduler&) = delete;
    TimerScheduler& operator=(const TimerScheduler&) = delete;
    TimerScheduler(TimerScheduler&&) = delete;
    TimerScheduler& operator=(TimerScheduler&&) = delete;

    static constexpr uint32_t capacity() noexcept { return MaxTasks; }

    // -----------------------------------------------------------------------
    // Task management. Thread-safe.
    // -----------------------------------------------------------------------

    // Register a task that submits `signal` to `target` every period_ms.
    // period_ms == 0 -> kInvalidPeriod; no free slot -> kSlotsFull.
    Expected<TimerTaskId, TimerError> schedule_periodic(TargetId target,
                                                        uint16_t signal,
                                                        uint32_t period_ms,
                                                        const EventQos& qos) noexcept
    {
        return schedule(target, signal, period_ms, qos, /*one_shot=*/false);
    }

    // Register a task that submits `signal` to `target` once after delay_ms.
    // The slot auto-frees after firing; a later cancel() returns kNotFound.
    Expected<TimerTaskId, TimerError> schedule_once(TargetId target,
                                                    uint16_t signal,
                                                    uint32_t delay_ms,
                                                    const EventQos& qos) noexcept
    {
        return schedule(target, signal, delay_ms, qos, /*one_shot=*/true);
    }

    // Deactivate a task by id. kNotFound for an unknown / already-fired id.
    Expected<void, TimerError> cancel(TimerTaskId task_id) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint32_t i = 0U; i < MaxTasks; ++i) {
            if (slots_[i].active && slots_[i].id == task_id.value()) {
                slots_[i].active = false;
                return Expected<void, TimerError>::success();
            }
        }
        return Expected<void, TimerError>::error(TimerError::kNotFound);
    }

    // -----------------------------------------------------------------------
    // Drive modes.
    // -----------------------------------------------------------------------

    // Start the background pthread loop. kAlreadyRunning on a second start;
    // kThreadStartFailed if pthread_create fails. With a ManualTickSource
    // this would hot-spin (sleep is a no-op) - use poll() there instead.
    Expected<void, TimerError> start() noexcept
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
            return Expected<void, TimerError>::error(TimerError::kAlreadyRunning);
        }
        if (0 != pthread_create(&thread_, nullptr, &TimerScheduler::trampoline, this)) {
            running_.store(false, std::memory_order_release);
            return Expected<void, TimerError>::error(TimerError::kThreadStartFailed);
        }
        thread_valid_ = true;
        return Expected<void, TimerError>::success();
    }

    // Request stop and join the scheduler thread. Idempotent; safe when the
    // thread was never started. Concurrent stop() calls serialize on the
    // lifecycle mutex so pthread_join is never invoked twice.
    void stop() noexcept
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        running_.store(false, std::memory_order_release);
        if (thread_valid_) {
            pthread_join(thread_, nullptr);
            thread_valid_ = false;
        }
    }

    bool running() const noexcept
    {
        return running_.load(std::memory_order_acquire);
    }

    // One collect-release-execute pass: scan slots, collect expired tasks
    // under the mutex, release it, then submit each expired task's event
    // OUTSIDE the lock. Deterministic drive mode for ManualTickSource (call
    // after advance()); also usable from a board thread instead of start().
    void poll() noexcept { fire_expired(); }

    // -----------------------------------------------------------------------
    // Monitoring hooks (Monitor philosophy: fixed counters, no formatting).
    // -----------------------------------------------------------------------

    // Events successfully allocated and handed to the coordinator pipeline.
    uint32_t fired_count() const noexcept
    {
        return fired_.load(std::memory_order_relaxed);
    }

    // Firings skipped because the pool was exhausted (the task stays
    // scheduled; its next period is unaffected).
    uint32_t skipped_count() const noexcept
    {
        return skipped_.load(std::memory_order_relaxed);
    }

    uint32_t task_count() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t count = 0U;
        for (uint32_t i = 0U; i < MaxTasks; ++i) {
            if (slots_[i].active) {
                ++count;
            }
        }
        return count;
    }

    // Nanoseconds until the next task fires: 0 when a task is already
    // overdue, UINT64_MAX when no task is scheduled. For external event
    // loops / sleep-until-next-task power management.
    uint64_t ns_to_next_task() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t min_remaining = UINT64_MAX;
        const uint64_t now = tick_.now_ns();
        for (uint32_t i = 0U; i < MaxTasks; ++i) {
            if (!slots_[i].active) {
                continue;
            }
            if (slots_[i].next_fire_ns <= now) {
                return 0U;
            }
            const uint64_t remaining = slots_[i].next_fire_ns - now;
            if (remaining < min_remaining) {
                min_remaining = remaining;
            }
        }
        return min_remaining;
    }

    // Direct access to the tick source (tests advance() a ManualTickSource;
    // a board may rebind a hardware-counter SteadyTickSource).
    TickSourceT& tick_source() noexcept { return tick_; }

private:
    struct TaskSlot {
        TargetId target = kInvalidTarget;
        uint16_t signal = 0U;
        EventQos qos{};
        uint64_t period_ns = 0U;
        uint64_t next_fire_ns = 0U;
        uint32_t id = 0U;
        bool active = false;
        bool one_shot = false;
    };

    // Snapshot of one expired task, collected under the mutex.
    struct PendingFire {
        TargetId target = kInvalidTarget;
        uint16_t signal = 0U;
        EventQos qos{};
    };

    static constexpr uint64_t kNsPerMs = 1000000ULL;
    static constexpr uint64_t kIdleSleepNs = 10000000ULL;  // 10 ms when idle
    static constexpr uint64_t kMinSleepNs = 1000000ULL;    // 1 ms floor
    static constexpr uint64_t kMaxSleepNs = 10000000ULL;   // 10 ms ceiling

    Expected<TimerTaskId, TimerError> schedule(TargetId target, uint16_t signal,
                                               uint32_t interval_ms,
                                               const EventQos& qos,
                                               bool one_shot) noexcept
    {
        if (0U == interval_ms) {
            return Expected<TimerTaskId, TimerError>::error(
                TimerError::kInvalidPeriod);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint32_t i = 0U; i < MaxTasks; ++i) {
            if (!slots_[i].active) {
                const uint64_t interval_ns =
                    static_cast<uint64_t>(interval_ms) * kNsPerMs;
                slots_[i].target = target;
                slots_[i].signal = signal;
                slots_[i].qos = qos;
                slots_[i].period_ns = interval_ns;
                slots_[i].next_fire_ns = tick_.now_ns() + interval_ns;
                slots_[i].id = next_id_;
                slots_[i].active = true;
                slots_[i].one_shot = one_shot;
                ++next_id_;
                return Expected<TimerTaskId, TimerError>::success(
                    TimerTaskId(slots_[i].id));
            }
        }
        return Expected<TimerTaskId, TimerError>::error(TimerError::kSlotsFull);
    }

    // Collect-release-execute: expired submissions happen outside mutex_ so
    // the pool/coordinator paths never deadlock against schedule/cancel.
    void fire_expired() noexcept
    {
        PendingFire pending[MaxTasks];
        uint32_t pending_count = 0U;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const uint64_t now = tick_.now_ns();
            for (uint32_t i = 0U; i < MaxTasks; ++i) {
                if (!slots_[i].active) {
                    continue;
                }
                if (now < slots_[i].next_fire_ns) {
                    continue;
                }
                pending[pending_count].target = slots_[i].target;
                pending[pending_count].signal = slots_[i].signal;
                pending[pending_count].qos = slots_[i].qos;
                ++pending_count;

                if (slots_[i].one_shot) {
                    slots_[i].active = false;
                } else {
                    /* Catch-up: advance past every missed period so a late
                       poll fires exactly once, never a burst. */
                    slots_[i].next_fire_ns += slots_[i].period_ns;
                    while (slots_[i].next_fire_ns <= now) {
                        slots_[i].next_fire_ns += slots_[i].period_ns;
                    }
                }
            }
        }
        /* mutex_ released: submissions run outside the lock. */

        for (uint32_t i = 0U; i < pending_count; ++i) {
            Event* e = pool_.alloc(pending[i].signal);
            if (nullptr == e) {
                /* Pool exhausted: skip this firing, keep the task alive. */
                skipped_.fetch_add(1U, std::memory_order_relaxed);
                continue;
            }
            /* The coordinator owns and consumes the reference on every
               disposition (direct / queued / dropped): no event_gc here. */
            (void)coordinator_.submit_from_task(pending[i].target, e,
                                                pending[i].qos);
            fired_.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    void run_loop() noexcept
    {
        while (running_.load(std::memory_order_acquire)) {
            fire_expired();
            uint64_t sleep_ns = ns_to_next_task();
            if (UINT64_MAX == sleep_ns) {
                sleep_ns = kIdleSleepNs;
            }
            if (kMaxSleepNs < sleep_ns) {
                sleep_ns = kMaxSleepNs;
            }
            if (sleep_ns >= kMinSleepNs) {
                tick_.sleep_ns(sleep_ns);
            }
        }
    }

    static void* trampoline(void* arg) noexcept
    {
        static_cast<TimerScheduler*>(arg)->run_loop();
        return nullptr;
    }

    PoolT& pool_;
    CoordinatorT& coordinator_;
    TickSourceT tick_{};

    TaskSlot slots_[MaxTasks]{};
    uint32_t next_id_ = 1U;

    // Guards slots_ / next_id_ only. The lock window is a fixed MaxTasks
    // scan plus one slot write - atomicization is a follow-up optimization.
    mutable std::mutex mutex_;

    std::atomic<bool> running_{false};
    pthread_t thread_{};
    bool thread_valid_ = false;
    // Serializes start()/stop() so pthread_create/join never race.
    std::mutex lifecycle_mutex_;

    std::atomic<uint32_t> fired_{0U};
    std::atomic<uint32_t> skipped_{0U};
};

}  // namespace coact
