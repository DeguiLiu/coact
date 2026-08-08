// coact RT-Thread PAL - concrete platform abstraction for RT-Thread 5.1+/5.2+.
// SPDX-License-Identifier: MIT
//
// Implements the PAL contract (pal.hpp) on top of RT-Thread kernel primitives:
//   irq_save/restore  -> rt_hw_interrupt_disable / rt_hw_interrupt_enable
//   monotonic_ns      -> rt_tick_get() * ns_per_tick (1-10ms precision)
//                        Override by subclassing for rdtime/rdcycle accuracy.
//   wait/signal       -> rt_sem (binary semaphore, ISR-safe release)
//   start_dispatcher  -> rt_thread_create + rt_thread_startup
//   join_dispatcher   -> rt_sem released by dispatcher on exit
//   current_context   -> rt_interrupt_get_nest() + rt_thread user_data
//
// Host testing: build with -DCOACT_RTT_STUB to pull in tests/rtthread_stub.h,
// which backs RT-Thread API with pthreads so tests run on Linux without a BSP.
//
// See design 13 and implementation contract 4.8.
#pragma once

#ifndef __cplusplus
#error "coact/pal_rtthread.hpp requires C++"
#endif

#include <cstdint>

#ifdef COACT_RTT_STUB
#include "test/rtthread_stub.h"
#else
#include <rtthread.h>
#include <rthw.h>
#endif

#include "coact/config.hpp"
#include "coact/pal.hpp"
#include "coact/queue.hpp"

namespace coact {
namespace pal {

// ---------------------------------------------------------------------------
// RtThread PAL.
//
// Lifecycle:
//   1. Construct RtThread (statically or on stack before init).
//   2. Optionally call set_cpu_freq(hz) before start() for higher-resolution
//      monotonic_ns (not used in default tick-based implementation).
//   3. Call register_current_task(prio) from each user thread that calls submit.
//   4. Use Runtime<DefaultConfig, pal::RtThread> for framework init.
//
// Semaphores are allocated via rt_sem_create during construction (one-time
// init allocation; not on the hot path).
// ---------------------------------------------------------------------------
class RtThread
{
public:
    explicit RtThread(uint32_t cpu_freq_hz = 0U) noexcept;

    /* ---- Interrupt masking -------------------------------------------- */
    CriticalToken irq_save() noexcept;
    void irq_restore(CriticalToken token) noexcept;

    /* ---- Execution context -------------------------------------------- */
    /* Register the calling thread's logical priority (call before submit). */
    bool register_current_task(LogicalPrio prio) noexcept;

    ExecutionContext current_context() const noexcept;

    /* ---- Monotonic clock ---------------------------------------------- */
    /* Default: rt_tick_get() * ns_per_tick (1ms precision at 1kHz tick). */
    uint64_t monotonic_ns() const noexcept;
    uint64_t clock_resolution_ns() const noexcept;

    /* ---- Dispatcher wait/signal --------------------------------------- */
    void wait_dispatcher(uint32_t timeout_ms) noexcept;
    void signal_dispatcher_from_task() noexcept;
    void signal_dispatcher_from_isr() noexcept;

    /* ---- Dispatcher thread lifecycle ---------------------------------- */
    void start_dispatcher(ThreadEntry entry, void* context) noexcept;
    void join_dispatcher() noexcept;
    void watchdog_progress(uint32_t marker) noexcept;

    /* ---- M1 C3: direct-dispatch depth (per-thread, stored in user_data) */
    void enter_direct() noexcept;
    void leave_direct() noexcept;

    /* ---- Queue backend (MPSC, lock-free, SMP-safe) -------------------- */
    template <typename T, uint16_t Cap>
    using QueueBackend = coact::BoundedMpscQueue<T, Cap>;

private:
    static void dispatcher_thread_entry(void* param) noexcept;

    /* Returns a pointer to the per-thread ExecutionContext stored in
       rt_thread::user_data. Null for ISR or unregistered threads. */
    static ExecutionContext* tls_ctx() noexcept;

    /* Wake semaphore (binary: 0 or 1). */
    rt_sem_t wake_sem_;
    /* Join semaphore: released when the dispatcher thread function returns. */
    rt_sem_t join_sem_;
    /* Handle to the created dispatcher thread (for diagnostics). */
    rt_thread_t dispatcher_thread_;

    ThreadEntry user_entry_;
    void*       user_ctx_;

    bool     thread_started_;
    uint32_t ns_per_tick_;   /* 1e9 / RT_TICK_PER_SECOND */
};

}  // namespace pal
}  // namespace coact
