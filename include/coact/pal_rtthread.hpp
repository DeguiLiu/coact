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

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

#ifdef COACT_RTT_STUB
#include "test/rtthread_stub.h"
#else
#include <rtthread.h>
#include <rthw.h>

#if defined(RT_USING_SMP) || !defined(RT_CPUS_NR) || (RT_CPUS_NR != 1)
#error "coact::pal::RtThread requires one non-SMP CPU; use a Linux/SMP PAL with HostSmpProfile instead"
#endif

/* SoftIrqOps needs SIGUSR1 — board configs that build without RT_USING_SIGNALS
   may not pull in the signal-header definitions; fall back to RT-Thread's
   standard number (also used by the host stub). */
#ifndef SIGUSR1
#define SIGUSR1 10
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
    ops.counter_bits = static_cast<uint8_t>(sizeof(rt_tick_t) * 8U);
    return ops;
}

namespace detail {

inline constexpr uint64_t exact_ns_per_counter(uint32_t frequency_hz) noexcept
{
    return (frequency_hz != 0U && (1000000000ULL % frequency_hz) == 0U)
        ? (1000000000ULL / frequency_hz)
        : 0U;
}

// Convert a Dispatcher wait timeout to RT-Thread ticks. Zero is the sole
// infinite-wait value; every non-zero timeout remains finite, including one
// whose rounded tick count reaches RT_WAITING_FOREVER.
inline constexpr rt_int32_t dispatcher_wait_ticks(uint32_t timeout_ms) noexcept
{
    if (timeout_ms == 0U) {
        return static_cast<rt_int32_t>(RT_WAITING_FOREVER);
    }
    uint64_t ticks = timeout_ms;
    if constexpr (RT_TICK_PER_SECOND != 1000U) {
        ticks =
            (static_cast<uint64_t>(timeout_ms) * RT_TICK_PER_SECOND + 999U) / 1000U;
    }
    const uint64_t forever = static_cast<uint64_t>(RT_WAITING_FOREVER);
    if (ticks >= forever) {
        return static_cast<rt_int32_t>(forever - 1U);
    }
    return static_cast<rt_int32_t>(ticks);
}

}  // namespace detail

// Fixed per-thread execution-context slot (design §7.5). Does NOT occupy
// RT-Thread's single rt_thread::user_data field.
struct ContextSlot {
    rt_thread_t tid;           // nullptr == free slot
    ExecutionContext ctx;
};

// SoftIrqOps mailbox capacity (the shared ring embedded in SoftIrqHandle).
// 8 slots is a compile-time constant — keep SoftIrqHandle layout deterministic
// and force callers that want a larger queue to chain multiple handles (the
// SoftIrqOps contract is SPSC, not MPSC).
inline constexpr uint32_t kSoftIrqRingSlots = 8U;
// Lock-free discipline (convention item 22): the SPSC ring only ever crosses
// two threads; tag the atomic width so a non-lock-free atomics port fails at
// compile time, not at runtime through libatomic.
static_assert(std::atomic<uint32_t>::is_always_lock_free,
             "SoftIrq ring indices must stay lock-free (no libatomic fallback)");


// ---------------------------------------------------------------------------
// ThreadOps static table (the wake/join precedent extended): one
// WorkerThreadSlot per demo worker thread — static rt_thread TCB, static
// stack, static join semaphore. thread_create() borrows a free slot
// (rt_thread_init, task context only); join() waits on the join sem, then
// frees the slot. ZERO heap: no rt_thread_create anywhere.
// ---------------------------------------------------------------------------
struct WorkerThreadSlot {
    struct rt_thread    thread;    // static worker TCB
    rt_uint8_t*         stack_base;
    uint32_t            stack_bytes;
    struct rt_semaphore join_sem;  // released by worker_thread_entry on exit
    bool                in_use;
    void*               user_ctx;  // ThreadEntry context handed to the trampoline
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
    // ThreadOps static table (may be null/0: PAL without worker threads).
    WorkerThreadSlot*    worker_slots;    // fixed WorkerThreadSlot[K]
    uint16_t             worker_slot_count;
    uint16_t             worker_slot_used;  // high-water mark of borrowed slots
};

// Zero-size placeholder for the WorkerSlots == 0 case (zero-length arrays are
// a GNU extension; std::array<T,0> would still carry the T member).
struct EmptyResource {};

// Caller-provided static resources (design §7.5). Construct in static/global
// storage or on a task stack BEFORE the RtThread that references it. The
// members are value-initialized so a stack/struct resource never hands the PAL
// garbage kernel objects (rt_sem_init reads the host stub's init_done flag).
template <uint32_t StackBytes, uint16_t ContextSlots,
          uint16_t WorkerSlots = 0U, uint32_t WorkerStackBytes = 2048U>
struct RtThreadResources : public RtThreadResourcesBase {
    struct rt_thread    thread;           // static Dispatcher TCB
    struct rt_semaphore wake;             // static wake semaphore
    struct rt_semaphore join;             // static join semaphore
    alignas(RT_ALIGN_SIZE) rt_uint8_t stack[StackBytes];
    ContextSlot slots[ContextSlots];
    // Worker thread table: K static TCBs + K static stacks. WorkerSlots == 0
    // (default) degenerates both members to the empty placeholder.
    typename std::conditional<
        (WorkerSlots > 0U),
        std::array<WorkerThreadSlot, WorkerSlots>,
        EmptyResource>::type worker_threads{};
    typename std::conditional<
        (WorkerSlots > 0U),
        std::array<std::array<rt_uint8_t, WorkerStackBytes>, WorkerSlots>,
        EmptyResource>::type worker_stacks{};

    RtThreadResources() noexcept
        : RtThreadResourcesBase(),
          thread{},
          wake{},
          join{},
          stack{},
          slots{},
          worker_threads{},
          worker_stacks{}
    {
        this->thread_obj   = &this->thread;
        this->wake_sem_obj = &this->wake;
        this->join_sem_obj = &this->join;
        this->stack_base   = this->stack;
        this->stack_bytes  = StackBytes;
        this->slot_table   = this->slots;
        this->slot_count   = ContextSlots;
        if constexpr (WorkerSlots > 0U) {
            this->worker_slots      = this->worker_threads.data();
            this->worker_slot_count = WorkerSlots;
            for (uint16_t i = 0U; i < WorkerSlots; ++i) {
                this->worker_threads[i].stack_base  =
                    reinterpret_cast<rt_uint8_t*>(this->worker_stacks[i].data());
                this->worker_threads[i].stack_bytes = WorkerStackBytes;
                this->worker_threads[i].in_use      = false;
                this->worker_threads[i].user_ctx    = nullptr;
            }
        }
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
    // ---------------------------------------------------------------------------
    // SemOps-family handles (pal.hpp): self-contained static kernel objects.
    // SemHandle EMBEDS the struct rt_semaphore (caller places the handle in
    // static or task-stack storage); rt_sem_init/detach run in task context only.
    // The `pal` back-pointer carries the PAL reference for handle-method calls.
    // ---------------------------------------------------------------------------
    struct SemHandle {
        struct rt_semaphore sem;   // embedded static semaphore (no heap)
        RtThread*           pal;   // set by RtThread::sem_init
    };

    struct MutexHandle {
        struct rt_mutex     mtx;   // embedded static mutex (no heap)
        RtThread*           pal;   // set by RtThread::mutex_init
    };

    // CondHandle: pthread_cond_wait semantics emulated on a counting
    // rt_semaphore — cond_wait releases the paired mutex, blocks, re-acquires
    // (see RtThread::cond_wait). waiters counts blocked threads so broadcast
    // releases exactly that many tokens (rt_sem has no broadcast primitive).
    struct CondHandle {
        struct rt_semaphore sem;   // wake signal (released once per waiter)
        std::atomic<uint32_t> waiters;
        RtThread*           pal;   // set by RtThread::cond_init
    };

    // ThreadOps handle: references the borrowed static WorkerThreadSlot. create()
    // fills pal/slot_idx/entry/context (entry+context are read by the trampoline
    // through the slot's user_ctx, which points back at this handle — the handle
    // is caller storage and join() is mandatory, so it outlives the thread, no
    // heap); join() waits on the slot's join semaphore and frees it.
    struct ThreadHandle {
        RtThread*           pal;
        ThreadEntry         entry;
        void*               context;
        uint16_t            slot_idx;
        bool                valid;
    };

    // SoftIrqHandle (SoftIrqOps family): a fixed-capacity SPSC mailbox
    // (head/tail atomic indices + int32_t ring) embedded in caller storage.
    // The signal (SIGUSR1) is a wake hint only — rt_thread_kill does not
    // carry data on RT-Thread, so the payload is published through the ring
    // before the signal is raised. The handler registered in init() is empty;
    // take() polls the ring on a 1 ms tick (see implementation).
    struct SoftIrqHandle {
        std::atomic<uint32_t> head;   // producer write index (wraps)
        std::atomic<uint32_t> tail;   // consumer read index (wraps)
        int32_t               ring[kSoftIrqRingSlots];
        rt_thread_t           consumer;
        RtThread*             pal;
    };

    // Host-test convenience: references an internal static RtThreadResources.
    // Production boards MUST pass explicit RtThreadResources.
    RtThread() noexcept;

    // Caller-provided static resources. The constructor only saves references;
    // all kernel API calls happen in initialize() (task context). Accepts any
    // RtThreadResources instantiation (worker-slot count is a template default).
    template <typename ResT>
    explicit RtThread(ResT& res) noexcept
        : res_(&res),
          user_entry_(nullptr),
          user_ctx_(nullptr),
          state_(Lifecycle::kUninitialized),
          last_error_(InitError::kOk),
          dispatcher_stack_bytes_(4096U),
          clock_ops_(tick_clock_ops()),
          ns_per_counter_(detail::exact_ns_per_counter(
              static_cast<uint32_t>(RT_TICK_PER_SECOND)))
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
        const rt_thread_t dispatcher = dispatcher_tcb();
        return (dispatcher != nullptr) && (rt_thread_self() == dispatcher);
    }

    /* ---- Monotonic clock ---------------------------------------------- */
    /* ClockOps static function table (design §7.5). Default: rt_tick_get() *
       ns_per_tick; override with set_clock_ops() before use. */
    uint64_t monotonic_ns() const noexcept;
    uint64_t clock_resolution_ns() const noexcept;
    void set_clock_ops(ClockOps ops) noexcept;

    /* ---- Dispatcher wait/signal (0 ms = wait forever) ----------------- */
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

    /* ---- Dispatcher heartbeat (external watchdog support) -------------- */
    /* Same contract as pal_posix.hpp: the Dispatcher records progress once
       per batch-loop iteration under the single-core irq-mask guard;
       external BSP watchdog threads probe the queries. On real boards the
       hardware watchdog (IWDG) stays BSP-owned: the BSP thread stops feeding
       it when dispatcher_alive_within() goes false. window sizing: pal.hpp. */
    void watchdog_progress(uint32_t marker) noexcept;
    uint64_t dispatcher_progress_ns() const noexcept;
    bool dispatcher_alive_within(uint32_t window_ms) const noexcept;

    // Override the Dispatcher thread stack size before initialize()/start().
    // The Runtime pushes Config::kDispatcherStackBytes here; default 4096. A
    // request larger than the RtThreadResources stack yields kStackTooLarge.
    void set_dispatcher_stack_bytes(uint32_t bytes) noexcept;
    uint32_t dispatcher_stack_bytes() const noexcept;

    /* ---- M1 C3: direct-dispatch depth (per-thread, stored in the table) */
    void enter_direct() noexcept;
    void leave_direct() noexcept;

    /* ---- SemOps family (pal.hpp): static rt_semaphore ------------------ */
    /* SemHandle owns its embedded struct rt_semaphore; init (rt_sem_init,
       task context only) / take / release / deinit (rt_sem_detach).
       timeout_ms 0 = non-blocking try, kWaitForever = RT_WAITING_FOREVER,
       any other value converts to ticks (ms at RT_TICK_PER_SECOND). */
    bool sem_init(SemHandle& sem, uint32_t initial) noexcept;
    bool sem_take(SemHandle& sem, uint32_t timeout_ms) noexcept;
    void sem_release(SemHandle& sem) noexcept;
    /* rt_sem_release IS ISR-safe in RT-Thread (and in the host stub). */
    void sem_release_from_isr(SemHandle& sem) noexcept;
    void sem_deinit(SemHandle& sem) noexcept;

    /* ---- MutexOps family: static rt_mutex ------------------------------ */
    bool mutex_init(MutexHandle& m) noexcept;
    void mutex_lock(MutexHandle& m) noexcept;
    void mutex_unlock(MutexHandle& m) noexcept;
    void mutex_deinit(MutexHandle& m) noexcept;

    /* ---- CondOps family: hand-off condition variable ------------------- */
    /* pthread_cond_wait semantics emulated on RT-Thread primitives: the wake
       signal is a counting rt_semaphore; cond_wait RELEASES the paired mutex,
       blocks on the signal, then RE-ACQUIRES the mutex before returning — so
       callers use the exact pthread pattern (lock; while(!cond) wait; unlock;
       signaler: lock; set state; signal; unlock). waiters counts blocked
       threads so broadcast releases exactly that many tokens (rt_sem has no
       broadcast primitive). */
    bool cond_init(CondHandle& c) noexcept;
    /* 0 = RT_WAITING_FOREVER. */
    void cond_wait(CondHandle& c, MutexHandle& m, uint32_t timeout_ms) noexcept;
    void cond_signal(CondHandle& c) noexcept;
    void cond_broadcast(CondHandle& c) noexcept;
    void cond_deinit(CondHandle& c) noexcept;

    /* ---- ThreadOps family: static thread table ------------------------- */
    /* create() borrows the NEXT free slot of the PAL's fixed WorkerThreadSlot
       table (static struct rt_thread + static stack + static join sem, in
       RtThreadResources) — same zero-heap philosophy as the Dispatcher; there
       is NO rt_thread_create. Returns false when the table is exhausted or the
       caller never bound resources with enough slots. join() waits on the
       slot's join semaphore, released by the slot's trampoline on entry
       return, then frees the slot for reuse. */
    bool thread_create(ThreadHandle& t, ThreadEntry entry, void* context) noexcept;
    void thread_join(ThreadHandle& t) noexcept;

    // -- SoftIrqOps family (pal.hpp): SPSC mailbox + rt_thread_kill wake ------
    // See the SoftIrqOps contract in pal.hpp. The signal (SIGUSR1) is a wake
    // hint only — the payload travels in the embedded ring. init() installs
    // an EMPTY handler; raise() publishes to the ring (busy-reject when full)
    // and pokes the consumer with rt_thread_kill; take() polls the ring on a
    // 1 ms tick and returns the payload or -1 on timeout. Board-level
    // verification of the real rt_signal_wait wakeup path is pending.
    bool softirq_init(SoftIrqHandle& h) noexcept;
    bool softirq_raise(SoftIrqHandle& h, int32_t payload) noexcept;
    int32_t softirq_take(SoftIrqHandle& h, uint32_t timeout_ms) noexcept;
    void softirq_deinit(SoftIrqHandle& h) noexcept;

    // -- Sleep (SemOps family companion): block the calling thread ------------
    // rt_thread_mdelay rounds to whole milliseconds (1 kHz tick), which is the
    // real-target resolution for hardware-latency simulation loops.
    void sleep_us(uint32_t us) noexcept;

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
    /* ThreadOps trampoline: runs the user entry, then releases the slot's
       join semaphore and frees the slot. */
    static void worker_thread_entry(void* param) noexcept;

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
    uint64_t read_extended_counter() const noexcept;

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
    uint64_t    ns_per_counter_;          /* zero selects exact fallback */
    /* Dispatcher heartbeat timestamp (ns). 0 = never beat. RT-Thread is a
       single-core PAL, so irq-mask protects the plain 64-bit value from the
       external watchdog reader without a libatomic dependency. */
    uint64_t last_progress_ns_ = 0U;
    mutable uint64_t counter_epoch_ = 0U;
    mutable uint64_t last_counter_ = 0U;
    mutable bool counter_seen_ = false;
};

}  // namespace pal
}  // namespace coact
