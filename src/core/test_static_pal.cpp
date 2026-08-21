// coact RT-Thread static PAL tests (design §7.5). TDD: this file was written
// FIRST against the not-yet-existing RtThreadResources / explicit-initialize /
// error-returning-start API; it is RED against the historical PAL (which
// ignored startup return values, had no explicit InitError, no freeze, and
// owned a hard-coded 8-slot global table).
//
// Coverage (design §7.5 + §7.4):
//   - RtThreadResources: caller-provided static TCB / semaphores / stack /
//     ContextSlot[N]; constructor only saves references.
//   - initialize(): rt_sem_init + rt_thread_init, definite InitError on every
//     failure path (sem / thread / stack-too-large), no dynamic fallback.
//   - start_dispatcher(): returns InitError, kOk only after
//     rt_thread_startup()==RT_EOK; second start and stop-then-start rejected;
//     startup failure never enters started.
//   - ContextSlot table: table-full error, duplicate-tid rejection, frozen
//     after start; the Dispatcher needs no slot (identified via its static
//     TCB), so a full table cannot block it.
//   - ISR recognition via rt_interrupt_get_nest().
//   - ClockOps static function table injection (monotonicity / conversion).
//   - Dispatcher single-core profile assembly (C's leftover): Runtime passes
//     RttSingleCoreProfile -> ImmediateReclaimer; host default stays batched.
// SPDX-License-Identifier: MIT

#include "test/test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <type_traits>
#include <unistd.h>

#ifndef COACT_RTT_STUB
#define COACT_RTT_STUB
#endif
#include "test/rtthread_stub.h"

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/coordinator.hpp"
#include "coact/dispatcher.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/monitor.hpp"
#include "coact/pal.hpp"
#include "coact/pal_rtthread.hpp"
#include "coact/pool.hpp"
#include "coact/queue.hpp"
#include "coact/runtime.hpp"
#include "coact/staging.hpp"

namespace {

/* ---- Stub-thread helper (distinct rt_thread TCBs for ContextSlot tests) --- */
struct ThreadRunCtx {
    void (*fn)(void*);
    void* arg;
};
static void run_thread_entry(void* p) noexcept
{
    auto* c = static_cast<ThreadRunCtx*>(p);
    c->fn(c->arg);
}
/* Runs fn(arg) on a fresh stub rt_thread with its own TCB, joined before
   returning. rt_thread_self() inside the entry == the TCB address, so each
   run_in_thread call registers a distinct tid. */
static void run_in_thread(rt_thread* tcb, void (*fn)(void*), void* arg) noexcept
{
    ThreadRunCtx ctx{fn, arg};
    rt_thread_init(tcb, "st", &run_thread_entry, &ctx, nullptr, 4096, 10U, 10U);
    rt_thread_startup(tcb);
    if (tcb->tid != pthread_t{}) {
        pthread_join(tcb->tid, nullptr);
    }
}

/* Registration helpers shared by the ContextSlot tests. */
static coact::pal::RtThread* g_reg_pal = nullptr;
static void reg_once_fn(void* p) noexcept
{
    auto* ok = static_cast<int*>(p);
    *ok = (g_reg_pal->register_current_task(10U)) ? 1 : 0;
}
struct DuoResult {
    int first;
    int second;
};
static void reg_twice_fn(void* p) noexcept
{
    auto* r = static_cast<DuoResult*>(p);
    r->first  = (g_reg_pal->register_current_task(10U)) ? 1 : 0;
    r->second = (g_reg_pal->register_current_task(10U)) ? 1 : 0;
}

/* ---- Basic lifecycle ----------------------------------------------------- */

COACT_TEST(static_pal_init_and_start_success)
{
    stub_reset_faults();
    coact::pal::RtThreadResources<4096U, 4U> res;
    coact::pal::RtThread pal(res);
    CHECK(!coact::pal::RtThread::in_dispatcher_thread());
    CHECK_EQ(static_cast<int>(coact::pal::InitError::kOk),
             static_cast<int>(pal.initialize()));

    std::atomic<int> ran{0};
    auto entry = [](void* ctx) {
        static_cast<std::atomic<int>*>(ctx)->store(1, std::memory_order_relaxed);
    };
    CHECK_EQ(static_cast<int>(coact::pal::InitError::kOk),
             static_cast<int>(pal.start_dispatcher(entry, &ran)));
    pal.join_dispatcher();
    CHECK_EQ(1, ran.load());
}

COACT_TEST(static_pal_init_sem_fail)
{
    stub_reset_faults();
    coact::pal::RtThreadResources<4096U, 4U> res;
    coact::pal::RtThread pal(res);
    stub_sem_init_fault() = -RT_ENOMEM;
    CHECK_EQ(static_cast<int>(coact::pal::InitError::kSemInitFailed),
             static_cast<int>(pal.initialize()));
    stub_reset_faults();
    /* A start on a failed init propagates the init error, never a fake
       success and never a crash. */
    CHECK_EQ(static_cast<int>(coact::pal::InitError::kSemInitFailed),
             static_cast<int>(pal.start_dispatcher(nullptr, nullptr)));
}

COACT_TEST(static_pal_init_thread_fail)
{
    stub_reset_faults();
    coact::pal::RtThreadResources<4096U, 4U> res;
    coact::pal::RtThread pal(res);
    stub_thread_init_fault() = -RT_ENOMEM;
    CHECK_EQ(static_cast<int>(coact::pal::InitError::kThreadInitFailed),
             static_cast<int>(pal.initialize()));
    stub_reset_faults();
}

COACT_TEST(static_pal_stack_too_large_rejected)
{
    stub_reset_faults();
    coact::pal::RtThreadResources<2048U, 4U> res;   /* small static stack */
    coact::pal::RtThread pal(res);
    pal.set_dispatcher_stack_bytes(4096U);          /* request exceeds resource */
    CHECK_EQ(static_cast<int>(coact::pal::InitError::kStackTooLarge),
             static_cast<int>(pal.initialize()));
}

COACT_TEST(static_pal_dispatcher_wait_ticks_preserve_finite_timeout)
{
    CHECK_EQ(static_cast<rt_int32_t>(RT_WAITING_FOREVER),
             coact::pal::detail::dispatcher_wait_ticks(0U));
    CHECK_EQ(static_cast<rt_int32_t>(1),
             coact::pal::detail::dispatcher_wait_ticks(1U));
    CHECK_EQ(static_cast<rt_int32_t>(RT_WAITING_FOREVER - 1),
             coact::pal::detail::dispatcher_wait_ticks(
                 static_cast<uint32_t>(RT_WAITING_FOREVER)));
    CHECK_EQ(static_cast<rt_int32_t>(RT_WAITING_FOREVER - 1),
             coact::pal::detail::dispatcher_wait_ticks(0xFFFFFFFFU));
}

static_assert(coact::pal::detail::dispatcher_wait_ticks(1U) == 1,
              "1 kHz dispatcher waits must remain a compile-time 1:1 conversion");

/* ---- One-time start / stop ----------------------------------------------- */

COACT_TEST(static_pal_second_start_rejected)
{
    stub_reset_faults();
    coact::pal::RtThreadResources<4096U, 4U> res;
    coact::pal::RtThread pal(res);
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));

    std::atomic<int> ran{0};
    auto entry = [](void* ctx) {
        static_cast<std::atomic<int>*>(ctx)->store(1, std::memory_order_relaxed);
    };
    CHECK_EQ(static_cast<int>(coact::pal::InitError::kOk),
             static_cast<int>(pal.start_dispatcher(entry, &ran)));
    CHECK_EQ(static_cast<int>(coact::pal::InitError::kAlreadyStarted),
             static_cast<int>(pal.start_dispatcher(entry, &ran)));
    pal.join_dispatcher();
    CHECK_EQ(1, ran.load());
}

COACT_TEST(static_pal_stop_then_start_rejected)
{
    stub_reset_faults();
    coact::pal::RtThreadResources<4096U, 4U> res;
    coact::pal::RtThread pal(res);
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));

    std::atomic<int> ran{0};
    auto entry = [](void* ctx) {
        static_cast<std::atomic<int>*>(ctx)->store(1, std::memory_order_relaxed);
    };
    CHECK_EQ(static_cast<int>(coact::pal::InitError::kOk),
             static_cast<int>(pal.start_dispatcher(entry, &ran)));
    pal.join_dispatcher();   /* stop: dispatcher joined, PAL is terminal */
    CHECK_EQ(1, ran.load());
    CHECK_EQ(static_cast<int>(coact::pal::InitError::kAlreadyStarted),
             static_cast<int>(pal.start_dispatcher(entry, &ran)));
}

COACT_TEST(static_pal_startup_fail_does_not_start)
{
    stub_reset_faults();
    coact::pal::RtThreadResources<4096U, 4U> res;
    coact::pal::RtThread pal(res);
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));

    std::atomic<int> ran{0};
    auto entry = [](void* ctx) {
        static_cast<std::atomic<int>*>(ctx)->store(1, std::memory_order_relaxed);
    };
    stub_thread_startup_fault() = -RT_ENOMEM;
    CHECK_EQ(static_cast<int>(coact::pal::InitError::kThreadStartFailed),
             static_cast<int>(pal.start_dispatcher(entry, &ran)));
    stub_reset_faults();

    CHECK_EQ(0, ran.load());               /* thread never ran */
    CHECK_NE(static_cast<int>(coact::pal::InitError::kOk),
             static_cast<int>(pal.start_dispatcher(entry, &ran)));  /* rejected */
    pal.join_dispatcher();                 /* no-op, must not hang */
    CHECK_EQ(0, ran.load());
}

/* ---- ContextSlot table --------------------------------------------------- */

COACT_TEST(static_pal_context_slots_full_rejected)
{
    stub_reset_faults();
    coact::pal::RtThreadResources<4096U, 2U> res;   /* only 2 slots */
    coact::pal::RtThread pal(res);
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));
    g_reg_pal = &pal;

    rt_thread t1;
    rt_thread t2;
    rt_thread t3;
    int a1 = -1;
    int a2 = -1;
    int a3 = -1;
    run_in_thread(&t1, reg_once_fn, &a1);
    run_in_thread(&t2, reg_once_fn, &a2);
    run_in_thread(&t3, reg_once_fn, &a3);
    CHECK_EQ(1, a1);   /* slots 0,1 occupied */
    CHECK_EQ(1, a2);
    CHECK_EQ(0, a3);   /* table full -> registration error */
}

COACT_TEST(static_pal_context_duplicate_rejected)
{
    stub_reset_faults();
    coact::pal::RtThreadResources<4096U, 4U> res;
    coact::pal::RtThread pal(res);
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));
    g_reg_pal = &pal;

    rt_thread t;
    DuoResult r{-1, -1};
    run_in_thread(&t, reg_twice_fn, &r);
    CHECK_EQ(1, r.first);    /* first registration occupies a slot */
    CHECK_EQ(0, r.second);   /* same tid re-register rejected */
}

/* Freeze: a producer that tries to register after the Dispatcher started is
   rejected, while a producer that registered before start keeps working. */
static std::atomic<int> g_reg_go{0};
static void reg_after_start_fn(void* p) noexcept
{
    while (0 == g_reg_go.load(std::memory_order_acquire)) {
        sched_yield();
    }
    *static_cast<int*>(p) = g_reg_pal->register_current_task(10U) ? 1 : 0;
}

COACT_TEST(static_pal_context_frozen_after_start)
{
    stub_reset_faults();
    coact::pal::RtThreadResources<4096U, 4U> res;
    coact::pal::RtThread pal(res);
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));
    g_reg_pal = &pal;
    g_reg_go.store(0, std::memory_order_release);

    std::atomic<int> ran{0};
    auto entry = [](void* ctx) {
        static_cast<std::atomic<int>*>(ctx)->store(1, std::memory_order_relaxed);
    };

    /* The producer thread blocks until the Dispatcher is started. */
    rt_thread t;
    int reg_result = -1;
    ThreadRunCtx ctx{&reg_after_start_fn, &reg_result};
    rt_thread_init(&t, "reg", &run_thread_entry, &ctx, nullptr, 4096, 10U, 10U);
    rt_thread_startup(&t);

    CHECK_EQ(static_cast<int>(coact::pal::InitError::kOk),
             static_cast<int>(pal.start_dispatcher(entry, &ran)));
    g_reg_go.store(1, std::memory_order_release);
    if (t.tid != pthread_t{}) {
        pthread_join(t.tid, nullptr);
    }

    /* join_dispatcher() blocks until the Dispatcher entry has returned, so
       ran is settled before the check (no scheduler race). */
    pal.join_dispatcher();
    CHECK_EQ(0, reg_result);   /* table frozen after start */
    CHECK_EQ(1, ran.load());
}

/* The Dispatcher identifies itself via its static TCB, so it needs NO slot:
   a fully occupied table cannot block it (design §7.5 "dispatcher 槽预留不可
   失败"). */
static std::atomic<int> g_disp_kind_seen{0};
static void disp_ctx_probe(void* p) noexcept
{
    auto* pal = static_cast<coact::pal::RtThread*>(p);
    const coact::ExecutionContext ctx = pal->current_context();
    g_disp_kind_seen.store(static_cast<int>(ctx.kind), std::memory_order_relaxed);
}

COACT_TEST(static_pal_dispatcher_needs_no_slot)
{
    stub_reset_faults();
    coact::pal::RtThreadResources<4096U, 4U> res;
    coact::pal::RtThread pal(res);
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));
    g_reg_pal = &pal;

    rt_thread t1;
    rt_thread t2;
    rt_thread t3;
    rt_thread t4;
    int a1 = -1;
    int a2 = -1;
    int a3 = -1;
    int a4 = -1;
    run_in_thread(&t1, reg_once_fn, &a1);
    run_in_thread(&t2, reg_once_fn, &a2);
    run_in_thread(&t3, reg_once_fn, &a3);
    run_in_thread(&t4, reg_once_fn, &a4);
    CHECK_EQ(1, a1);
    CHECK_EQ(1, a2);
    CHECK_EQ(1, a3);
    CHECK_EQ(1, a4);   /* all 4 slots full */

    g_disp_kind_seen.store(-1, std::memory_order_relaxed);
    CHECK_EQ(static_cast<int>(coact::pal::InitError::kOk),
             static_cast<int>(pal.start_dispatcher(&disp_ctx_probe, &pal)));
    pal.join_dispatcher();
    CHECK_EQ(static_cast<int>(coact::ContextKind::Dispatcher),
             g_disp_kind_seen.load());
}

/* ---- ISR / context ------------------------------------------------------ */

COACT_TEST(static_pal_current_context_isr)
{
    stub_reset_faults();
    coact::pal::RtThreadResources<4096U, 4U> res;
    coact::pal::RtThread pal(res);
    stub_set_isr_nest(1U);
    const coact::ExecutionContext isr_ctx = pal.current_context();
    stub_set_isr_nest(0U);
    CHECK_EQ(static_cast<int>(coact::ContextKind::Isr),
             static_cast<int>(isr_ctx.kind));
}

/* ---- ClockOps static function table ------------------------------------- */

struct MockCounter {
    uint64_t counter;
};
static uint64_t mock_counter_read(void* ctx) noexcept
{
    return static_cast<MockCounter*>(ctx)->counter;
}

COACT_TEST(static_pal_clock_ops)
{
    stub_reset_faults();
    coact::pal::RtThreadResources<4096U, 4U> res;
    coact::pal::RtThread pal(res);
    MockCounter mc{1000U};
    coact::pal::ClockOps ops;
    ops.read_counter = &mock_counter_read;
    ops.frequency_hz = 1000000U;   /* 1 MHz */
    ops.ctx = &mc;
    pal.set_clock_ops(ops);

    /* 1000 ticks @ 1 MHz == 1 ms == 1,000,000 ns. */
    CHECK_EQ(1000000ULL, pal.monotonic_ns());
    CHECK_EQ(1000ULL, pal.clock_resolution_ns());

    mc.counter = 2000U;
    CHECK(pal.monotonic_ns() > 1000000ULL);   /* monotonic across increments */

    /* Default PAL still reads the RT tick clock (backward compatible). */
    coact::pal::RtThread pal2;
    const uint64_t t0 = pal2.monotonic_ns();
    usleep(2000);
    const uint64_t t1 = pal2.monotonic_ns();
    CHECK(t1 >= t0);
}

COACT_TEST(static_pal_clock_ops_avoids_counter_product_overflow)
{
    stub_reset_faults();
    coact::pal::RtThreadResources<4096U, 4U> res;
    coact::pal::RtThread pal(res);
    MockCounter mc{18446744074ULL};
    coact::pal::ClockOps ops;
    ops.read_counter = &mock_counter_read;
    ops.frequency_hz = 10000000U;
    ops.ctx = &mc;
    pal.set_clock_ops(ops);

    // This value is just past UINT64_MAX / 1e9. Multiplying before dividing
    // wraps, while quotient/remainder conversion remains exact.
    CHECK_EQ(1844674407400ULL, pal.monotonic_ns());
}

COACT_TEST(static_pal_clock_ops_extends_32bit_counter_wrap)
{
    stub_reset_faults();
    coact::pal::RtThreadResources<4096U, 4U> res;
    coact::pal::RtThread pal(res);
    MockCounter mc{0xFFFFFFFCULL};
    coact::pal::ClockOps ops;
    ops.read_counter = &mock_counter_read;
    ops.frequency_hz = 100U;
    ops.ctx = &mc;
    ops.counter_bits = 32U;
    pal.set_clock_ops(ops);

    const uint64_t before_wrap = pal.monotonic_ns();
    mc.counter = 5U;
    const uint64_t after_wrap = pal.monotonic_ns();
    CHECK_EQ(90000000ULL, after_wrap - before_wrap);
    CHECK(after_wrap > before_wrap);
}

/* ---- Dispatcher single-core profile assembly (C's leftover) -------------- */

using StagingRT = coact::Staging<coact::DefaultConfig,
                                 coact::pal::RtThread::QueueBackend>;
using DispHost   = coact::Dispatcher<StagingRT, coact::pal::RtThread>;
using DispSingle = coact::Dispatcher<StagingRT, coact::pal::RtThread,
                                     coact::RttSingleCoreProfile>;

static_assert(std::is_same<DispHost::ReclaimerT,
                           coact::BatchedReclaimer<DispHost::kBatchPools>>::value,
              "host default Dispatcher profile must stay batched (design §7.4)");
static_assert(std::is_same<DispSingle::ReclaimerT,
                           coact::ImmediateReclaimer>::value,
              "single-core Dispatcher profile must select immediate reclaim");

/* End-to-end single-core fixture (file scope: local classes cannot carry
   static data members such as Traits::kRtcBudgetNs). */
static std::atomic<int> g_single_counter{0};
struct SingleCtx {};
static void single_on_evt(SingleCtx&, const coact::Event&) noexcept
{
    g_single_counter.fetch_add(1, std::memory_order_relaxed);
}
static bool single_ok_guard(const SingleCtx&, const coact::Event&) noexcept
{
    return true;
}
static const coact::StateDef<SingleCtx> kSingleStates[] = {
    { -1, nullptr, nullptr, nullptr },
    { 0, nullptr, nullptr, nullptr },
};
static const coact::TransitionDef<SingleCtx> kSingleTrans[] = {
    { 1, 1U, 1, coact::TransitionKind::Internal, single_ok_guard, single_on_evt },
};
struct SingleTraits {
    static coact::LogicalPrio logical_prio() noexcept { return 20U; }
    static coact::PriorityClass priority_class() noexcept
    {
        return coact::PriorityClass::Normal;
    }
    static bool direct_eligible() noexcept { return false; }
    static bool isr_direct_safe() noexcept { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
using SingleAo = coact::Ao<SingleCtx, coact::Hsm<SingleCtx>, SingleTraits>;

COACT_TEST(static_pal_dispatcher_profile_assembly)
{
    /* Runtime must forward the board profile into the Dispatcher (C leftover). */
    using RtHost   = coact::Runtime<coact::DefaultConfig, coact::pal::RtThread>;
    using RtSingle = coact::Runtime<coact::DefaultConfig, coact::pal::RtThread,
                                    coact::RttSingleCoreProfile>;
    static_assert(std::is_same<typename RtHost::DispatcherType::ReclaimerT,
                               coact::BatchedReclaimer<
                                   RtHost::DispatcherType::kBatchPools>>::value,
                  "Runtime default (host) must keep batched reclaim");
    static_assert(std::is_same<typename RtSingle::DispatcherType::ReclaimerT,
                               coact::ImmediateReclaimer>::value,
                  "Runtime single-core assembly must select immediate reclaim");

    /* End-to-end single-core assembly: RttSingleCoreProfile pool + Runtime. */
    stub_reset_faults();
    coact::pal::RtThreadResources<4096U, 4U> res;
    coact::pal::RtThread pal(res);

    static constexpr uint16_t kBlk = 16U;
    static constexpr uint16_t kCap = 16U;
    alignas(16) static unsigned char storage[kBlk * kCap + kBlk];
    coact::EventPool<kBlk, kCap, coact::RttSingleCoreProfile> pool;
    pool.init(storage, sizeof(storage), coact::make_critical_section(pal));

    SingleAo ao(kSingleStates, 2U, kSingleTrans, 1U, 1, 4U);
    coact::Event init_e{0U, 0U, 0U};
    ao.init(init_e);

    coact::Runtime<coact::DefaultConfig, coact::pal::RtThread,
                   coact::RttSingleCoreProfile> rt(pal);
    REQUIRE(rt.bind(&ao));
    REQUIRE(rt.initialize());
    REQUIRE(rt.start());

    g_single_counter.store(0, std::memory_order_relaxed);
    constexpr int kN = 12;
    for (int i = 0; i < kN; ++i) {
        coact::Event* e = pool.alloc(1U);
        REQUIRE(e != nullptr);
        coact::EventQos qos{false, false};
        coact::SubmitResult r = rt.coordinator().submit_from_task(coact::TargetId(1U), e, qos);
        REQUIRE(r.disposition != coact::SubmitDisposition::RejectedFull);
    }
    for (int w = 0; w < 200; ++w) {
        if (g_single_counter.load(std::memory_order_relaxed) >= kN) {
            break;
        }
        usleep(5000);
    }
    rt.stop();
    CHECK_EQ(kN, g_single_counter.load(std::memory_order_relaxed));
    CHECK_EQ(0U, pool.used());
}

}  // namespace

COACT_TEST_MAIN()
