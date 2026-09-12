// coact Windows PAL implementation.
// SPDX-License-Identifier: MIT
#include "coact/pal_windows.hpp"

#if defined(_WIN32)
#include <process.h>   // _beginthreadex (real Windows SDK)
#endif

namespace coact {
namespace pal {

namespace {

// Busy-wait the sub-millisecond remainder of sleep_us() on QPC. Kept private
// and tiny: the alternative (a waitable timer per call) would be a heap
// allocation for a delay measured in microseconds.
inline void busy_wait_us(uint32_t us) noexcept
{
    LARGE_INTEGER f{};
    if ((0 == QueryPerformanceFrequency(&f)) || (f.QuadPart <= 0)) {
        return;
    }
    const uint64_t target = (static_cast<uint64_t>(us)
                             * static_cast<uint64_t>(f.QuadPart))
                          / 1000000ULL;
    LARGE_INTEGER start{};
    QueryPerformanceCounter(&start);
    for (;;) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const uint64_t delta =
            static_cast<uint64_t>(now.QuadPart - start.QuadPart);
        if (delta >= target) {
            return;
        }
    }
}

}  // namespace

thread_local ExecutionContext Windows::tls_ctx_ = {
    ContextKind::Task, 0U, 0U, false
};

Windows::Windows() noexcept
    : wake_event_(nullptr),
      started_event_(nullptr),
      thread_valid_(false),
      dispatcher_thread_(nullptr),
      user_entry_(nullptr),
      user_ctx_(nullptr),
      freq_{},
      tick_hz_(0U),
      ns_per_tick_(0U)
{
    wake_event_    = CreateEventW(nullptr, FALSE /*auto-reset*/,
                                  FALSE /*initially non-signaled*/, nullptr);
    started_event_ = CreateEventW(nullptr, FALSE /*auto-reset*/,
                                  FALSE /*initially non-signaled*/, nullptr);
    QueryPerformanceFrequency(&freq_);
}

// Release the Dispatcher wake event. Safe here: the owning runtime always
// join_dispatcher()s before the PAL member is destroyed, so no thread is
// blocked on or signaling wake_event_ now.
Windows::~Windows() noexcept
{
    join_dispatcher();
    if (wake_event_ != nullptr) {
        CloseHandle(wake_event_);
        wake_event_ = nullptr;
    }
    if (started_event_ != nullptr) {
        CloseHandle(started_event_);
        started_event_ = nullptr;
    }
}

// Interrupt masking is a no-op on a Windows SMP host; the token carries no
// state and the shared pool must use make_spin_critical_section instead.
CriticalToken Windows::irq_save() noexcept
{
    CriticalToken tok;
    tok.value = 0U;
    return tok;
}

void Windows::irq_restore(CriticalToken /*token*/) noexcept
{
    // no-op on a Windows host
}

bool Windows::register_current_task(LogicalPrio prio) noexcept
{
    if (kInvalidPrio == prio || prio > kMaxPrio) {
        return false;
    }
    tls_ctx_.kind         = ContextKind::Task;
    tls_ctx_.logical_prio = prio;
    tls_ctx_.prio_valid   = true;
    tls_ctx_.direct_depth = 0U;
    return true;
}

ExecutionContext Windows::current_context() const noexcept
{
    return tls_ctx_;
}

bool Windows::in_dispatcher_thread() noexcept
{
    return ContextKind::Dispatcher == tls_ctx_.kind;
}

uint64_t Windows::monotonic_ns() const noexcept
{
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    if (freq_.QuadPart <= 0) {
        return 0U;
    }
    /* Convert ticks to nanoseconds without overflowing. QPC typically runs at
       10 MHz, so count * 1e9 overflows int64 after roughly 15 minutes of
       uptime — and signed overflow is undefined, not merely wrong. Split the
       count into whole seconds and the remainder so each product stays small:
       the quotient contributes seconds * 1e9, the remainder is bounded by
       freq_ so remainder * 1e9 fits comfortably (freq_ is far below 2^33). */
    const uint64_t ticks = static_cast<uint64_t>(c.QuadPart);
    const uint64_t freq = static_cast<uint64_t>(freq_.QuadPart);
    uint64_t raw = (ticks / freq) * 1000000000ULL +
                   ((ticks % freq) * 1000000000ULL) / freq;
    if (0U != tick_hz_) {
        raw = (raw / ns_per_tick_) * ns_per_tick_;
    }
    return raw;
}

uint64_t Windows::clock_resolution_ns() const noexcept
{
    if (0U != tick_hz_) {
        return ns_per_tick_;
    }
    // QPC itself is sub-microsecond, but the practical floor the runtime
    // relies on (Sleep / wait granularity) is the ~1 ms scheduler tick.
    return 1000000ULL;
}

void Windows::set_tick_hz(uint32_t hz) noexcept
{
    if (0U == hz || hz > 1000000000U) {
        tick_hz_    = 0U;
        ns_per_tick_ = 0U;
        return;
    }
    tick_hz_     = hz;
    ns_per_tick_ = 1000000000ULL / hz;
}

void Windows::set_dispatcher_stack_bytes(uint32_t /*bytes*/) noexcept
{
    // Default thread stack is used.
}

void Windows::wait_dispatcher(uint32_t timeout_ms) noexcept
{
    if (nullptr == wake_event_) {
        return;
    }
    const DWORD ms = (0U == timeout_ms) ? INFINITE
                                        : static_cast<DWORD>(timeout_ms);
    (void)WaitForSingleObject(wake_event_, ms);
}

void Windows::signal_dispatcher_from_task() noexcept
{
    if (wake_event_ != nullptr) {
        SetEvent(wake_event_);
    }
}

// Non-blocking: safe for real callback/ISR producers. Uses SetEvent only.
void Windows::signal_dispatcher_from_isr() noexcept
{
    if (wake_event_ != nullptr) {
        SetEvent(wake_event_);
    }
}

unsigned int __stdcall Windows::dispatcher_entry(void* arg) noexcept
{
    Windows* self = static_cast<Windows*>(arg);
    tls_ctx_.kind         = ContextKind::Dispatcher;
    tls_ctx_.logical_prio = 0U;
    tls_ctx_.prio_valid   = false;
    tls_ctx_.direct_depth = 0U;
    // Dispatcher runs dispatch and wake consumers. Use an above-normal host
    // priority, deliberately not realtime.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    if (self->started_event_ != nullptr) {
        SetEvent(self->started_event_);
    }
    self->user_entry_(self->user_ctx_);
    return 0U;
}

bool Windows::start_dispatcher(ThreadEntry entry, void* context) noexcept
{
    if (entry == nullptr || started_event_ == nullptr || thread_valid_) {
        return false;
    }
    user_entry_ = entry;
    user_ctx_   = context;
    // _beginthreadex returns 0 (null handle) on failure — no C++ exceptions,
    // which matters because coact_core is built with -fno-exceptions.
    const uintptr_t raw = _beginthreadex(
        nullptr, 0U, &Windows::dispatcher_entry, this, 0U, nullptr);
    if (0U == raw) {
        return false;
    }
    dispatcher_thread_ = reinterpret_cast<HANDLE>(raw);
    thread_valid_      = true;
    // Handshake: only report success once the Dispatcher has actually entered
    // its entry function.
    if (WAIT_OBJECT_0 == WaitForSingleObject(started_event_, 1000U)) {
        return true;
    }
    // The handshake timed out — e.g. the thread was starved before it could
    // signal. Do NOT join here: the thread runs user_entry_, the Dispatcher
    // loop, which does not return on its own, so an INFINITE wait would block
    // the caller forever. Release our reference to the handle and report the
    // failure; the thread is left to finish under the OS. Closing the handle
    // does not terminate the thread, and _beginthreadex followed by
    // CloseHandle is the documented way to detach it.
    CloseHandle(dispatcher_thread_);
    dispatcher_thread_ = nullptr;
    thread_valid_      = false;
    return false;
}

void Windows::join_dispatcher() noexcept
{
    if (thread_valid_ && dispatcher_thread_ != nullptr) {
        (void)WaitForSingleObject(dispatcher_thread_, INFINITE);
        CloseHandle(dispatcher_thread_);
        dispatcher_thread_ = nullptr;
    }
    thread_valid_ = false;
}

void Windows::watchdog_progress(uint32_t /*marker*/) noexcept
{
    // no-op on a Windows host
}

void Windows::enter_direct() noexcept
{
    ++tls_ctx_.direct_depth;
}

void Windows::leave_direct() noexcept
{
    if (tls_ctx_.direct_depth > 0U) {
        --tls_ctx_.direct_depth;
    }
}

/* ---------------------------------------------------------------------------
 * SemOps family: CreateSemaphoreW. WaitForSingleObject's millisecond timeout
 * maps directly onto the PAL contract — 0 = non-blocking try, kWaitForever =
 * INFINITE, any other value = bounded wait.
 * ------------------------------------------------------------------------- */

bool Windows::sem_init(SemHandle& sem, uint32_t initial) noexcept
{
    sem.sem = CreateSemaphoreW(nullptr, static_cast<LONG>(initial),
                               0x7FFFFFFFL /*LONG_MAX*/, nullptr);
    if (nullptr == sem.sem) {
        return false;
    }
    sem.pal = this;
    return true;
}

bool Windows::sem_take(SemHandle& sem, uint32_t timeout_ms) noexcept
{
    if (nullptr == sem.sem) {
        return false;
    }
    DWORD ms;
    if (0U == timeout_ms) {
        ms = 0U;   // non-blocking try
    }
    else if (kWaitForever == timeout_ms) {
        ms = INFINITE;
    }
    else {
        ms = static_cast<DWORD>(timeout_ms);
    }
    return (WAIT_OBJECT_0 == WaitForSingleObject(sem.sem, ms));
}

void Windows::sem_release(SemHandle& sem) noexcept
{
    if (sem.sem != nullptr) {
        (void)ReleaseSemaphore(sem.sem, 1L, nullptr);
    }
}

void Windows::sem_release_from_isr(SemHandle& sem) noexcept
{
    // Host ISR simulation: ReleaseSemaphore is callable from any normal thread
    // standing in for the ISR (same limitation as signal_dispatcher_from_isr).
    sem_release(sem);
}

void Windows::sem_deinit(SemHandle& sem) noexcept
{
    if (sem.sem != nullptr) {
        CloseHandle(sem.sem);
        sem.sem = nullptr;
    }
}

/* ---------------------------------------------------------------------------
 * MutexOps family: CRITICAL_SECTION (recursive, non-priority-inheriting on
 * Windows — the target-side priority-inheriting contract cannot be reproduced
 * on a host, documented limitation).
 * ------------------------------------------------------------------------- */

bool Windows::mutex_init(MutexHandle& m) noexcept
{
    if (0 == InitializeCriticalSectionAndSpinCount(&m.cs, 0U)) {
        return false;
    }
    m.pal = this;
    return true;
}

void Windows::mutex_lock(MutexHandle& m) noexcept
{
    EnterCriticalSection(&m.cs);
}

void Windows::mutex_unlock(MutexHandle& m) noexcept
{
    LeaveCriticalSection(&m.cs);
}

void Windows::mutex_deinit(MutexHandle& m) noexcept
{
    DeleteCriticalSection(&m.cs);
}

/* ---------------------------------------------------------------------------
 * CondOps family: CONDITION_VARIABLE over the paired CRITICAL_SECTION.
 * CRITICAL NOTE: the PAL convention is timeout_ms 0 = wait forever, but
 * SleepConditionVariableCS treats dwMilliseconds 0 as "return immediately".
 * cond_wait therefore translates 0 / kWaitForever to INFINITE explicitly —
 * the two conventions are opposite and must never be passed through raw.
 * ------------------------------------------------------------------------- */

bool Windows::cond_init(CondHandle& c) noexcept
{
    InitializeConditionVariable(&c.cv);
    c.pal = this;
    return true;
}

void Windows::cond_wait(CondHandle& c, MutexHandle& m, uint32_t timeout_ms) noexcept
{
    DWORD ms;
    if (0U == timeout_ms || kWaitForever == timeout_ms) {
        ms = INFINITE;   // 0 means forever here, NOT SleepConditionVariableCS's 0
    }
    else {
        ms = static_cast<DWORD>(timeout_ms);
    }
    (void)SleepConditionVariableCS(&c.cv, &m.cs, ms);
}

void Windows::cond_signal(CondHandle& c) noexcept
{
    WakeConditionVariable(&c.cv);
}

void Windows::cond_broadcast(CondHandle& c) noexcept
{
    WakeAllConditionVariable(&c.cv);
}

void Windows::cond_deinit(CondHandle& c) noexcept
{
    // CONDITION_VARIABLE has no destroy; nothing to release.
    (void)c;
}

/* ---------------------------------------------------------------------------
 * ThreadOps family: _beginthreadex + WaitForSingleObject. entry + context are
 * stored in the caller's ThreadHandle (which join() is required to outlive the
 * thread for), so the trampoline needs no heap.
 * ------------------------------------------------------------------------- */

unsigned int __stdcall Windows::thread_trampoline(void* arg) noexcept
{
    ThreadHandle* t = static_cast<ThreadHandle*>(arg);
    t->entry(t->context);
    return 0U;
}

bool Windows::thread_create(ThreadHandle& t, ThreadEntry entry,
                            void* context) noexcept
{
    t.thread  = nullptr;
    t.valid   = false;
    t.entry   = entry;
    t.context = context;
    t.pal     = this;
    const uintptr_t raw = _beginthreadex(
        nullptr, 0U, &Windows::thread_trampoline, &t, 0U, nullptr);
    if (0U == raw) {
        return false;
    }
    t.thread = reinterpret_cast<HANDLE>(raw);
    t.valid  = true;
    return true;
}

void Windows::thread_join(ThreadHandle& t) noexcept
{
    if (!t.valid) {
        return;
    }
    (void)WaitForSingleObject(t.thread, INFINITE);
    CloseHandle(t.thread);
    t.thread = nullptr;
    t.valid  = false;
}

/* ---------------------------------------------------------------------------
 * SoftIrqOps family: CreateEventW wake hint + fixed 8-slot SPSC payload ring.
 *
 * Windows has no signalfd and a Win32 event cannot carry the int32_t payload,
 * so — exactly like the RT-Thread port — the payload is published to the ring
 * and the event is only a wake hint. raise() rejects (returns false) when the
 * ring is full instead of overwriting; take() drains FIFO, so N distinct
 * raises yield N distinct payloads (no coalescing). The ring is SPSC, not
 * MPSC. timeout_ms 0 / kWaitForever waits forever; -1 on timeout.
 * ------------------------------------------------------------------------- */

bool Windows::softirq_init(SoftIrqHandle& h) noexcept
{
    h.event = CreateEventW(nullptr, FALSE /*auto-reset*/,
                           FALSE /*initially non-signaled*/, nullptr);
    if (nullptr == h.event) {
        return false;
    }
    h.head.store(0U, std::memory_order_relaxed);
    h.tail.store(0U, std::memory_order_relaxed);
    h.pal = this;
    return true;
}

bool Windows::softirq_raise(SoftIrqHandle& h, int32_t payload) noexcept
{
    // SPSC ring: the producer's head publish uses a release barrier paired
    // with the consumer's head.load(acquire). relaxed tail.load is fine — the
    // consumer never writes tail until it has consumed the slot.
    const uint32_t head = h.head.load(std::memory_order_relaxed);
    const uint32_t tail = h.tail.load(std::memory_order_acquire);
    if (head - tail >= kSoftIrqRingSlots) {
        return false;   // mailbox full: drop, never overwrite
    }
    h.ring[head % kSoftIrqRingSlots] = payload;
    h.head.store(head + 1U, std::memory_order_release);
    // Wake hint: the payload already lives in the ring, so a failed wake does
    // not invalidate it — take() still drains the ring.
    if (h.event != nullptr) {
        SetEvent(h.event);
    }
    return true;
}

int32_t Windows::softirq_take(SoftIrqHandle& h, uint32_t timeout_ms) noexcept
{
    const bool forever = (0U == timeout_ms) || (kWaitForever == timeout_ms);
    const uint64_t start = monotonic_ms();
    for (;;) {
        const uint32_t tail = h.tail.load(std::memory_order_relaxed);
        const uint32_t head = h.head.load(std::memory_order_acquire);
        if (tail != head) {
            const int32_t payload = h.ring[tail % kSoftIrqRingSlots];
            // Pair the producer's release: the slot is consumed only after the
            // tail publish.
            h.tail.store(tail + 1U, std::memory_order_release);
            return payload;
        }
        DWORD wait_ms;
        if (forever) {
            wait_ms = INFINITE;
        }
        else {
            const uint64_t elapsed = monotonic_ms() - start;
            if (elapsed >= timeout_ms) {
                return -1;
            }
            wait_ms = static_cast<DWORD>(timeout_ms - elapsed);
            if (0U == wait_ms) {
                wait_ms = 1U;
            }
        }
        if (nullptr == h.event) {
            return -1;
        }
        const DWORD w = WaitForSingleObject(h.event, wait_ms);
        if (WAIT_TIMEOUT == w || WAIT_OBJECT_0 != w) {
            return -1;   // timed out (or wait failure)
        }
        // Signaled: loop back and drain the ring (an auto-reset event may also
        // carry one stale signal from a raise() the consumer already drained).
    }
}

void Windows::softirq_deinit(SoftIrqHandle& h) noexcept
{
    if (h.event != nullptr) {
        CloseHandle(h.event);
        h.event = nullptr;
    }
    h.head.store(0U, std::memory_order_relaxed);
    h.tail.store(0U, std::memory_order_relaxed);
    h.pal = nullptr;
}

void Windows::sleep_us(uint32_t us) noexcept
{
    if (0U == us) {
        return;
    }
    // Whole milliseconds via Sleep(); the remainder busy-waits on QPC. Note
    // Windows' default timer resolution is ~15.6 ms unless timeBeginPeriod()
    // was called, so a whole-ms Sleep() can overshoot to the next tick — this
    // is a floor, not a precise microsecond delay.
    const uint32_t whole_ms = us / 1000U;
    const uint32_t rem_us   = us % 1000U;
    if (whole_ms > 0U) {
        Sleep(whole_ms);
    }
    if (rem_us > 0U) {
        busy_wait_us(rem_us);
    }
}

/* ---------------------------------------------------------------------------
 * WakeEvent
 * ------------------------------------------------------------------------- */

WakeEvent::WakeEvent() noexcept
    : handle_(CreateEventW(nullptr, FALSE /*auto-reset*/,
                           FALSE /*initially non-signaled*/, nullptr))
{
}

WakeEvent::~WakeEvent() noexcept
{
    if (handle_ != nullptr) {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
}

WakeEvent::WakeEvent(WakeEvent&& other) noexcept
    : handle_(other.handle_)
{
    other.handle_ = nullptr;
}

WakeEvent& WakeEvent::operator=(WakeEvent&& other) noexcept
{
    if (this != &other) {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
        handle_       = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

bool WakeEvent::valid() const noexcept
{
    return handle_ != nullptr;
}

void WakeEvent::signal() noexcept
{
    if (handle_ != nullptr) {
        SetEvent(handle_);
    }
}

bool WakeEvent::wait(uint32_t ms) noexcept
{
    if (handle_ == nullptr) {
        return false;
    }
    // PAL convention: 0 = wait forever.
    const DWORD native_ms = (0U == ms) ? INFINITE : static_cast<DWORD>(ms);
    return (WAIT_OBJECT_0 == WaitForSingleObject(handle_, native_ms));
}

/* ---------------------------------------------------------------------------
 * Neutral wall-clock / sleep helpers (free functions).
 * ------------------------------------------------------------------------- */
uint64_t monotonic_ms() noexcept
{
    // QueryPerformanceFrequency is stable for the process lifetime; read it
    // once and cache in a function-local so callers need no instance state.
    static const LARGE_INTEGER freq = []() noexcept {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    if (freq.QuadPart <= 0) {
        return 0U;
    }
    return static_cast<uint64_t>(
        (static_cast<long long>(c.QuadPart) * 1000LL) / freq.QuadPart);
}

void sleep_ms(uint32_t ms) noexcept
{
    Sleep(static_cast<DWORD>(ms));
}

}  // namespace pal
}  // namespace coact
