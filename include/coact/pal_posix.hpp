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

    uint64_t monotonic_ns() const noexcept;
    uint64_t clock_resolution_ns() const noexcept;

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
    static thread_local ExecutionContext tls_ctx_;
};

}  // namespace pal
}  // namespace coact
