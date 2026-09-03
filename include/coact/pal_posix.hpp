// coact POSIX PAL - concrete platform abstraction for Linux / ARM-Linux.
// SPDX-License-Identifier: MIT
//
// Implements the PAL contract (pal.hpp) on top of pthreads and
// clock_gettime(CLOCK_MONOTONIC): a condition-variable dispatcher wait/wake,
// per-thread execution context via thread_local, and the queue-backend
// selection (SMP bounded MPSC). POSIX has no interrupt masking; irq_save /
// irq_restore are documented no-ops. See design 13 and implementation
// contract 4.8.
//
// xxxOps sync-primitive extension (pal.hpp SemOps family): POSIX uses
// pthread primitives directly — a counting semaphore emulated over
// mutex+cond (sem_t would also work, but the mutex+cond form is shared with
// the CondOps implementation and stays clean under -fno-exceptions), plain
// pthread_mutex_t, pthread_cond_t, and pthread_create/join for ThreadOps.
// release_from_isr is the documented host ISR simulation: pthread_cond_signal
// is not async-signal-safe, so callers must be normal threads (the same
// limitation as signal_dispatcher_from_isr, P2-11).
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
    // ---------------------------------------------------------------------------
    // POSIX sync handles (SemOps family). Self-contained values filled in by the
        // PAL's init methods; the `pal` back-pointer lets lambdas and free
        // functions call back into the PAL through the handle.
    // ---------------------------------------------------------------------------
    struct SemHandle {
        pthread_mutex_t mtx;
        pthread_cond_t  cond;
        uint32_t        count;
        Posix*          pal;       // set by Posix::sem_init
    };

    struct MutexHandle {
        pthread_mutex_t mtx;
        Posix*          pal;       // set by Posix::mutex_init
    };

    struct CondHandle {
        pthread_cond_t  cond;
        Posix*          pal;       // set by Posix::cond_init
    };

    struct ThreadHandle {
        pthread_t       tid;
        bool            valid;
        ThreadEntry     entry;     // filled by thread_create; read by the trampoline
        void*           context;
        Posix*          pal;       // set by Posix::thread_create
    };

    // SoftIrqHandle (SoftIrqOps family): the consumer's signalfd fd plus the
    // pthread_t of the installing (consumer) thread. fd defaults to -1 so a
    // not-yet-installed handle cannot accidentally read from fd 0.
    struct SoftIrqHandle {
        int          fd = -1;
        pthread_t    consumer{};
        Posix*       pal = nullptr;
    };

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
    // Host-side ISR simulation only: acquires a pthread mutex and may block.
    // Callers must be normal pthread contexts, not POSIX signal handlers or
    // strict non-blocking ISR paths.
    void signal_dispatcher_from_isr() noexcept;

    void start_dispatcher(ThreadEntry entry, void* context) noexcept;
    void join_dispatcher() noexcept;
    void watchdog_progress(uint32_t marker) noexcept;

    // -- M1 C3 extension: track the calling thread's direct-dispatch depth --
    void enter_direct() noexcept;
    void leave_direct() noexcept;

    // POSIX backend: ready-set bounded MPSC; requires lock-free 64-bit atomics.
    template <typename T, uint16_t Cap>
    using QueueBackend = coact::BoundedMpscQueue<T, Cap>;

    // -- SemOps family (pal.hpp): counting semaphore over mutex + cond -------
    // init/take/release may be called from any task thread; take(0) is a
    // non-blocking try, take(kWaitForever) blocks until a token exists.
    bool sem_init(SemHandle& sem, uint32_t initial) noexcept;
    bool sem_take(SemHandle& sem, uint32_t timeout_ms) noexcept;
    void sem_release(SemHandle& sem) noexcept;
    // Host ISR simulation (pthread cond_signal is not async-signal-safe);
    // identical to sem_release on POSIX.
    void sem_release_from_isr(SemHandle& sem) noexcept;
    void sem_deinit(SemHandle& sem) noexcept;

    // -- MutexOps family (pal.hpp): plain pthread_mutex_t ---------------------
    bool mutex_init(MutexHandle& m) noexcept;
    void mutex_lock(MutexHandle& m) noexcept;
    void mutex_unlock(MutexHandle& m) noexcept;
    void mutex_deinit(MutexHandle& m) noexcept;

    // -- CondOps family (pal.hpp): hand-off condition variable ----------------
    bool cond_init(CondHandle& c) noexcept;
    // timeout_ms 0 = wait forever (the Dispatcher wait convention).
    void cond_wait(CondHandle& c, MutexHandle& m, uint32_t timeout_ms) noexcept;
    void cond_signal(CondHandle& c) noexcept;
    void cond_broadcast(CondHandle& c) noexcept;
    void cond_deinit(CondHandle& c) noexcept;

    // -- ThreadOps family (pal.hpp): pthread create/join ----------------------
    bool thread_create(ThreadHandle& t, ThreadEntry entry, void* context) noexcept;
    void thread_join(ThreadHandle& t) noexcept;

    // -- SoftIrqOps family (pal.hpp): signalfd-backed ISR simulation ---------
    // No signal handler is ever registered (see the SoftIrqOps contract in
    // pal.hpp). init() runs on the consumer thread; raise() may run from any
    // producer thread (it blocks SoftIrqSignal in the producer so the queued
    // signal can only surface through the consumer's signalfd); take() returns
    // the payload or -1 on timeout; deinit() restores the consumer's signal
    // mask. SoftIrqSignal is a fixed Linux real-time number (the kernel range
    // SIGRTMIN..SIGRTMAX is reserved for RT signals that never carry a
    // handler; glibc exposes SIGRTMIN as a runtime call, so we hardcode a
    // documented constant instead). Tests use this symbol to inspect the
    // consumer's restored signal mask.
    static constexpr int SoftIrqSignal = 34;
    bool softirq_init(SoftIrqHandle& h) noexcept;
    bool softirq_raise(SoftIrqHandle& h, int32_t payload) noexcept;
    int32_t softirq_take(SoftIrqHandle& h, uint32_t timeout_ms) noexcept;
    void softirq_deinit(SoftIrqHandle& h) noexcept;

    // -- Sleep (SemOps family companion): block the calling thread ------------
    // microsecond granularity; POSIX uses nanosleep (usleep is obsolete per
    // POSIX.1-2008). Used by the demo workers' hardware-latency simulation.
    void sleep_us(uint32_t us) noexcept;

private:
    static void* dispatcher_entry(void* arg) noexcept;
    static void* thread_trampoline(void* arg) noexcept;

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
