// coact RT-Thread PAL - concrete platform abstraction for RT-Thread 5.1+/5.2+.
// SPDX-License-Identifier: MIT
//
// Implements the PAL contract (pal.hpp) on top of RT-Thread kernel primitives:
//   irq_save/restore  -> rt_hw_interrupt_disable / rt_hw_interrupt_enable
//   monotonic_ns      -> ClockOps static function table (default rt_tick_get();
//                        targets bind a 10 MHz TIM, design §7.5)
//   wait/signal       -> static rt_semaphore (rt_sem_init, ISR-safe release)
//   start_dispatcher  -> static rt_thread (rt_thread_init) + fixed stack array
//   join_dispatcher   -> static rt_semaphore released by dispatcher on exit
//   current_context   -> rt_interrupt_get_nest() + fixed per-thread context
//                        table (keyed by rt_thread identity; no user_data,
//                        no heap allocation)
//
// Static PAL (design §7.5, cmdfw §7): the caller explicitly provides the
// static resources in `RtThreadResources<StackBytes, ContextSlots>` (static
// `struct rt_thread`, an RT_ALIGN_SIZE-aligned Dispatcher stack, two static
// `struct rt_semaphore`, and a fixed `ContextSlot[N]` table). The PAL
// constructor only SAVES REFERENCES and never calls kernel API; explicit
// initialize() runs rt_sem_init + rt_thread_init in task context and returns a
// definite InitError on any failure - no fallback to dynamic create/malloc.
//
// Lifecycle is "one init, one start, one stop"; restart is not supported.
// start_dispatcher() returns InitError and only succeeds after
// rt_thread_startup()==RT_EOK; a second start and a stop-then-start are both
// rejected. The fixed ContextSlot table registers producers inside a short
// irq-mask critical section (dedup + occupy a free slot), is frozen after
// start, and the Dispatcher is identified by comparing rt_thread_self() with
// its own static TCB (so it needs no slot and a full table cannot block it).
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

#if defined(RT_USING_SMP) || !defined(RT_CPUS_NR) || (RT_CPUS_NR != 1)
#error "coact::pal::RtThread requires one non-SMP CPU; use a Linux/SMP PAL with HostSmpProfile instead"
#endif
#endif

#include "coact/config.hpp"
#include "coact/pal.hpp"
#include "coact/pool.hpp"
#include "coact/queue.hpp"

namespace coact {
namespace pal {

// Default ClockOps backed by rt_tick_get() (RT tick precision, design §7.5:
// RT tick is only for Dispatcher blocking waits and long deadlines). Targets
// bind a 10 MHz TIM counter for microsecond budget checks via set_clock_ops().
inline uint64_t rt_tick_counter_read(void* /*ctx*/) noexcept
{
    return static_cast<uint64_t>(rt_tick_get());
}
inline ClockOps tick_clock_ops() noexcept
{
    ClockOps ops;
    ops.read_counter = &rt_tick_counter_read;
    ops.frequency_hz = static_cast<uint32_t>(RT_TICK_PER_SECOND);
    ops.ctx = nullptr;
    return ops;
}

// Fixed per-thread execution-context slot (design §7.5). Does NOT occupy
// RT-Thread's single rt_thread::user_data field.
struct ContextSlot {
    rt_thread_t tid;           // nullptr == free slot
    ExecutionContext ctx;
};

// Resource pointer base: the PAL stores only these references (constructor
// saves references, never calls kernel API). RtThreadResources<...> derives
// and owns the actual static storage.
struct RtThreadResourcesBase {
    struct rt_thread*    thread_obj;      // static Dispatcher TCB
    struct rt_semaphore* wake_sem_obj;    // wake signal (ISR-safe release)
    struct rt_semaphore* join_sem_obj;    // released on Dispatcher exit
    rt_uint8_t*          stack_base;      // Dispatcher stack (RT_ALIGN_SIZE)
    uint32_t             stack_bytes;
    ContextSlot*         slot_table;      // fixed ContextSlot[N]
    uint16_t             slot_count;
};

// Caller-provided static resources (design §7.5). Construct in static/global
// storage or on a task stack BEFORE the RtThread that references it. The
// members are value-initialized so a stack/struct resource never hands the PAL
// garbage kernel objects (rt_sem_init reads the host stub's init_done flag).
template <uint32_t StackBytes, uint16_t ContextSlots>
struct RtThreadResources : public RtThreadResourcesBase {
    struct rt_thread    thread;           // static Dispatcher TCB
    struct rt_semaphore wake;             // static wake semaphore
    struct rt_semaphore join;             // static join semaphore
    alignas(RT_ALIGN_SIZE) rt_uint8_t stack[StackBytes];
    ContextSlot slots[ContextSlots];

    RtThreadResources() noexcept
        : RtThreadResourcesBase(),
          thread{},
          wake{},
          join{},
          stack{},
          slots{}
    {
        this->thread_obj   = &this->thread;
        this->wake_sem_obj = &this->wake;
        this->join_sem_obj = &this->join;
        this->stack_base   = this->stack;
        this->stack_bytes  = StackBytes;
        this->slot_table   = this->slots;
        this->slot_count   = ContextSlots;
    }
};

// Explicit init/start status (design §7.5). Any failure returns a definite
// error - the PAL never falls back to dynamic create/malloc.
enum class InitError : uint8_t {
    kOk = 0,
    kSemInitFailed,        // rt_sem_init failed
    kThreadInitFailed,     // rt_thread_init failed
    kThreadStartFailed,    // rt_thread_startup failed
    kStackTooLarge,        // requested Dispatcher stack > RtThreadResources
    kAlreadyInitialized,   // initialize() after start/stop
    kAlreadyStarted,       // start after start, or after stop (no restart)
};

// ---------------------------------------------------------------------------
// RtThread PAL.
//
// Lifecycle (one init, one start, one stop; no restart):
//   1. Construct RtThread over caller-provided RtThreadResources (the default
//      ctor is a host-test convenience backed by an internal static resource).
//      The constructor only saves references; no kernel API is called.
//   2. Call initialize() in task context (rt_sem_init x2 + rt_thread_init);
//      any failure returns a definite InitError. Idempotent-kOk once
//      initialized; rejected after start/stop.
//   3. Optionally call set_dispatcher_stack_bytes(bytes) and set_clock_ops(ops)
//      BEFORE the first kernel-API use (initialize / register_current_task /
//      start_dispatcher) so the requested Dispatcher stack / clock take effect.
//      Runtime::start() does exactly this: it pushes Config::kDispatcherStackBytes
//      before start_dispatcher() triggers the lazy initialize(). A stack request
//      larger than the RtThreadResources stack returns kStackTooLarge.
//   4. Call register_current_task(prio) from each producer thread that calls
//      submit (fixed ContextSlot table, frozen after start).
//   5. start_dispatcher(entry, ctx) returns kOk only after
//      rt_thread_startup()==RT_EOK; a second start and stop-then-start are
//      rejected with kAlreadyStarted. join_dispatcher() is the single stop.
//
// Static resources are owned by the caller (RtThreadResources); the PAL holds
// only references. No rt_sem_create / rt_thread_create / rt_malloc are used.
// ---------------------------------------------------------------------------
class RtThread
{
public:
    // Host-test convenience: references an internal static RtThreadResources.
    // Production boards MUST pass explicit RtThreadResources.
    RtThread() noexcept;

    // Caller-provided static resources. The constructor only saves references;
    // all kernel API calls happen in initialize() (task context).
    template <uint32_t StackBytes, uint16_t ContextSlots>
    explicit RtThread(RtThreadResources<StackBytes, ContextSlots>& res) noexcept
        : res_(&res),
          user_entry_(nullptr),
          user_ctx_(nullptr),
          state_(Lifecycle::kUninitialized),
          last_error_(InitError::kOk),
          dispatcher_stack_bytes_(4096U),
          clock_ops_(tick_clock_ops())
    {
    }

    /* ---- Interrupt masking -------------------------------------------- */
    CriticalToken irq_save() noexcept;
    void irq_restore(CriticalToken token) noexcept;

    /* ---- Execution context -------------------------------------------- */
    /* Register the calling thread's logical priority (call after initialize(),
       before start). Returns false when the table is full, the tid is already
       registered, or registration is frozen after start. */
    bool register_current_task(LogicalPrio prio) noexcept;

    ExecutionContext current_context() const noexcept;

    // Real thread identity (R1): true only on the coact Dispatcher thread.
    // dispatcher_thread_entry captures its own TCB (the static Dispatcher
    // thread object) and in_dispatcher_thread() compares rt_thread_self()
    // against it. No thread_local: RT-Thread targets may build without TLS, and
    // a thread_local write on a no-TLS ARM target faults at runtime (QEMU
    // data-abort regression). The TCB comparison is non-forgeable (only the
    // Dispatcher thread runs on that TCB) and needs no ContextSlot. Static so
    // cmdfw can bind it as a gate callback without an instance.
    static bool in_dispatcher_thread() noexcept
    {
        return (rt_thread_self() == dispatcher_tcb());
    }

    /* ---- Monotonic clock ---------------------------------------------- */
    /* ClockOps static function table (design §7.5). Default: rt_tick_get() *
       ns_per_tick; override with set_clock_ops() before use. */
    uint64_t monotonic_ns() const noexcept;
    uint64_t clock_resolution_ns() const noexcept;
    void set_clock_ops(ClockOps ops) noexcept;

    /* ---- Dispatcher wait/signal --------------------------------------- */
    void wait_dispatcher(uint32_t timeout_ms) noexcept;
    void signal_dispatcher_from_task() noexcept;
    void signal_dispatcher_from_isr() noexcept;

    /* ---- Dispatcher thread lifecycle ---------------------------------- */
    /* Explicit initialization in task context: rt_sem_init x2 + rt_thread_init.
       Definite InitError on any failure; idempotent-kOk once initialized;
       rejected after start/stop. Auto-invoked lazily by the other entry points
       so the historical host tests keep working. */
    InitError initialize() noexcept;

    /* Start the Dispatcher thread. Returns kOk only after
       rt_thread_startup()==RT_EOK; kAlreadyStarted on a second start or after
       stop. Auto-initializes if initialize() was not called. */
    InitError start_dispatcher(ThreadEntry entry, void* context) noexcept;
    void join_dispatcher() noexcept;
    void watchdog_progress(uint32_t marker) noexcept;

    // Override the Dispatcher thread stack size before initialize()/start().
    // The Runtime pushes Config::kDispatcherStackBytes here; default 4096. A
    // request larger than the RtThreadResources stack yields kStackTooLarge.
    void set_dispatcher_stack_bytes(uint32_t bytes) noexcept;
    uint32_t dispatcher_stack_bytes() const noexcept;

    /* ---- M1 C3: direct-dispatch depth (per-thread, stored in the table) */
    void enter_direct() noexcept;
    void leave_direct() noexcept;

    /* ---- Queue backend (single-core irq-mask ring, no atomics) --------- */
    template <typename T, uint16_t Cap>
    using QueueBackend = coact::SingleCoreCriticalRing<T, Cap>;

    // Board profile for single-core RT-Thread product assembly (design §7.4):
    // Runtime<BoardCfg, RtThread, RtThread::Profile> selects immediate reclaim.
    // Compile-time contract: the single-core profile REQUIRES RT_CPUS_NR==1 and
    // no RT_USING_SMP. The target include gate above enforces this; Linux/SMP
    // assemblies use their own PAL with HostSmpProfile instead.
    using Profile = coact::RttSingleCoreProfile;

private:
    enum class Lifecycle : uint8_t {
        kUninitialized,
        kReady,        // initialize() succeeded; start allowed
        kStarted,      // dispatcher running; stop/join allowed
        kStopped,      // joined; terminal - no restart
        kInitFailed,   // initialize() or start failed; terminal - no retry
    };

    static void dispatcher_thread_entry(void* param) noexcept;

    /* Runs initialize() once and caches the result. Returns last_error_ when
       the PAL is in the failed state. */
    InitError ensure_initialized() noexcept;

    /* TCB captured by dispatcher_thread_entry on the Dispatcher thread
       (design §7.5). Defined in pal_rtthread.cpp. */
    static rt_thread_t dispatcher_tcb() noexcept;

    /* Fixed ContextSlot-table lookups over the resource's slot array. Never
       allocates. */
    static ContextSlot* find_slot(RtThreadResourcesBase* res,
                                  rt_thread_t t) noexcept;
    static ContextSlot* alloc_slot(RtThreadResourcesBase* res,
                                   rt_thread_t t) noexcept;

    /* Returns a pointer to the current thread's ExecutionContext in the fixed
       table. Null for ISR, the Dispatcher (TCB-identified), or unregistered
       threads. Never allocates. */
    ExecutionContext* tls_ctx() const noexcept;

    RtThreadResourcesBase* res_;         /* caller-provided static resources */
    ThreadEntry user_entry_;
    void*       user_ctx_;
    Lifecycle   state_;
    InitError   last_error_;
    uint32_t    dispatcher_stack_bytes_; /* default 4096; overridable */
    ClockOps    clock_ops_;              /* default: RT tick; §7.5 */
};

}  // namespace pal
}  // namespace coact
