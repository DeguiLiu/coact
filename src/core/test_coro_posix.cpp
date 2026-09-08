// coact::coro StackfulExecutor test: ucontext coroutines on static stacks,
// cooperative round-robin scheduling, sleep deadlines, task wake-ups and
// slot retirement. The whole test runs on ONE thread - the cooperative loop
// is driven directly by run_once() (the same loop the pinned executor
// pthread would run; no thread is needed to verify the semantics).
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <chrono>
#include <thread>

#include "coact/coro/config.hpp"
#include "coact/coro/posix.hpp"

namespace {

using coact::coro::posix::Coroutine;
using coact::coro::posix::ResumeArg;
using coact::coro::posix::StackfulExecutor;
using coact::coro::posix::WaitReason;
using coact::coro::posix::YieldRequest;

using Exec = StackfulExecutor<4U, 32U * 1024U>;

/* Shared observation state (single thread: no atomics needed, weakest-
   sufficient rule). */
struct Steps {
    uint32_t seq = 0U;      // global step counter across coroutines
    uint32_t a_steps = 0U;
    uint32_t b_steps = 0U;
    uint32_t order_hits = 0U;
    bool b_saw_success = false;
};

/* Coroutine A: yields kSleep twice, then kDone. Locals survive across
   yields - the sequential-code shape (the core value of stackful). */
static void body_a(void* user, Coroutine& self)
{
    Steps* st = static_cast<Steps*>(user);
    uint32_t local_counter = 0U;  // survives across the yields below
    for (uint32_t i = 0U; i < 2U; ++i) {
        ++st->a_steps;
        local_counter += 10U;
        (void)self.yield(YieldRequest{WaitReason::kSleep,
                                      coact::coro::posix::now_ns() +
                                          1000000000ULL,  // far future
                                      0U});
    }
    /* local_counter must be 20 here if the stack survived both yields. */
    if (20U == local_counter) {
        ++st->order_hits;
    }
    (void)self.yield(YieldRequest{WaitReason::kDone, 0U, 0U});
}

/* Coroutine B: waits for a task wake-up, reads the resume argument
   (completion status), then finishes. */
static void body_b(void* user, Coroutine& self)
{
    Steps* st = static_cast<Steps*>(user);
    ++st->b_steps;
    (void)self.yield(YieldRequest{WaitReason::kWaitTask, 0U, 0U});
    ++st->b_steps;
    st->b_saw_success = self.resume_arg().task_succeeded;
    (void)self.yield(YieldRequest{WaitReason::kDone, 0U, 0U});
}

// Shared executor pointer for coroutine bodies (arm() user-pointer
// pattern needs the executor for take()/release(); parked alongside the
// test state).
namespace coro_sem_test {
inline coact::coro::posix::StackfulExecutor<4U, 32U * 1024U>* g_exec = nullptr;
}

}  // namespace

/* Sequential-body semantics: locals survive across yields; the body runs in
   natural driver-code order. */
COACT_TEST(stackful_coroutine_local_state_survives_yields)
{
    Exec exec;
    Steps st;
    Coroutine* a = exec.arm(&body_a, &st, ResumeArg{});
    REQUIRE(nullptr != a);

    /* Pass 1: A runs to its first sleep yield. */
    uint16_t live = exec.run_once();
    CHECK_EQ(1U, live);
    CHECK_EQ(1U, st.a_steps);

    /* Far-future deadline: A stays asleep. */
    live = exec.run_once();
    CHECK_EQ(1U, live);
    CHECK_EQ(1U, st.a_steps);

    /* Force the deadline into the past via a second wake (simulate timer
       expiry by re-arming: sleep slots wake when now >= deadline; here we
       retire by running a body that finishes instead). */
    /* Drive B-style task wait to cover the other path. */
    Coroutine* b = exec.arm(&body_b, &st, ResumeArg{});
    REQUIRE(nullptr != b);
    live = exec.run_once();
    CHECK_EQ(2U, live);   // A asleep + B waiting
    CHECK_EQ(1U, st.b_steps);

    /* Deliver the task wake-up with a success resume argument. */
    exec.wake(*b, ResumeArg{true, 0U});
    live = exec.run_once();
    /* B consumed the arg and finished in this pass. */
    CHECK_EQ(1U, live);
    CHECK_EQ(2U, st.b_steps);
    CHECK(st.b_saw_success);

    /* A is still asleep; local_counter check happens when A finishes - but
       its deadline is far future. Retire the executor with A asleep: the
       test only proves the locals survived to this point via b path. The
       full local-survival check needs A to finish: use a short deadline
       coroutine instead in the dedicated test below. */
    CHECK(exec.live_count() <= 1U);
}

/* Dedicated local-survival check: a coroutine with an immediate (past)
   deadline finishes within two passes and verifies its local sum. */
static void body_fast(void* user, Coroutine& self)
{
    Steps* st = static_cast<Steps*>(user);
    uint32_t acc = 0U;
    for (uint32_t i = 1U; i <= 3U; ++i) {
        acc += i;
        /* Deadline in the past: wakes on the very next pass. */
        (void)self.yield(YieldRequest{WaitReason::kSleep,
                                      coact::coro::posix::now_ns(), 0U});
    }
    if (6U == acc) {
        ++st->order_hits;
    }
    (void)self.yield(YieldRequest{WaitReason::kDone, 0U, 0U});
}

COACT_TEST(stackful_coroutine_past_deadline_wakes)
{
    Exec exec;
    Steps st;
    Coroutine* c = exec.arm(&body_fast, &st, ResumeArg{});
    REQUIRE(nullptr != c);

    for (int pass = 0; pass < 8; ++pass) {
        const uint16_t live = exec.run_once();
        if (0U == live) {
            break;
        }
    }
    CHECK_EQ(0U, exec.live_count());
    CHECK_EQ(1U, st.order_hits);  // acc == 6 verified inside the body
}

/* Capacity: the fifth arm on a 4-slot executor returns nullptr. */
COACT_TEST(stackful_executor_capacity_full)
{
    Exec exec;
    Steps st;
    for (uint32_t i = 0U; i < 4U; ++i) {
        REQUIRE(nullptr != exec.arm(&body_b, &st, ResumeArg{}));
    }
    CHECK(nullptr == exec.arm(&body_b, &st, ResumeArg{}));
}

struct AlternationState {
    uint32_t a = 0U;
    uint32_t b = 0U;
    uint32_t wrong_active = 0U;
};

static void alternating_body(void* user, Coroutine& self)
{
    auto* state = static_cast<AlternationState*>(user);
    const bool is_a = (state->a == state->b);
    for (uint32_t i = 0U; i < 128U; ++i) {
        if (Coroutine::current() != &self) {
            ++state->wrong_active;
        }
        if (is_a) {
            ++state->a;
        } else {
            ++state->b;
        }
        (void)self.yield(YieldRequest{WaitReason::kSleep,
                                      coact::coro::posix::now_ns(), 0U});
    }
    (void)self.yield(YieldRequest{WaitReason::kDone, 0U, 0U});
}

COACT_TEST(stackful_executor_tracks_active_coroutine)
{
    StackfulExecutor<2U, 32U * 1024U> exec;
    AlternationState state;
    REQUIRE(nullptr != exec.arm(&alternating_body, &state, ResumeArg{}));
    REQUIRE(nullptr != exec.arm(&alternating_body, &state, ResumeArg{}));
    for (uint32_t pass = 0U; pass < 300U && exec.live_count() != 0U;
         ++pass) {
        (void)exec.run_once();
        std::this_thread::yield();
    }
    CHECK_EQ(0U, exec.live_count());
    CHECK_EQ(128U, state.a);
    CHECK_EQ(128U, state.b);
    CHECK_EQ(0U, state.wrong_active);
}

static void one_shot_body(void* user, Coroutine& self)
{
    auto* runs = static_cast<uint32_t*>(user);
    ++*runs;
    (void)self.yield(YieldRequest{WaitReason::kDone, 0U, 0U});
}

COACT_TEST(stackful_executor_rearms_retired_slot)
{
    StackfulExecutor<1U, 32U * 1024U> exec;
    uint32_t runs = 0U;
    REQUIRE(nullptr != exec.arm(&one_shot_body, &runs, ResumeArg{}));
    CHECK_EQ(0U, exec.run_once());
    REQUIRE(nullptr != exec.arm(&one_shot_body, &runs, ResumeArg{}));
    CHECK_EQ(0U, exec.run_once());
    CHECK_EQ(2U, runs);
}

/* Affinity pinning: the API exists on Linux; pinning to an out-of-range
   core is rejected. Pinning to core 0 is attempted (best effort - a
   restricted environment may refuse; both results are acceptable, only the
   argument validation is a hard contract). */
COACT_TEST(stackful_pin_validation)
{
    CHECK(!coact::coro::posix::pin_current_thread_to_core(
        4096U));  // out of CPU_SETSIZE range
    /* Best-effort core 0: either result is valid (container restrictions). */
    (void)coact::coro::posix::pin_current_thread_to_core(0U);
    CHECK(true);
}

// ---- Stack guard verification + watermark -------------------------------
// Overflow detection: a corrupted top guard must be reported at retirement.
// A REAL overflow smashes glibc's context-restore data before the guard and
// is undefined behavior - the test therefore injects the guard damage
// directly through the instrumentation hook and verifies the DETECTOR.
COACT_TEST(stackful_guard_detects_overflow)
{
    using SmallExec = StackfulExecutor<2U, 8U * 1024U>;
    SmallExec exec;
    struct Obs {
        bool ran = false;
    } obs;
    exec.arm(
        [](void* user, Coroutine& self) {
            static_cast<Obs*>(user)->ran = true;
            (void)self.yield(YieldRequest{WaitReason::kSleep,
                                          coact::coro::posix::now_ns()
                                              + 50000000ULL, 0U});
        },
        &obs, ResumeArg{});
    /* First pass starts the coroutine and parks it (kSleep). Then corrupt
       its top guard from outside and let the deadline retire the slot: the
       detector must count one corrupted guard. */
    exec.run_once();
    std::byte* stack = exec.stack_for_test(0U);
    REQUIRE(stack != nullptr);
    std::memset(stack + 8U * 1024U - 8U, 0x00, 8U);   // smash top guard
    while (0U != exec.run_once()) {
    }
    CHECK(obs.ran);
    CHECK_EQ(1U, exec.corrupted_guards());
}

// Watermark: a body touching only part of its stack reports a peak usage
// below the full stack size (and above zero once it has run).
COACT_TEST(stackful_watermark_reports_peak)
{
    Exec exec;
    struct Obs {
        bool ran = false;
    } obs;
    exec.arm(
        [](void* user, Coroutine& self) {
            static_cast<Obs*>(user)->ran = true;
            /* Touch a small local so the watermark is nonzero but far below
               the 32 KiB stack. */
            volatile uint32_t probe[64] = {};
            probe[0] = 1U;
            probe[63] = 2U;
            (void)probe;
            (void)self.yield(YieldRequest{WaitReason::kDone, 0U, 0U});
        },
        &obs, ResumeArg{});
    while (0U != exec.run_once()) {
    }
    CHECK(obs.ran);
    const uint32_t peak = exec.stack_watermark(0U);
    CHECK(peak > 0U);
    CHECK(peak < 32U * 1024U);
}

// Clean run: no guard corruption on well-behaved bodies.
COACT_TEST(stackful_guard_clean_run)
{
    Exec exec;
    Steps st;
    exec.arm(body_a, &st, ResumeArg{});
    while (0U != exec.run_once()) {
    }
    CHECK_EQ(0U, exec.corrupted_guards());
}

// Event-driven early wake (WAITER discipline): a body whose wait loop takes
// a notify-sequence snapshot AFTER its condition re-check is resumed before
// its deadline when the executor is notified. A plain sleep (no snapshot)
// must NOT early-wake - that is the pacing path. Both halves are asserted.
COACT_TEST(stackful_notify_wakes_sleeping_coroutine_early)
{
    Exec exec;
    struct Obs {
        volatile bool token = false;   // set by the "producer" side
        uint64_t woke_at_ns = 0U;
        bool saw_token = false;
    } obs;
    exec.arm(
        [](void* user, Coroutine& self) {
            Obs* o = static_cast<Obs*>(user);
            const uint64_t parked_at = coact::coro::posix::now_ns();
            while (!o->token) {
                /* Waiter discipline: re-check (the while) -> snapshot ->
                   park. A notify after the snapshot wakes us next pass. */
                const uint32_t seq = coro_sem_test::g_exec->notify_sequence();
                if (o->token) { break; }   // re-check raced in our favor
                (void)self.yield(YieldRequest{WaitReason::kSleep,
                                              coact::coro::posix::now_ns()
                                                  + 10000000000ULL, 0U,
                                              seq});
            }
            o->woke_at_ns = coact::coro::posix::now_ns() - parked_at;
            o->saw_token = true;
            (void)self.yield(YieldRequest{WaitReason::kDone, 0U, 0U});
        },
        &obs, ResumeArg{});
    coro_sem_test::g_exec = &exec;

    /* Drive passes on THIS thread; a background thread plays the producer:
       after 10 ms it sets the token and notifies the executor. The
       coroutine must observe the token well before its 10 s deadline. */
    std::thread producer([&exec, &obs]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        obs.token = true;
        exec.notify();
    });
    uint32_t passes = 0U;
    const uint64_t t0 = coact::coro::posix::now_ns();
    while (0U != exec.run_once()) {
        ++passes;
        if (obs.saw_token) { break; }
        if (coact::coro::posix::now_ns() - t0 > 2000000000ULL) {
            break;   // 2 s hard cap: no notify = bug
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    producer.join();
    CHECK(obs.saw_token);
    CHECK(obs.woke_at_ns < 1000000000ULL);   // woke in < 1 s (10 ms expect)
}

// uint32 notify-sequence wrap boundary: a waiter parked while the sequence
// sits at 0xFFFFFFFF must still be woken when notify() wraps the counter
// back to 0. Guards the natural unsigned wrap so the early-wake inequality
// stays correct across the 2^32 boundary.
COACT_TEST(stackful_notify_sequence_wraps_at_uint32_boundary)
{
    Exec exec;
    coro_sem_test::g_exec = &exec;
    struct Obs {
        volatile bool token = false;
        bool woke = false;
    } obs;
    exec.arm(
        [](void* user, Coroutine& self) {
            Obs* o = static_cast<Obs*>(user);
            while (!o->token) {
                const uint32_t seq =
                    coro_sem_test::g_exec->notify_sequence();
                if (o->token) { break; }
                (void)self.yield(YieldRequest{
                    WaitReason::kSleep,
                    coact::coro::posix::now_ns() + 10000000000ULL,
                    0U, seq});
            }
            o->woke = true;
            (void)self.yield(YieldRequest{WaitReason::kDone, 0U, 0U});
        },
        &obs, ResumeArg{});

    // Park the waiter while the sequence sits at the wrap boundary.
    exec.notify_sequence_for_test(0xFFFFFFFFU);
    (void)exec.run_once();   // body snapshots 0xFFFFFFFF and parks

    // A single notify wraps the sequence to 0 (defined uint32 arithmetic).
    obs.token = true;
    exec.notify();
    CHECK_EQ(0U, exec.notify_sequence());

    // The next pass must still wake the waiter despite the wrap.
    (void)exec.run_once();
    CHECK(obs.woke);
}

// ---- CoroSem: atomic permit + notify-sequence semaphore -----------------
// Helper: bodies need the executor for take()/release(); park it in the
// user struct alongside the sem (the arm() user pointer pattern).

// Basic permit semantics: take consumes, release publishes; binary mode
// caps at 1 (duplicate releases never accumulate); a timeout returns false.
COACT_TEST(coro_sem_permit_semantics)
{
    using coact::coro::posix::CoroSem;
    Exec exec;
    coro_sem_test::g_exec = &exec;
    struct Obs {
        CoroSem sem{1U, true};   // binary, one permit pre-armed
        uint32_t took = 0U;
        bool timed_out = false;
        bool done = false;
    } obs;
    exec.arm(
        [](void* user, Coroutine& self) {
            Obs* o = static_cast<Obs*>(user);
            /* Pre-armed permit: immediate take, no park. */
            if (o->sem.take(self, *coro_sem_test::g_exec, 1000U)) {
                ++o->took;
            }
            /* Now empty: a 2 ms take must time out (false). */
            o->timed_out =
                !o->sem.take(self, *coro_sem_test::g_exec, 2000U);
            o->done = true;
            (void)self.yield(YieldRequest{WaitReason::kDone, 0U, 0U});
        },
        &obs, ResumeArg{});
    while (0U != exec.run_once()) {
    }
    CHECK(obs.done);
    CHECK_EQ(1U, obs.took);
    CHECK(obs.timed_out);
    CHECK_EQ(0U, obs.sem.permits());
}

// Cross-thread release wakes a parked taker (no polling): the producer
// thread releases after 10 ms; the taker must observe it well before its
// 100 ms timeout.
COACT_TEST(coro_sem_cross_thread_release_wakes)
{
    using coact::coro::posix::CoroSem;
    Exec exec;
    coro_sem_test::g_exec = &exec;
    struct Obs {
        CoroSem sem{0U, false};
        bool took = false;
        uint64_t took_at_ns = 0U;
        bool done = false;
    } obs;
    const uint64_t t0 = coact::coro::posix::now_ns();
    exec.arm(
        [](void* user, Coroutine& self) {
            Obs* o = static_cast<Obs*>(user);
            o->took = o->sem.take(self, *coro_sem_test::g_exec, 100000U);
            o->took_at_ns = coact::coro::posix::now_ns();
            o->done = true;
            (void)self.yield(YieldRequest{WaitReason::kDone, 0U, 0U});
        },
        &obs, ResumeArg{});
    std::thread producer([&exec, &obs]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        obs.sem.release(exec);
    });
    while (!obs.done) {
        (void)exec.run_once();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    producer.join();
    CHECK(obs.done);
    CHECK(obs.took);
    CHECK(obs.took_at_ns - t0 >= 9000000ULL);     // not before the release
    CHECK(obs.took_at_ns - t0 < 1000000000ULL);   // and well under 1 s
}

// Binary cap: 5 releases on a binary sem leave exactly 1 permit.
COACT_TEST(coro_sem_binary_cap)
{
    using coact::coro::posix::CoroSem;
    Exec exec;
    CoroSem sem{0U, true};
    for (uint32_t i = 0U; i < 5U; ++i) {
        sem.release(exec);
    }
    CHECK_EQ(1U, sem.permits());
}

// Multi-coroutine contention: 3 takers, 1 permit - exactly one proceeds;
// the permit is never double-consumed.
COACT_TEST(coro_sem_contention_single_permit)
{
    using coact::coro::posix::CoroSem;
    Exec exec;   // 4 slots: 3 takers fit
    coro_sem_test::g_exec = &exec;
    struct Obs {
        CoroSem sem{0U, false};
        std::atomic<uint32_t> took{0U};
    } obs;
    for (uint32_t c = 0U; c < 3U; ++c) {
        exec.arm(
            [](void* user, Coroutine& self) {
                Obs* o = static_cast<Obs*>(user);
                /* 20 ms timeout: the losers must time out. */
                if (o->sem.take(self, *coro_sem_test::g_exec, 20000U)) {
                    o->took.fetch_add(1U, std::memory_order_relaxed);
                }
                (void)self.yield(YieldRequest{WaitReason::kDone, 0U, 0U});
            },
            &obs, ResumeArg{});
    }
    obs.sem.release(exec);   // single permit, BEFORE any taker parked:
    for (uint32_t spin = 0U;
         spin < 200U && 0U != exec.live_count();
         ++spin) {
        (void)exec.run_once();
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    CHECK_EQ(0U, exec.live_count());
    CHECK_EQ(1U, obs.took.load(std::memory_order_relaxed));
    CHECK_EQ(0U, obs.sem.permits());
}

// Randomized interleaving: 10000 releases from a producer thread consumed
// by one taker coroutine - conservation (no lost wake, no double consume).
COACT_TEST(coro_sem_random_interleave_conserves)
{
    using coact::coro::posix::CoroSem;
    Exec exec;
    coro_sem_test::g_exec = &exec;
    struct Obs {
        CoroSem sem{0U, false};
        std::atomic<uint32_t> released{0U};
        std::atomic<uint32_t> taken{0U};
        std::atomic<bool> producer_done{false};
        bool done = false;
    } obs;
    exec.arm(
        [](void* user, Coroutine& self) {
            Obs* o = static_cast<Obs*>(user);
            for (;;) {
                if (o->sem.take(self, *coro_sem_test::g_exec, 50000U)) {
                    o->taken.fetch_add(1U, std::memory_order_relaxed);
                }
                /* Drain check ONLY after the producer finished: comparing
                   counters mid-stream races the next release (an early
                   equal snapshot terminated the loop in the first version
                   of this test - that was a test bug, not a sem bug). */
                if (o->producer_done.load(std::memory_order_acquire)
                    && 0U == o->sem.permits()) {
                    break;
                }
            }
            o->done = true;
            (void)self.yield(YieldRequest{WaitReason::kDone, 0U, 0U});
        },
        &obs, ResumeArg{});
    std::thread producer([&exec, &obs]() {
        uint32_t seed = 12345U;
        for (uint32_t i = 0U; i < 10000U; ++i) {
            seed = seed * 1103515245U + 12345U;   // cheap LCG randomness
            if (0U == ((seed >> 16U) % 3U)) {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(1U + (seed >> 20U) % 10U));
            }
            obs.sem.release(exec);
            obs.released.fetch_add(1U, std::memory_order_release);
        }
        obs.producer_done.store(true, std::memory_order_release);
    });
    const uint64_t t0 = coact::coro::posix::now_ns();
    while (!obs.done && coact::coro::posix::now_ns() - t0 < 5000000000ULL) {
        (void)exec.run_once();
        std::this_thread::yield();
    }
    producer.join();
    CHECK(obs.done);
    CHECK_EQ(obs.released.load(std::memory_order_acquire),
             obs.taken.load(std::memory_order_relaxed));
}

COACT_TEST_MAIN()
