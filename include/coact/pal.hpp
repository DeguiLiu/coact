// coact Platform Abstraction Layer contract.
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>

#include "coact/config.hpp"

namespace coact {

// ---------------------------------------------------------------------------
// CriticalSection: platform interrupt-critical-section hook injected into the
// single-core pool / queue backends. save() masks interrupts and returns an
// opaque token; restore(token) unmasks them again. Host tests inject no-op
// hooks; RT-Thread 5.2.x maps save/restore to rt_hw_interrupt_disable/enable so
// a single-core 100 MHz MCU guards the pool head RMW in O(1), no libatomic.
// Per contract 4.3 the function pointers carry no noexcept qualifier.
// ---------------------------------------------------------------------------
struct CriticalSection {
    using Token = uintptr_t;
    void* ctx;                             // opaque PAL context (passed to hooks)
    Token (*save)(void* ctx);
    void (*restore)(void* ctx, Token);
};

// Unified RAII guard for a CriticalSection: save() on construction (a null
// save hook degrades to no-op), restore() on destruction (a null restore hook
// is a no-op). Factored into one canonical place so consumers no longer each
// hand-roll their own `CriticalSectionScope` (previously duplicated across the
// cmdfw delivery_runtime and response paths with inconsistent null-tolerance).
// Value-initialized token field makes a null save leave the token in a safe
// state. Trivial inline, zero heap, no exceptions.
class CriticalSectionGuard {
public:
    explicit CriticalSectionGuard(const CriticalSection& cs) noexcept
        : cs_(cs),
          token_((cs_.save != nullptr) ? cs_.save(cs_.ctx)
                                       : CriticalSection::Token{})
    {
    }
    CriticalSectionGuard(const CriticalSectionGuard&) = delete;
    CriticalSectionGuard& operator=(const CriticalSectionGuard&) = delete;
    ~CriticalSectionGuard() noexcept
    {
        if (cs_.restore != nullptr) {
            cs_.restore(cs_.ctx, token_);
        }
    }

private:
    CriticalSection cs_;
    CriticalSection::Token token_;
};

namespace pal {
struct CriticalToken {
    uintptr_t value;
};

// High-resolution monotonic counter injected into a PAL as a static function
// table (design §7.5): no inheritance / virtual dispatch. read_counter()
// returns raw counter ticks; frequency_hz converts them to nanoseconds.
// counter_bits declares a wrapping hardware width (64 means already extended).
// RT tick remains the source for Dispatcher blocking waits and long deadlines.
struct ClockOps {
    uint64_t (*read_counter)(void* ctx);
    uint32_t frequency_hz;
    void* ctx;
    uint8_t counter_bits = 64U;
};
}  // namespace pal

// Build a CriticalSection from any PAL that exposes irq_save()/irq_restore()
// (e.g. pal::RtThread -> rt_hw_interrupt_disable/enable on RT-Thread 5.2.x,
// pal::Posix -> no-op on host). The PAL pointer travels as the CS ctx (the
// hooks are capture-less, so they convert to plain function pointers).
// Pass the result to EventPool::init and the SingleCoreCriticalRing staging
// backend so single-core targets guard the pool / queue head RMW in O(1)
// without libatomic.
template <typename PalT>
inline CriticalSection make_critical_section(PalT& pal) noexcept
{
    CriticalSection cs;
    cs.ctx     = static_cast<void*>(&pal);
    cs.save    = [](void* ctx) -> CriticalSection::Token {
        return static_cast<PalT*>(ctx)->irq_save().value;
    };
    cs.restore = [](void* ctx, CriticalSection::Token v) {
        pal::CriticalToken tok;
        tok.value = v;
        static_cast<PalT*>(ctx)->irq_restore(tok);
    };
    return cs;
}

// A CriticalSection that actually serializes on SMP hosts. The POSIX PAL's
// irq_save() is a documented no-op (see design 13) — its pools/queues rely on
// the 32-bit head CAS alone. That is safe for the free-list HEAD, but the
// batched reclaim (ReclaimBatcher) writes each block's `next` field OUTSIDE
// the head CAS while chaining blocks, which races a concurrent alloc's
// load_next on the same block. For any pool shared between an allocating
// thread and a reclaiming thread on SMP, inject THIS critical section instead
// of make_critical_section(pal): a short per-pool spinlock held across the
// alloc / reclaim / batch-splice operations so their head + next-field writes
// serialize. Single-core targets keep make_critical_section (irq mask), which
// is O(1) and unaffected by the race (alloc and reclaim are never concurrent
// on one core).
struct SpinCriticalSection {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
};
inline CriticalSection make_spin_critical_section(SpinCriticalSection& scs) noexcept
{
    CriticalSection cs;
    cs.ctx = static_cast<void*>(&scs);
    cs.save = [](void* ctx) -> CriticalSection::Token {
        auto* s = static_cast<SpinCriticalSection*>(ctx);
        // Acquire: spin until the flag was clear. Token 1 marks "held".
        while (s->flag.test_and_set(std::memory_order_acquire)) {
        }
        return 1U;
    };
    cs.restore = [](void* ctx, CriticalSection::Token) {
        auto* s = static_cast<SpinCriticalSection*>(ctx);
        s->flag.clear(std::memory_order_release);
    };
    return cs;
}

namespace pal {

using ThreadEntry = void (*)(void* context);

// Concrete PAL types (Posix, RtThread) provide these members; they are not
// required to inherit from any base (concept-checked via the Runtime template).
// Per design 14.2, callback function pointers carry no noexcept qualifier.
//
//   CriticalToken irq_save() noexcept;
//   void irq_restore(CriticalToken token) noexcept;
//   ExecutionContext current_context() const noexcept;
//   uint64_t monotonic_ns() const noexcept;
//   uint64_t clock_resolution_ns() const noexcept;
//   void wait_dispatcher(uint32_t timeout_ms) noexcept;
//   void signal_dispatcher_from_task() noexcept;
//   void signal_dispatcher_from_isr() noexcept;
//   void start_dispatcher(ThreadEntry entry, void* context) noexcept;
//       // RtThread additionally returns pal::InitError (design §7.5): kOk only
//       // after rt_thread_startup()==RT_EOK; kAlreadyStarted after a second
//       // start or after stop. Posix keeps void.
//   void join_dispatcher() noexcept;
//   void watchdog_progress(uint32_t marker) noexcept;
//   uint64_t dispatcher_progress_ns() const noexcept;
//   bool dispatcher_alive_within(uint32_t window_ms) const noexcept;
//       // Dispatcher heartbeat for external watchdog detection of a blocked
//       // handler. watchdog_progress() is the single writer (the Dispatcher
//       // thread, once per batch-loop iteration); dispatcher_progress_ns() /
//       // dispatcher_alive_within() are the external-reader queries. A PAL
//       // that never beat reports progress 0 and alive_within()==false ("not
//       // proven alive"). No heartbeat exists MID-BATCH by design: the loop
//       // only beats between RTC steps, so a handler that blocks forever
//       // stops the beat and is thus detectable. Window sizing:
//       //   window_ms >= kBatchSizeMax * max(RTC budget ms) + kBatchTimeoutMs
//       //             + margin
//       // (default config ~ 8*1ms + 5ms + headroom; the beat is the batch-loop
//       // period, not a fixed timer). RTT targets: hardware watchdog (IWDG)
//       // stays BSP-owned - a BSP thread feeds it only while
//       // dispatcher_alive_within() holds, else stops feeding to reset.
//   void set_dispatcher_stack_bytes(uint32_t bytes) noexcept;   // may be no-op
//   void set_clock_ops(ClockOps ops) noexcept;                  // optional (§7.5)
//
// Sync-primitive extension (SemOps family, see below): a concrete PAL also
// provides sem_init/sem_take/sem_release/sem_release_from_isr/sem_deinit,
// mutex_init/lock/unlock/deinit, cond_init/wait/signal/broadcast/deinit and
// thread_create/thread_join so examples with worker threads (isp_pipeline)
// run unmodified on Linux host and RT-Thread targets. The handle types
// (SemHandle / MutexHandle / CondHandle / ThreadHandle) are per-PAL; only the
// method names are contract. The SoftIrqOps family (below) extends the same
// contract with software-interrupt simulation for the ISR completion path.

// ---------------------------------------------------------------------------
// Sync-primitive policy family (design §7.5): PAL strategy interfaces carry
// the xxxOps suffix (ClockOps precedent) and are static function tables or
// concrete-PAL method sets resolved at COMPILE time (Runtime<Config, Pal>
// template parameter) — never a runtime if.
//
// SemOps contract (counting semaphore, static allocation only):
//   init(handle, initial_count)   task context only (RT-Thread rt_sem_init is
//                                 NOT ISR-safe; both platforms)
//   take(handle, timeout_ms)      0 = non-blocking try, kWaitForever = block
//                                 forever; returns false on timeout/empty
//   release(handle)               task context
//   release_from_isr(handle)      ISR-safe: RT-Thread rt_sem_release IS
//                                 ISR-safe; POSIX host simulates ISR with a
//                                 normal thread (documented limitation —
//                                 pthread_cond_signal is not async-signal-safe)
//   deinit(handle)                task context
//
// MutexOps contract (binary, may be priority-inheriting on target):
//   init / lock / unlock / deinit
//   Priority-inversion notes: RT-Thread rt_mutex HAS priority inheritance
//   built in (the kernel propagates the holder's priority through
//   _mutex_update_priority) and every RtThread PAL ipc_init uses
//   RT_IPC_FLAG_PRIO wake ordering; POSIX default pthread_mutex has NO PI.
//   Callers must not rely on PI semantics being present: on Linux the PAL
//   mutex is a plain normal mutex. If a target path depends on bounded
//   priority inversion, that path must stay on the RT-Thread PAL only.
//
// CondOps contract (hand-off condition variable, always paired with a
// MutexOps mutex):
//   init / wait(handle, mutex, timeout_ms) / signal / broadcast / deinit
//   wait(timeout 0) = block until signaled (pthread_cond_wait /
//   RT_WAITING_FOREVER on rt_sem — the "0 means forever" convention the
//   Dispatcher wait already uses).
//
// ThreadOps contract (create/join, static allocation philosophy):
//   create(handle, entry, context)  Posix: pthread_create. RtThread: static
//                                   thread table (rt_thread_init over caller
//                                   storage, no rt_thread_create/heap).
//   join(handle)                    block until the thread entry returns.
//
// SoftIrqOps contract (software interrupt simulation, the ISR -> completion
// event path made explicit):
//   init(handle)                     consumer side, task context. Linux: block
//                                    SIGRTMIN via pthread_sigmask, then
//                                    signalfd(-1, mask) so no handler ever
//                                    runs; the installing thread is the
//                                    consumer. RT-Thread: install an empty
//                                    handler for SIGUSR1 + a shared payload
//                                    mailbox (the handler itself carries no
//                                    data — see KEY DESIGN POINT below).
//   raise(handle, payload)           producer side (worker thread standing
//                                    in for the ISR). Linux: sigqueue(pid,
//                                    SIGRTMIN, {.sival_int = payload}) — the
//                                    producer blocks SIGRTMIN in itself first
//                                    so the queued signal can only surface
//                                    through the consumer's signalfd.
//                                    RT-Thread: push payload into the shared
//                                    fixed ring, then rt_thread_kill(consumer,
//                                    SIGUSR1) as a wake hint only.
//   take(handle, timeout_ms)         consumer side. Linux: poll(fd) + read
//                                    signalfd_siginfo -> ssi_int; RT-Thread:
//                                    poll the shared mailbox. timeout 0 ==
//                                    kWaitForever (wait forever convention);
//                                    returns -1 on timeout.
//   deinit(handle)                   close fd / cleanup; restores the consumer
//                                    thread's signal mask on Linux.
//
// KEY DESIGN POINT: on Linux NO signal handler is ever registered. POSIX
// signal handlers are constrained to async-signal-safe functions (no malloc,
// no locks), which would poison the consumer path the moment it needs to
// observe other workers. signalfd sidesteps that by converting the signal
// into an fd event: the signal stays blocked on every thread, and
// consumption happens entirely in ordinary thread context where malloc /
// locks are allowed. This is the essential advantage over the
// pthread_kill+handler scheme. The RT-Thread port keeps the payload out of
// the handler (the handler is empty; the shared fixed ring carries data) so
// the handler only ever runs trivial code — board-level blocking delivery
// (rt_signal_wait wakeups) is future work.
// ---------------------------------------------------------------------------

// Blocking-wait sentinel for the xxxOps take/wait timeouts. Distinct from the
// SemOps non-blocking value 0 so a caller cannot conflate "try once" with
// "wait forever" (the Dispatcher's wait_dispatcher uses raw 0 = forever; that
// legacy contract is unchanged — these Ops use the explicit constant).
constexpr uint32_t kWaitForever = 0xFFFFFFFFU;

// Semaphore/mutex/condvar/thread handles: the concrete TYPE is defined by
// each PAL header (pal_posix.hpp / pal_rtthread.hpp) — POSIX embeds pthread
// objects, RT-Thread embeds static rt_semaphore / rt_mutex / a slot
// reference. Only the METHOD names (init/take/release/...) are contract here.
// (The handle types are therefore NOT forward-declared in this header: the
// concrete definitions live in the concrete PAL headers.)

// Thread entry signature shared with the Dispatcher (pal::ThreadEntry).
// ThreadHandle is likewise concrete per-PAL (pthread_t vs static slot idx).

}  // namespace pal
}  // namespace coact
