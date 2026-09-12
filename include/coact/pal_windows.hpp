// coact Windows PAL - concrete platform abstraction for Windows hosts.
// SPDX-License-Identifier: MIT
//
// Implements the PAL contract (pal.hpp) on top of Win32 primitives:
//   - auto-reset CreateEventW for the Dispatcher sleep/wake
//     (wait_dispatcher / signal_dispatcher_from_task/isr)
//   - QueryPerformanceCounter monotonic clock (monotonic_ns /
//     clock_resolution_ns)
//   - _beginthreadex for the Dispatcher thread, with an explicit started
//     handshake (start/join_dispatcher)
//   - thread_local Dispatcher identity (in_dispatcher_thread); interrupt
//     masking is a documented no-op on a Windows host, matching the POSIX PAL.
//
// Irq masking (irq_save / irq_restore) is a no-op: on an SMP host the shared
// EventPool must be bound to a real coact::SpinCriticalSection
// (make_spin_critical_section), NOT to make_critical_section(pal). See the P0
// spinlock note in the xcom_core integration.
//
// xxxOps sync-primitive extension (pal.hpp SemOps family): native Win32
// primitives, one per family —
//   SemOps    : CreateSemaphoreW + WaitForSingleObject (millisecond timeout
//               maps directly: 0 = non-blocking try, kWaitForever = INFINITE)
//   MutexOps  : CRITICAL_SECTION
//   CondOps   : CONDITION_VARIABLE paired with the CRITICAL_SECTION
//   ThreadOps : _beginthreadex + WaitForSingleObject
//   SoftIrqOps: CreateEventW wake hint + a fixed 8-slot SPSC payload ring
//               (Windows has no signalfd, and an event carries no payload).
//   sleep_us  : Sleep() for whole milliseconds + a QPC busy-wait remainder.
// See design 13 and implementation contract 4.8.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>

#include "coact/config.hpp"
#include "coact/pal.hpp"
#include "coact/queue.hpp"

namespace coact {
namespace pal {

// ---------------------------------------------------------------------------
// WakeEvent - a move-only, null-safe auto-reset Win32 event wrapper.
//
// Caller-storage rendezvous helper (e.g. an app-layer session writer or a
// capacity waiter). CreateEventW runs in the constructor (failure leaves the
// handle null so valid() is false); signal() / wait() are both null-safe.
// Kept as a bare HANDLE with explicit CloseHandle / hand-written move semantics
// so the PAL stays free of any application-layer RAII dependency (coact must
// not depend on the consuming app's foundation headers).
// ---------------------------------------------------------------------------
class WakeEvent {
public:
    WakeEvent() noexcept;               // CreateEventW auto-reset, null on failure
    ~WakeEvent() noexcept;              // CloseHandle
    WakeEvent(const WakeEvent&) = delete;
    WakeEvent& operator=(const WakeEvent&) = delete;
    WakeEvent(WakeEvent&& other) noexcept;
    WakeEvent& operator=(WakeEvent&& other) noexcept;

    bool valid() const noexcept;        // true iff the handle was created
    void signal() noexcept;             // SetEvent (null-safe)
    bool wait(uint32_t ms) noexcept;    // WaitForSingleObject; true == signaled
                                        // (0 = wait forever, PAL convention)

private:
    HANDLE handle_;
};

class Windows {
public:
    // SoftIrqOps mailbox capacity (the fixed SPSC ring embedded in
    // SoftIrqHandle). A class member rather than a namespace-scope constant so
    // a translation unit that also includes pal_rtthread.hpp does not collide
    // with kSoftIrqRingSlots. Keep the layout deterministic; callers wanting a
    // larger queue chain multiple handles (the SoftIrqOps contract is SPSC).
    static constexpr uint32_t kSoftIrqRingSlots = 8U;
    // Lock-free discipline: the SPSC ring only ever crosses two threads; tag
    // the atomic width so a non-lock-free atomics port fails at compile time,
    // not at runtime through a fallback.
    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "SoftIrq ring indices must stay lock-free");

    // -----------------------------------------------------------------------
    // Windows sync handles. Self-contained values filled in by the PAL's init
    // methods; the `pal` back-pointer lets lambdas and free functions call
    // back into the PAL through the handle (same shape as pal::Posix).
    // -----------------------------------------------------------------------
    struct SemHandle {
        HANDLE   sem;   // CreateSemaphoreW (counting, max LONG_MAX)
        Windows* pal;   // set by Windows::sem_init
    };

    struct MutexHandle {
        CRITICAL_SECTION cs;    // InitializeCriticalSectionAndSpinCount
        Windows*         pal;   // set by Windows::mutex_init
    };

    struct CondHandle {
        CONDITION_VARIABLE cv;  // InitializeConditionVariable
        Windows*           pal; // set by Windows::cond_init
    };

    struct ThreadHandle {
        HANDLE      thread;     // _beginthreadex handle, closed by thread_join
        bool        valid;
        ThreadEntry entry;      // filled by thread_create; read by the trampoline
        void*       context;
        Windows*    pal;        // set by Windows::thread_create
    };

    // SoftIrqHandle (SoftIrqOps family): a fixed-capacity SPSC mailbox plus an
    // auto-reset event used purely as a wake hint — a Win32 event cannot carry
    // the int32_t payload, so the payload travels in the ring (exactly the
    // RT-Thread port's design). init() runs on the consumer thread; raise()
    // publishes then SetEvent(); take() drains FIFO and returns -1 on timeout.
    struct SoftIrqHandle {
        HANDLE                event = nullptr;  // CreateEventW (auto-reset)
        std::atomic<uint32_t> head{0};          // producer write index (wraps)
        std::atomic<uint32_t> tail{0};          // consumer read index (wraps)
        int32_t               ring[kSoftIrqRingSlots];
        Windows*              pal = nullptr;    // set by Windows::softirq_init
    };

    Windows() noexcept;
    ~Windows() noexcept;   // join_dispatcher + close the wake/started events

    // A PAL owns raw kernel HANDLEs (wake/started events and, while a
    // Dispatcher is live, its thread handle). Copying one would produce two
    // owners for the same handle, and the second CloseHandle to run would
    // close whatever the OS had since handed out under that value. The type is
    // a per-process singleton in practice, so deleting the copy operations
    // costs nothing and turns that into a compile error.
    Windows(const Windows&) = delete;
    Windows& operator=(const Windows&) = delete;

    // -- Interrupt masking: no-op on a Windows SMP host (tokens opaque) ------
    CriticalToken irq_save() noexcept;
    void irq_restore(CriticalToken token) noexcept;

    // Register the calling thread's logical priority for the C2 admission
    // gate. Returns false for invalid priorities.
    bool register_current_task(LogicalPrio prio) noexcept;

    ExecutionContext current_context() const noexcept;

    // Real thread identity: true only on the coact Dispatcher thread. Backed by
    // the thread_local context set in dispatcher_entry(). Static so the
    // Dispatcher can bind it as a gate callback without an instance.
    static bool in_dispatcher_thread() noexcept;

    uint64_t monotonic_ns() const noexcept;
    uint64_t clock_resolution_ns() const noexcept;

    // Host-test hook: simulate an RT-Thread-style coarse tick. When hz != 0,
    // monotonic_ns() is quantized to 1/hz s (e.g. 100 Hz -> 10 ms), matching a
    // tick-based PAL. Default 0 = no quantization (QPC resolution).
    void set_tick_hz(uint32_t hz) noexcept;

    // No-op: Windows uses the default thread stack; kept so the Runtime can
    // push Config::kDispatcherStackBytes to any PAL uniformly.
    void set_dispatcher_stack_bytes(uint32_t bytes) noexcept;

    // Block up to timeout_ms for a dispatcher signal (0 = wait forever).
    void wait_dispatcher(uint32_t timeout_ms) noexcept;
    void signal_dispatcher_from_task() noexcept;
    // Non-blocking SetEvent (callers may be real callback/ISR threads).
    void signal_dispatcher_from_isr() noexcept;

    [[nodiscard]] bool start_dispatcher(ThreadEntry entry,
                                        void* context) noexcept;
    void join_dispatcher() noexcept;
    void watchdog_progress(uint32_t marker) noexcept;

    // -- M1 C3 extension: track the calling thread's direct-dispatch depth --
    void enter_direct() noexcept;
    void leave_direct() noexcept;

    // Windows backend: bounded MPSC (same as every SMP host).
    template <typename T, uint16_t Cap>
    using QueueBackend = coact::BoundedMpscQueue<T, Cap>;

    // -- SemOps family (pal.hpp): CreateSemaphoreW --------------------------
    // take(0) is a non-blocking try, take(kWaitForever) blocks until a token
    // exists, any other timeout_ms bounds the wait. release/release_from_isr
    // both call ReleaseSemaphore (host "ISR" producers are ordinary threads).
    bool sem_init(SemHandle& sem, uint32_t initial) noexcept;
    bool sem_take(SemHandle& sem, uint32_t timeout_ms) noexcept;
    void sem_release(SemHandle& sem) noexcept;
    void sem_release_from_isr(SemHandle& sem) noexcept;
    void sem_deinit(SemHandle& sem) noexcept;

    // -- MutexOps family (pal.hpp): CRITICAL_SECTION ------------------------
    bool mutex_init(MutexHandle& m) noexcept;
    void mutex_lock(MutexHandle& m) noexcept;
    void mutex_unlock(MutexHandle& m) noexcept;
    void mutex_deinit(MutexHandle& m) noexcept;

    // -- CondOps family (pal.hpp): CONDITION_VARIABLE + CRITICAL_SECTION ----
    bool cond_init(CondHandle& c) noexcept;
    // timeout_ms 0 = wait forever (the Dispatcher wait convention) — NOTE this
    // is the OPPOSITE of the Win32 dwMilliseconds==0 "return immediately"
    // meaning, so cond_wait translates 0/kWaitForever to INFINITE explicitly.
    void cond_wait(CondHandle& c, MutexHandle& m, uint32_t timeout_ms) noexcept;
    void cond_signal(CondHandle& c) noexcept;
    void cond_broadcast(CondHandle& c) noexcept;
    void cond_deinit(CondHandle& c) noexcept;

    // -- ThreadOps family (pal.hpp): _beginthreadex + WaitForSingleObject ----
    bool thread_create(ThreadHandle& t, ThreadEntry entry, void* context) noexcept;
    void thread_join(ThreadHandle& t) noexcept;

    // -- SoftIrqOps family (pal.hpp): event wake + SPSC payload ring -------
    bool softirq_init(SoftIrqHandle& h) noexcept;
    bool softirq_raise(SoftIrqHandle& h, int32_t payload) noexcept;
    // timeout_ms 0 == kWaitForever (the documented wait-forever convention);
    // returns -1 on timeout.
    int32_t softirq_take(SoftIrqHandle& h, uint32_t timeout_ms) noexcept;
    void softirq_deinit(SoftIrqHandle& h) noexcept;

    // -- Sleep (SemOps family companion): block the calling thread ---------
    // Whole milliseconds via Sleep(), the sub-millisecond remainder via a QPC
    // busy-wait. Windows' default timer resolution is ~15.6 ms unless the
    // process calls timeBeginPeriod(), so whole-ms Sleep() calls round UP to
    // the next tick — this is NOT a precise microsecond delay.
    void sleep_us(uint32_t us) noexcept;

private:
    static unsigned int __stdcall dispatcher_entry(void* arg) noexcept;
    static unsigned int __stdcall thread_trampoline(void* arg) noexcept;

    HANDLE      wake_event_;      // auto-reset Dispatcher wake
    HANDLE      started_event_;   // auto-reset start handshake
    bool        thread_valid_;
    HANDLE      dispatcher_thread_;  // _beginthreadex handle, closed on join
    ThreadEntry user_entry_;
    void*       user_ctx_;
    LARGE_INTEGER freq_;
    uint32_t    tick_hz_;         // 0 = no quantization (QPC native)
    uint64_t    ns_per_tick_;     // 1e9 / tick_hz_, valid when tick_hz_ != 0
    static thread_local ExecutionContext tls_ctx_;
};

// ---------------------------------------------------------------------------
// Neutral wall-clock / sleep helpers (free functions). Keep Win32's GetTickCount
// / Sleep / ULONGLONG / DWORD out of the business layer: callers use a monotonic
// millisecond counter and a bounded sleep, both expressed in fixed-width C++
// types. Implemented in pal_windows.cpp on QueryPerformanceCounter.
// ---------------------------------------------------------------------------
// Monotonic millisecond counter (QPC-derived). Never decreases; suitable for
// deadline arithmetic. Returns 0 if the performance counter is unavailable.
uint64_t monotonic_ms() noexcept;

// Block the calling thread for at least `ms` milliseconds.
void sleep_ms(uint32_t ms) noexcept;

}  // namespace pal
}  // namespace coact
