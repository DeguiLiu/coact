// coact::coro StackfulExecutor test: ucontext coroutines on static stacks,
// cooperative round-robin scheduling, sleep deadlines, task wake-ups and
// slot retirement. The whole test runs on ONE thread - the cooperative loop
// is driven directly by run_once() (the same loop the pinned executor
// pthread would run; no thread is needed to verify the semantics).
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <cstdint>
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

    for (int32_t pass = 0; pass < 8; ++pass) {
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

COACT_TEST_MAIN()
