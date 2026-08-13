// coact POSIX PAL - concrete platform abstraction for Linux / ARM-Linux.
// SPDX-License-Identifier: MIT
//
// Implements the PAL contract (pal.hpp) on top of pthreads and
// clock_gettime(CLOCK_MONOTONIC): a condition-variable dispatcher wait/wake,
// per-thread execution context via thread_local, and the queue-backend
// selection (SMP bounded MPSC). POSIX has no interrupt masking; irq_save /
// irq_restore are documented no-ops. See design 13 and implementation
// contract 4.8.
#pragma once

#include <cstddef>
#include <cstdint>
#include <pthread.h>

#include "coact/config.hpp"
#include "coact/pal.hpp"
#include "coact/queue.hpp"

namespace coact {
namespace pal {

class Posix {
public:
    Posix() noexcept;

    // -- Interrupt masking: no-op on POSIX (tokens are opaque) --
    CriticalToken irq_save() noexcept;
    void irq_restore(CriticalToken token) noexcept;

    // Register the calling thread's logical priority for the C2 admission
    // gate. Returns false for invalid priorities.
    bool register_current_task(LogicalPrio prio) noexcept;

    // Execution context of the current thread (Task, or Dispatcher inside the
    // dispatcher thread). direct_depth tracks nested direct dispatches.
    ExecutionContext current_context() const noexcept;

    // Real thread identity: true only on the coact Dispatcher thread. Backed by
    // a thread_local set solely in dispatcher_entry(), so no other thread can
    // forge it (an RAII "dispatch guard" on a non-Dispatcher thread still sees
    // false). Static so cmdfw can bind it as a gate callback without an
    // instance.
    static bool in_dispatcher_thread() noexcept
    {
        return ContextKind::Dispatcher == tls_ctx_.kind;
    }

    uint64_t monotonic_ns() const noexcept;
    uint64_t clock_resolution_ns() const noexcept;

    // Host-test hook: simulate an RT-Thread-style coarse tick. When hz != 0,
    // monotonic_ns() is quantized to 1/hz s (e.g. 100 Hz -> 10 ms), matching a
    // tick-based PAL. Default 0 = no quantization (ns resolution).
    void set_tick_hz(uint32_t hz) noexcept;

    // No-op on POSIX (pthread uses the default 8 MiB stack); kept so the
    // Runtime can push Config::kDispatcherStackBytes to any PAL uniformly.
    void set_dispatcher_stack_bytes(uint32_t bytes) noexcept;

    // Block up to timeout_ms for a dispatcher signal (0 = wait forever).
    void wait_dispatcher(uint32_t timeout_ms) noexcept;
    void signal_dispatcher_from_task() noexcept;
    void signal_dispatcher_from_isr() noexcept;

    void start_dispatcher(ThreadEntry entry, void* context) noexcept;
    void join_dispatcher() noexcept;
    void watchdog_progress(uint32_t marker) noexcept;

    // -- M1 C3 extension: track the calling thread's direct-dispatch depth --
    void enter_direct() noexcept;
    void leave_direct() noexcept;

    // SMP POSIX backend: lock-free bounded multi-producer single-consumer.
    template <typename T, uint16_t Cap>
    using QueueBackend = coact::BoundedMpscQueue<T, Cap>;

private:
    static void* dispatcher_entry(void* arg) noexcept;

    pthread_mutex_t mutex_;
    pthread_cond_t cond_;
    bool wake_;
    bool thread_valid_;
    pthread_t thread_;
    ThreadEntry user_entry_;
    void* user_ctx_;
    uint32_t tick_hz_;        /* 0 = ns resolution (host native) */
    uint64_t ns_per_tick_;    /* 1e9 / tick_hz_, valid when tick_hz_ != 0 */
    static thread_local ExecutionContext tls_ctx_;
};

}  // namespace pal
}  // namespace coact
