// coact::coro POSIX layer: single-core cooperative executor carrier and
// blocking-IO escape hatch. LINUX PAL ONLY - never compiled into RT-Thread
// units (plan §1.1: coact::coro supports embedded Linux only).
// SPDX-License-Identifier: MIT
//
// Scope (user-clarified):
//   1. StackfulExecutor - THE point of coact::coro on Linux: one real
//      pthread (optionally pinned to one CPU core) running ALL worker
//      execute logic as stackful coroutines (ucontext + static stack pool).
//      It replaces the demo's per-worker pthreads so Linux timing semantics
//      match a single-core MCU (fair comparison with the RT-Thread PAL).
//   2. AffinityPin - optional CPU pinning (pthread_setaffinity_np); degrades
//      to a no-op single-thread process when the API is unavailable - the
//      fairness property (no multi-core preemption of task state) holds
//      either way, which the header comment must state.
//   3. Blocking-IO escape hatch helpers (WorkerThread, sleep_ns) - only for
//      operations that genuinely must block.
//
// Stackful coroutine model (Lua coroutine analogy):
//   - yield(WaitReason) == coroutine.yield(reason): the coroutine declares
//     WHY it pauses ("sleep until T" / "wait for task X" / "done"); the
//     scheduler never guesses. A resume arrives as an AO event whose payload
//     is the resume argument (completion result, timer expiry).
//   - The ucontext stack preserves the execution position, so worker execute
//     logic is written as natural sequential code (send command -> wait for
//     IRQ -> continue) - the same shape as real RS500 driver code, unlike
//     the stackless HSM decomposition.
//   - Stacks are STATIC (compile-time array of aligned std::byte), never
//     heap-allocated: zero-heap holds.
//
// Constraints:
//   - ucontext works under -fno-exceptions.
//   - TSan has known false positives around swapcontext (stack switching);
//     coro tests document this and verify under ASan/UBSan primarily.
//   - No business #ifdef: including this header IS the platform choice.
#pragma once

#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <ucontext.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

#include "coact/coro/config.hpp"

namespace coact {
namespace coro {
namespace posix {

// Monotonic clock in nanoseconds (CLOCK_MONOTONIC, same source as the
// SteadyTickSource).
inline uint64_t now_ns() noexcept
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

// Blocking sleep for worker bodies only (never on the executor thread).
inline void sleep_ns(uint64_t ns) noexcept
{
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
    ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
    clock_nanosleep(CLOCK_MONOTONIC, 0U, &ts, nullptr);
}

// Optional CPU pinning for the executor thread. Returns false when the
// affinity API is unavailable or the core index is invalid; the caller then
// continues unpinned (a single-threaded process is equally fair - there is
// no second core preempting task state).
inline bool pin_current_thread_to_core(uint32_t core_index) noexcept
{
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (static_cast<unsigned>(core_index) >= CPU_SETSIZE) {
        return false;
    }
    /* CPU_SET's bit arithmetic on the cpu_set_t word is a signed-word
       operation by glibc's definition; the int cast below matches its
       parameter type and the sign is provably non-negative here. */
    const int cpu = static_cast<int>(core_index);
    set.__bits[static_cast<size_t>(cpu) / (8U * sizeof(unsigned long))] |=
        1ULL << (static_cast<size_t>(cpu) % (8U * sizeof(unsigned long)));
    return 0 == pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)core_index;
    return false;
#endif
}

// -------------------------------------------------------------------------
// Yield reasons (coroutine.yield(reason) equivalent). The scheduler
// dispatches on the reason: sleep -> timer event, wait_task -> registry
// waiter registration, done -> coroutine retires.
// -------------------------------------------------------------------------
enum class WaitReason : uint8_t {
    kNone = 0U,     // resume immediately (not a pause)
    kSleep,         // pause until the deadline (ns since executor start)
    kWaitTask,      // pause until the named task completes
    kDone           // coroutine finished
};

// What a coroutine hands to the scheduler when it yields. The scheduler
// consumes the reason and the parameter (deadline or task id) and registers
// the resume condition.
struct YieldRequest {
    WaitReason reason = WaitReason::kDone;
    uint64_t param_ns = 0U;   // kSleep: absolute deadline (now_ns based)
    uint16_t param_task = 0U; // kWaitTask: raw TaskId value
    // kSleep only: notify-sequence snapshot taken by the body's wait loop
    // AFTER its condition re-check and BEFORE this yield. 0 (default) = a
    // plain timed sleep with no condition: deadline-only wake. The sleeping
    // slot re-wakes when the global sequence differs from this snapshot -
    // the snapshot being taken after the re-check closes the lost-wake
    // window (a notify that lands between the re-check and the park is
    // already != snapshot).
    uint32_t seq_snapshot = 0U;
};

// Resume argument (coroutine.resume(co, ...) equivalent): the payload of
// the event that woke the coroutine.
struct ResumeArg {
    bool task_succeeded = false;
    uint16_t task_id = 0U;    // raw TaskId of the completed task
};

// Stackful coroutine body signature: a plain function running on the
// coroutine's own stack. The body yields via self.yield(...) and reads the
// resume argument of each wake-up via self.resume_arg() - the entry-time
// argument is only the INITIAL one (Lua semantics: resume args arrive per
// wake-up, coroutine state survives on the ucontext stack).
class Coroutine;

// Static stack pool: one aligned std::byte array per coroutine slot. The
// pool is owned by the caller (embedded in the executor object); no heap.
template <uint32_t StackBytes>
struct StaticStackPool {
    static constexpr uint32_t kStackBytes = StackBytes;

    alignas(16) std::array<std::byte, StackBytes> bytes{};

    void* base() noexcept { return bytes.data(); }
};

// One stackful coroutine: ucontext pair (its own stack + the executor's
// context), the body trampoline and the yield/resume mailbox. Moveable as a
// plain handle (the context does not move - only the executor owns slots).
class Coroutine final {
public:
    Coroutine() noexcept = default;

    Coroutine(const Coroutine&) = delete;
    Coroutine& operator=(const Coroutine&) = delete;

    // Arm the coroutine on the given stack. arg is the initial resume
    // argument (Lua: the first coroutine.resume args); later args arrive
    // through resume_arg() after each yield.
    bool arm(void* stack, uint32_t stack_bytes,
             void (*body)(void* user, Coroutine& self),
             void* user, ResumeArg arg) noexcept;

    // Resume the coroutine (scheduler side). Returns the next yield request.
    YieldRequest resume(ResumeArg arg) noexcept;

    // Resume argument of the CURRENT wake-up: valid inside the body right
    // after a yield returns (the executor refreshed it before the swap).
    ResumeArg resume_arg() const noexcept { return pending_arg_; }

    static Coroutine* current() noexcept { return active_; }

    bool is_running() const noexcept
    {
        return running_.load(std::memory_order_acquire);
    }

    // True once start() entered the body (executor picks start vs resume).
    bool started() const noexcept { return started_; }

    // Initial entry of an armed coroutine (see definition below).
    YieldRequest start(ResumeArg arg) noexcept;

private:
    ucontext_t ctx_{};          // coroutine context (runs on its stack)
    ucontext_t return_ctx_{};   // executor context to swap back to
    void (*body_)(void*, Coroutine&) = nullptr;
    void* user_ = nullptr;
    ResumeArg pending_arg_{};
    YieldRequest last_yield_{};
    bool armed_ = false;
    std::atomic<bool> running_{false};
    bool started_ = false;
    inline static thread_local Coroutine* active_ = nullptr;
    static void trampoline(void* self_void) noexcept;
    void run_body() noexcept;

public:
    // Called from INSIDE the body (on the coroutine stack): yield control
    // back to the executor with a reason. The mailbox pattern mirrors Lua's
    // coroutine.yield: the scheduler sees the reason, the body's locals stay
    // on its stack.
    YieldRequest yield(YieldRequest request) noexcept;
};


inline void Coroutine::trampoline(void* self_void) noexcept
{
    static_cast<Coroutine*>(self_void)->run_body();
}

inline void Coroutine::run_body() noexcept
{
    // Single body run: the body loops/yields internally; control returns
    // here only when the body RETURNS (no kDone yield) or after its kDone
    // yield swapped back and the body then fell through. A plain return is
    // treated as done. Re-running a finished body is not supported: the
    // executor retires the coroutine on kDone / plain return.
    running_ = true;
    body_(user_, *this);
    running_ = false;
    last_yield_ = YieldRequest{WaitReason::kDone, 0U, 0U};
    swapcontext(&ctx_, &return_ctx_);
}

inline YieldRequest Coroutine::yield(YieldRequest request) noexcept
{
    /* kSleep: the CALLER filled seq_snapshot BEFORE this call (after its
       own condition re-check) - the lost-wake window is closed by that
       ordering, not here. Plain sleeps (no waiter) leave it zero, which
       never equals a bumped sequence and behaves like a normal deadline
       sleep plus notify-wake (harmless: a body that did not check any
       condition just re-parks). */
    last_yield_ = request;
    if (WaitReason::kDone == request.reason) {
        running_ = false;
    }
    swapcontext(&ctx_, &return_ctx_);
    // Resumed: pending_arg_ was refreshed by resume() before the swap back.
    return last_yield_;
}

inline bool Coroutine::arm(void* stack, uint32_t stack_bytes,
                           void (*body)(void*, Coroutine&),
                           void* user, ResumeArg arg) noexcept
{
    if (nullptr == stack || nullptr == body || 0U == stack_bytes) {
        return false;
    }
    body_ = body;
    user_ = user;
    pending_arg_ = arg;
    running_ = false;
    started_ = false;
    armed_ = getcontext(&ctx_) == 0;
    if (!armed_) {
        return false;
    }
    ctx_.uc_stack.ss_sp = stack;
    ctx_.uc_stack.ss_size = stack_bytes;
    // Natural body return is handled by run_body(), which swaps to the
    // executor context captured by resume/start. Keep uc_link null so
    // makecontext never attempts an implicit setcontext to the arming thread.
    ctx_.uc_link = nullptr;
    makecontext(&ctx_, reinterpret_cast<void (*)()>(&Coroutine::trampoline),
                1, static_cast<void*>(this));
    return true;
}

inline YieldRequest Coroutine::resume(ResumeArg arg) noexcept
{
    if (!armed_ || !running_) {
        return YieldRequest{WaitReason::kDone, 0U, 0U};
    }
    Coroutine* const prev_active = active_;   // restore on the way back so
    active_ = this;                           // the executor never sees a
    pending_arg_ = arg;                       // suspended coroutine as
    if (swapcontext(&return_ctx_, &ctx_) != 0) {  // "current" after a yield
        running_ = false;
        active_ = prev_active;
        return YieldRequest{WaitReason::kDone, 0U, 0U};
    }
    active_ = prev_active;
    return last_yield_;
}

// Initial entry of an armed coroutine (Lua's first coroutine.resume):
// mirrors resume() but flips the running flag so the trampoline executes.
// The trampoline sets running_ = true itself before calling the body; this
// helper only needs to bypass the not-yet-running guard once.
inline YieldRequest Coroutine::start(ResumeArg arg) noexcept
{
    if (!armed_ || running_) {
        return YieldRequest{WaitReason::kDone, 0U, 0U};
    }
    running_ = true;
    started_ = true;
    Coroutine* const prev_active = active_;   // same restore discipline as
    active_ = this;                           // resume(): after the body
    pending_arg_ = arg;                       // yields or returns, the
    if (swapcontext(&return_ctx_, &ctx_) != 0) {  // executor must observe its
        running_ = false;                     // own view in current(), not a
        active_ = prev_active;                // suspended coroutine
        return YieldRequest{WaitReason::kDone, 0U, 0U};
    }
    active_ = prev_active;
    return last_yield_;
}

// -------------------------------------------------------------------------
// StackfulExecutor: one real pthread (optionally pinned) running ALL
// coroutines cooperatively. THE Linux PAL replacement for per-worker
// pthreads: N workers become N coroutines on ONE thread, matching the
// single-core MCU execution semantics of the RT-Thread PAL.
//
// Cooperative loop (one pass of run_once(), called by the executor thread):
//   1. Every runnable (not-paused) coroutine gets one resume slot per pass
//      (round-robin fairness - no coroutine starves another).
//   2. A kSleep yield parks the coroutine until now_ns() >= param_ns.
//   3. A kWaitTask yield parks it until wake(TaskId) is called (the task
//      registry completion path invokes wake() - or the executor's host
//      application does from the coact Dispatcher).
//   4. kDone retires the slot.
//
// Resume arguments are zero on timer wake-ups and carry the completion
// status on task wake-ups (wake() takes a ResumeArg).
//
// Synchronization contract with the outside (Dispatcher / test thread):
//   The executor is SINGLE-THREADED by design. arm()/wake()/retire() must
//   be called either from the executor thread itself or before start().
//   Cross-thread wake-ups go through the coact event plane instead (the
//   TaskRegistry mutex makes complete() safe from any thread; the resume
//   itself is delivered on the executor's next pass). No mutex is needed
//   inside the executor loop (weakest-sufficient rule: the loop is the
//   only accessor once started).
// -------------------------------------------------------------------------
template <uint16_t MaxCoroutines, uint32_t StackBytes>
class StackfulExecutor final {
public:
    static constexpr uint16_t kCapacity = MaxCoroutines;
    static constexpr uint32_t kStackBytes = StackBytes;

    StackfulExecutor() noexcept {
        /* Fill all stack guards with the magic at construction so the
           run_once() canary check does not flag never-armed slots. arm()
           overwrites both ends on each use. */
        constexpr uint64_t kGuard = 0xDEADBEEFCAFEBABEULL;
        for (uint16_t i = 0U; i < kCapacity; ++i) {
            std::byte* base = stacks_[i].bytes.data();
            std::memcpy(base, &kGuard, sizeof(kGuard));
            std::memcpy(base + kStackBytes - sizeof(kGuard), &kGuard,
                        sizeof(kGuard));
        }
    }
    StackfulExecutor(const StackfulExecutor&) = delete;
    StackfulExecutor& operator=(const StackfulExecutor&) = delete;

    // Arm a coroutine on a free stack slot. Returns nullptr when all
    // slots are busy. Must be called before start() or from the executor
    // thread.
    Coroutine* arm(void (*body)(void*, Coroutine&), void* user,
                   ResumeArg initial_arg) noexcept
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (uint16_t i = 0U; i < kCapacity; ++i) {
            if (SlotState::kFree == slots_[i].state) {
                Coroutine& co = slots_[i].co;
                if (!co.arm(stacks_[i].bytes.data(), StackBytes, body, user,
                            initial_arg)) {
                    return nullptr;
                }
                /* STACK GUARD: mark both ends of this coroutine stack so
                   run_once() can detect overflow / underflow precisely. */
                {
                    std::byte* base = stacks_[i].bytes.data();
                    constexpr uint64_t kGuard = 0xDEADBEEFCAFEBABEULL;
                    std::memcpy(base, &kGuard, sizeof(kGuard));
                    std::memcpy(base + StackBytes - sizeof(kGuard), &kGuard,
                                sizeof(kGuard));
                }
                slots_[i].state = SlotState::kRun;
                slots_[i].deadline_ns = 0U;
                return &co;
            }
        }
        return nullptr;
    }

    // Deliver a task-completion wake-up to a paused coroutine. The arg
    // carries the completion status (resume argument). Safe to call from
    // the TaskRegistry completion path.
    void wake(Coroutine& co, ResumeArg arg) noexcept
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        Slot* s = slot_of(co);
        if ((nullptr != s) && (SlotState::kWaitTask == s->state)) {
            s->state = SlotState::kRun;
            s->pending_arg = arg;
        }
    }

    // Cumulative count of slots whose stack guard was corrupted at
    // retirement. Grows monotonically; a non-zero value means a coroutine
    // body overran or underran its stack, and that slot's neighbors may be
    // silently corrupted. Lock-free read: the counter is only written by
    // run_once() (inside the state lock), relaxed read is a plain word on
    // every supported target.
    uint16_t corrupted_guards() const noexcept
    {
        return corrupted_guards_.load(std::memory_order_relaxed);
    }

    // Peak stack usage of a slot: the distance from the stack top guard to
    // the deepest byte a body wrote (the stack grows down; end guards are
    // excluded). Returns 0 for a never-armed slot. Diagnostic only: call
    // after the executor is stopped. Best-effort read, no watermark tracking
    // on the hot resume path.
    uint32_t stack_watermark(uint16_t slot) const noexcept
    {
        if (slot >= kCapacity) {
            return 0U;
        }
        const std::byte* const base = stacks_[slot].bytes.data();
        /* Skip the top guard, then scan down for the first un-written byte:
           the floor of the used region. Stack grows toward base. */
        for (uint32_t i = kStackBytes - (sizeof(uint64_t) + 1U); i > 0U; --i) {
            if (std::byte{0U} != base[i]) {
                return i + 1U;
            }
        }
        return 0U;
    }

    // One cooperative pass: run every runnable coroutine once, retire the
    // finished ones. Called by the executor thread loop (or a single-shot
    // driver in tests - the same cooperative semantics, no thread needed).
    // Returns the number of live (non-retired) coroutines after the pass.
    uint16_t run_once() noexcept
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const uint64_t now = now_ns();
        const uint32_t notify_seq =
            notify_seq_.load(std::memory_order_acquire);
        uint16_t live = 0U;

        for (uint16_t i = 0U; i < kCapacity; ++i) {
            Slot& s = slots_[i];
            if (SlotState::kFree == s.state) {
                continue;
            }
            if (SlotState::kSleep == s.state) {
                if (now < s.deadline_ns) {
                    /* Event-driven early wake (WAITER sleeps only): the
                       snapshot is taken by the body's wait loop AFTER its
                       condition re-check. Snapshot 0 = a plain timed sleep
                       with no condition to re-check - deadline-only wake.
                       A waiter's snapshot never equals a sequence bumped
                       since its park (the lost-wake closer); a false alarm
                       costs one pass (the body re-parks). */
                    if ((0U == s.park_seq)
                        || (s.park_seq == notify_seq)) {
                        ++live;
                        continue;
                    }
                    s.state = SlotState::kRun;
                    s.pending_arg = ResumeArg{};
                }
                else {
                    s.state = SlotState::kRun;
                    s.pending_arg = ResumeArg{};
                }
            }
            if (SlotState::kRun != s.state) {
                ++live;
                continue;
            }

            YieldRequest y = s.co.started()
                                 ? s.co.resume(s.pending_arg)
                                 : s.co.start(s.pending_arg);
            s.pending_arg = ResumeArg{};
            switch (y.reason) {
            case WaitReason::kDone:
            case WaitReason::kNone:
                check_guard(i);
                s.state = SlotState::kFree;
                break;
            case WaitReason::kSleep:
                s.state = SlotState::kSleep;
                s.deadline_ns = y.param_ns;
                s.park_seq = y.seq_snapshot;   // from yield(): post-recheck
                ++live;
                break;
            case WaitReason::kWaitTask:
                s.state = SlotState::kWaitTask;
                ++live;
                break;
            default:
                s.state = SlotState::kFree;
                break;
            }
        }
        return live;
    }

    // Number of non-free slots.
    uint16_t live_count() const noexcept
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        uint16_t live = 0U;
        for (uint16_t i = 0U; i < kCapacity; ++i) {
            if (SlotState::kFree != slots_[i].state) {
                ++live;
            }
        }
        return live;
    }

    // Test/instrumentation hook: writable view of a slot's stack (guard
    // region included). Lets the guard-verification test corrupt a guard
    // deterministically - a REAL overflow smashes glibc's context-restore
    // data before the guard, so the test injects the guard damage directly
    // instead of triggering undefined behavior.
    std::byte* stack_for_test(uint16_t slot) noexcept
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return (slot < kCapacity) ? stacks_[slot].bytes.data() : nullptr;
    }

    // Event-driven early wake (PAL sem_release/cond_signal path): bump the
    // notification sequence so the NEXT run_once() pass resumes every
    // sleeping coroutine before its deadline (each body then re-checks its
    // own condition - sem/cond state lives outside the executor). Safe from
    // any thread: a single relaxed increment. Thread-safe by design with
    // run_once(); kWaitTask slots are NOT disturbed (they have their own
    // wake() channel).
    void notify() noexcept
    {
        notify_seq_.fetch_add(1U, std::memory_order_release);
    }

    // Current notify-sequence value (wait loops snapshot this AFTER their
    // condition re-check and pass it via YieldRequest.seq_snapshot).
    uint32_t notify_sequence() const noexcept
    {
        return notify_seq_.load(std::memory_order_acquire);
    }

    // Test hook: force the notify sequence to a value near the uint32 wrap
    // boundary so the boundary test can park a waiter at 0xFFFFFFFF and prove
    // a notify() wrapping back to 0 still wakes it. Relaxed store: tests call
    // it before arming/parking on a single thread.
    void notify_sequence_for_test(uint32_t v) noexcept
    {
        notify_seq_.store(v, std::memory_order_release);
    }

private:
    enum class SlotState : uint8_t { kFree = 0U, kRun, kSleep, kWaitTask };

    struct Slot {
        Coroutine co;
        SlotState state = SlotState::kFree;
        uint64_t deadline_ns = 0U;
        ResumeArg pending_arg{};
        uint32_t park_seq = 0U;   // notify_seq_ snapshot when the body parked
    };

    Slot* slot_of(Coroutine& co) noexcept
    {
        for (uint16_t i = 0U; i < kCapacity; ++i) {
            if (&slots_[i].co == &co) {
                return &slots_[i];
            }
        }
        return nullptr;
    }

    // Verify the slot's two stack-end guards at retirement. The magic is
    // re-armed on each arm(); a body that overran or underran its stack
    // overwrites the guard word, which this check turns into the cumulative
    // corrupted_guards_ counter (observable, never a crash).
    void check_guard(uint16_t slot) noexcept
    {
        constexpr uint64_t kGuard = 0xDEADBEEFCAFEBABEULL;
        std::byte* const base = stacks_[slot].bytes.data();
        uint64_t lo = 0U;
        uint64_t hi = 0U;
        std::memcpy(&lo, base, sizeof(kGuard));
        std::memcpy(&hi, base + kStackBytes - sizeof(kGuard), sizeof(kGuard));
        if ((kGuard != lo) || (kGuard != hi)) {
            corrupted_guards_.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    Slot slots_[kCapacity]{};
    StaticStackPool<StackBytes> stacks_[kCapacity]{};
    mutable std::mutex state_mutex_;
    std::atomic<uint16_t> corrupted_guards_{0U};
    std::atomic<uint32_t> notify_seq_{1U};
};

// -------------------------------------------------------------------------
// CoroSem: atomic permit + notify-sequence semaphore for coroutine bodies.
// THE coro-side replacement for the pthread mutex/cond emulation: no mutex,
// no condition variable, no periodic polling.
//
//   take():  acquire-load the permit -> CAS-decrement -> on failure snapshot
//            the notify sequence AFTER the re-check -> park (kSleep, far
//            future) with that snapshot. The re-check-then-snapshot-then-
//            park ordering closes the lost-wake window: a release() between
//            the re-check and the park bumps the sequence, so the executor's
//            park-sequence comparison wakes the taker on the next pass.
//   release(): release-store the permit increment, then notify() - every
//            parked taker re-runs its CAS; only permit holders proceed.
//
// Binary mode caps the permit at 1 (duplicate releases do not accumulate).
// The semaphore NEVER wakes a specific coroutine: notify publishes a state
// change only, and each woken body re-evaluates - the wake order is the
// executor's round-robin, never the releaser's choice.
//
// take() with a timeout returns false at the deadline; zero timeout = wait
// forever (deadline sentinel). The deadline re-park keeps the loop honest
// without periodic polling: a far-future deadline is only cut short by
// notify() or by the deadline itself.
//
// x86/ARM Linux: standard C++ atomics, no assembly, no atomic_flag spins.
// -------------------------------------------------------------------------
class CoroSem final {
public:
    static constexpr uint32_t kWaitForever = 0xFFFFFFFFU;

    explicit CoroSem(uint32_t initial_permits = 0U, bool binary = false)
        noexcept
        : permits_(initial_permits), binary_(binary)
    {
        static_assert(std::atomic<uint32_t>::is_always_lock_free,
                      "CoroSem requires lock-free 32-bit atomics; a toolchain "
                      "that cannot provide them must fail at compile time, "
                      "not silently lock");
    }
    CoroSem(const CoroSem&) = delete;
    CoroSem& operator=(const CoroSem&) = delete;

    // Consume one permit, parking the calling coroutine until one exists.
    // Returns false on timeout (or halt). Must run on the executor thread
    // inside a coroutine body (nullptr Coroutine::current() is a caller
    // bug; asserted by behavior: non-coro callers never park).
    template <typename Exec>
    bool take(Coroutine& self, Exec& exec,
              uint32_t timeout_us = kWaitForever) noexcept
    {
        const uint64_t deadline =
            (kWaitForever == timeout_us)
                ? 0xFFFFFFFFFFFFFFFFULL
                : now_ns() + static_cast<uint64_t>(timeout_us) * 1000ULL;
        for (;;) {
            if (try_consume()) {
                return true;
            }
            /* Re-check passed: snapshot AFTER it, BEFORE the park. */
            const uint32_t seq = exec.notify_sequence();
            if (try_consume()) {
                return true;   // raced in our favor between check and snap
            }
            (void)self.yield(YieldRequest{
                WaitReason::kSleep, deadline, 0U, seq});
            if (try_consume()) {
                return true;
            }
            if (now_ns() >= deadline) {
                return false;   // timeout: no permit arrived in the window
            }
            /* Spurious wake (sequence bumped by an unrelated release):
               loop re-checks the permit - no periodic polling involved. */
        }
    }

    // Publish one permit, then nudge the executor. Safe from any thread
    // (coroutine body, Dispatcher thread, bare pthread).
    template <typename Exec>
    void release(Exec& exec) noexcept
    {
        uint32_t cur = permits_.load(std::memory_order_acquire);
        for (;;) {
            const uint32_t next = binary_ ? 1U : cur + 1U;
            if (permits_.compare_exchange_weak(cur, next,
                                               std::memory_order_release,
                                               std::memory_order_acquire)) {
                break;
            }
            /* cur was refreshed by the failed CAS; binary keeps the cap. */
        }
        exec.notify();
    }

    // Non-consuming observation (diagnostics/tests).
    [[nodiscard]] uint32_t permits() const noexcept
    {
        return permits_.load(std::memory_order_acquire);
    }

private:
    bool try_consume() noexcept
    {
        uint32_t cur = permits_.load(std::memory_order_acquire);
        for (;;) {
            if (0U == cur) {
                return false;
            }
            if (permits_.compare_exchange_weak(cur, cur - 1U,
                                               std::memory_order_acquire,
                                               std::memory_order_acquire)) {
                return true;
            }
            /* cur refreshed; re-test. */
        }
    }

    std::atomic<uint32_t> permits_;
    bool binary_;
};

// -------------------------------------------------------------------------
// Blocking-IO escape hatch: one pthread running Fn to completion. NOT used
// by the coro core - include this only for genuinely blocking operations.
// -------------------------------------------------------------------------
template <typename Fn>
class WorkerThread final {
public:
    WorkerThread() noexcept = default;

    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;

    bool start(Fn entry, void* arg) noexcept
    {
        if (started_) {
            return false;
        }
        entry_ = entry;
        arg_ = arg;
        if (0 != pthread_create(&thread_, nullptr, &WorkerThread::trampoline,
                                static_cast<void*>(this))) {
            return false;
        }
        started_ = true;
        valid_ = true;
        return true;
    }

    ~WorkerThread()
    {
        if (valid_) {
            pthread_join(thread_, nullptr);
            valid_ = false;
        }
    }

    void join() noexcept
    {
        if (valid_) {
            pthread_join(thread_, nullptr);
            valid_ = false;
        }
    }

    bool joinable() const noexcept { return valid_; }

private:
    static void* trampoline(void* self) noexcept
    {
        WorkerThread* w = static_cast<WorkerThread*>(self);
        w->entry_(w->arg_);
        return nullptr;
    }

    pthread_t thread_{};
    Fn entry_ = nullptr;
    void* arg_ = nullptr;
    bool started_ = false;
    bool valid_ = false;
};

}  // namespace posix
}  // namespace coro
}  // namespace coact
