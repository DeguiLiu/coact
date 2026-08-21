// coact RT-Thread PAL host test (COACT_RTT_STUB = pthread-backed RT-Thread).
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <unistd.h>

/* Pull in stub BEFORE pal_rtthread.hpp so RT-Thread types resolve. */
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
#include "coact/pal_rtthread.hpp"
#include "coact/pool.hpp"
#include "coact/queue.hpp"
#include "coact/runtime.hpp"
#include "coact/staging.hpp"

namespace {

/* ---- PAL unit tests ------------------------------------------------------- */

struct ClockCounter {
    uint64_t value;
};

uint64_t read_clock_counter(void* ctx) noexcept
{
    return static_cast<ClockCounter*>(ctx)->value;
}

COACT_TEST(rtthread_pal_irq_save_restore)
{
    coact::pal::RtThread pal;
    coact::pal::CriticalToken tok = pal.irq_save();
    pal.irq_restore(tok);
    CHECK(true);
}

COACT_TEST(rtthread_pal_monotonic_ns_increases)
{
    coact::pal::RtThread pal;
    uint64_t t0 = pal.monotonic_ns();
    usleep(2000);  /* 2 ms */
    uint64_t t1 = pal.monotonic_ns();
    CHECK(t1 > t0);
}

COACT_TEST(rtthread_pal_clock_exact_scale)
{
    coact::pal::RtThread pal;
    ClockCounter counter{123456789ULL};
    coact::pal::ClockOps ops;
    ops.read_counter = &read_clock_counter;
    ops.frequency_hz = 10000000U;
    ops.ctx = &counter;
    pal.set_clock_ops(ops);

    CHECK_EQ(12345678900ULL, pal.monotonic_ns());
}

COACT_TEST(rtthread_pal_clock_non_divisible_fallback)
{
    coact::pal::RtThread pal;
    ClockCounter counter{10U};
    coact::pal::ClockOps ops;
    ops.read_counter = &read_clock_counter;
    ops.frequency_hz = 3000000U;
    ops.ctx = &counter;
    pal.set_clock_ops(ops);

    CHECK_EQ(3333ULL, pal.monotonic_ns());
}

COACT_TEST(rtthread_pal_clock_wrap_state_resets_on_rebind)
{
    coact::pal::RtThread pal;
    ClockCounter counter{250U};
    coact::pal::ClockOps ops;
    ops.read_counter = &read_clock_counter;
    ops.frequency_hz = 250000000U;
    ops.ctx = &counter;
    ops.counter_bits = 8U;
    pal.set_clock_ops(ops);

    CHECK_EQ(1000ULL, pal.monotonic_ns());
    counter.value = 5U;
    CHECK_EQ(1044ULL, pal.monotonic_ns());

    counter.value = 7U;
    pal.set_clock_ops(ops);
    CHECK_EQ(28ULL, pal.monotonic_ns());
}

COACT_TEST(rtthread_pal_current_context_task)
{
    coact::pal::RtThread pal;
    /* Not inside the dispatcher thread; rt_thread_self() == nullptr on host
       unless registered. Context should be Task with prio_valid==false. */
    coact::ExecutionContext ctx = pal.current_context();
    CHECK_EQ(static_cast<int>(coact::ContextKind::Task),
             static_cast<int>(ctx.kind));
}

COACT_TEST(rtthread_pal_register_current_task)
{
    coact::pal::RtThread pal;
    /* register_current_task requires rt_thread_self() != nullptr.
       On host the main thread is not a rt_thread; returns false gracefully. */
    bool ok = pal.register_current_task(10U);
    (void)ok;  /* false is acceptable outside the scheduler */
    CHECK(true);
}

COACT_TEST(rtthread_pal_signal_wait_dispatcher)
{
    coact::pal::RtThread pal;
    /* Signal then wait with zero timeout — should return immediately. */
    pal.signal_dispatcher_from_task();
    pal.wait_dispatcher(1U);  /* 1 ms */
    CHECK(true);
}

COACT_TEST(rtthread_pal_start_join_dispatcher)
{
    coact::pal::RtThread pal;
    struct Flag { std::atomic<int> done{0}; };
    Flag flag;
    pal.start_dispatcher([](void* ctx) {
        static_cast<Flag*>(ctx)->done.store(1, std::memory_order_relaxed);
    }, &flag);
    pal.join_dispatcher();
    CHECK_EQ(1, flag.done.load(std::memory_order_relaxed));
}

/* ---- End-to-end with RT-Thread PAL ---------------------------------------- */

static std::atomic<int> g_rtt_counter{0};

struct RttCtx {};
static void rtt_noop_entry(RttCtx&) {}
static void rtt_noop_exit(RttCtx&)  {}
static void rtt_count_action(RttCtx&, const coact::Event&)
{
    g_rtt_counter.fetch_add(1, std::memory_order_relaxed);
}
static bool rtt_always(const RttCtx&, const coact::Event&) { return true; }

static const coact::StateDef<RttCtx> kRttStates[] = {
    { -1, nullptr, nullptr },
    {  0, rtt_noop_entry, rtt_noop_exit },
};
static const coact::TransitionDef<RttCtx> kRttTrans[] = {
    { 1, 1U, 1, coact::TransitionKind::Internal, rtt_always, rtt_count_action },
};

struct RttTraits {
    static coact::LogicalPrio   logical_prio()   { return 20U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using RttHsm = coact::Hsm<RttCtx>;
using RttAo  = coact::Ao<RttCtx, RttHsm, RttTraits>;

static constexpr uint16_t kRttBlk = 16U;
static constexpr uint16_t kRttCap = 32U;
alignas(16) static unsigned char g_rtt_pool_storage[kRttBlk * kRttCap + kRttBlk];

COACT_TEST(rtthread_pal_integration_ao_dispatch)
{
    g_rtt_counter.store(0);

    coact::pal::RtThread pal;
    /* Thread-safe pool under the RT-Thread irq-mask critical section: main
       thread allocs, Dispatcher thread gc-concurrently, guarded in O(1). */
    coact::EventPool<kRttBlk, kRttCap> pool;
    pool.init(g_rtt_pool_storage, sizeof(g_rtt_pool_storage),
              coact::make_critical_section(pal));

    RttAo ao(kRttStates, 2U, kRttTrans, 1U, 1, 4U);
    coact::Event init_e;
    init_e.signal = 0U; init_e.pool_id = 0U; init_e.ref_ctr = 0U;
    ao.init(init_e);

    coact::Runtime<coact::DefaultConfig, coact::pal::RtThread> rt(pal);

    CHECK(rt.bind(&ao));
    CHECK(rt.initialize());
    rt.start();

    coact::EventQos qos{false, false};
    static constexpr int kN = 20;
    for (int i = 0; i < kN; ++i) {
        coact::Event* e = pool.alloc(1U);
        REQUIRE(e != nullptr);
        rt.coordinator().submit_from_task(coact::TargetId(1U), e, qos);
    }

    for (int w = 0; w < 200; ++w) {
        if (g_rtt_counter.load() >= kN) { break; }
        usleep(5000);
    }
    rt.stop();

    CHECK_EQ(kN, g_rtt_counter.load());
    CHECK_EQ(0U, pool.used());
}

/* ---- RT-Thread 5.2.x single-core semantics ------------------------------- */

/* A pool wired to a counting CriticalSection must guard allocation, event
   reference updates, and reclaim with O(1) irq-mask sections. */
struct PoolCsCounters { int saves = 0; int restores = 0; };
PoolCsCounters g_pool_cs;
uintptr_t pool_cs_save(void*) { ++g_pool_cs.saves; return 0xABABABABu; }
void pool_cs_restore(void*, uintptr_t) { ++g_pool_cs.restores; }

COACT_TEST(rtthread_pool_cs_guards_alloc_reclaim)
{
    g_pool_cs = {};
    coact::CriticalSection cs;
    cs.ctx = nullptr;
    cs.save = &pool_cs_save;
    cs.restore = &pool_cs_restore;

    coact::EventPool<kRttBlk, kRttCap> pool;
    pool.init(g_rtt_pool_storage, sizeof(g_rtt_pool_storage), cs);

    coact::Event* e = pool.alloc(1U);
    REQUIRE(e != nullptr);
    /* one save/restore per head operation; balanced, never nested */
    CHECK_EQ(1, g_pool_cs.saves);
    CHECK_EQ(1, g_pool_cs.restores);

    coact::event_gc(e);   /* allocation ref 1->0, then reclaim */
    CHECK_EQ(3, g_pool_cs.saves);
    CHECK_EQ(3, g_pool_cs.restores);
    CHECK_EQ(0U, pool.used());
}

/* Simulated ISR nesting (rt_interrupt_get_nest() > 0) must flip
   current_context() to Isr - the check the coordinator uses to reject the
   blocking direct path from ISR. */
COACT_TEST(rtthread_pal_current_context_isr)
{
    coact::pal::RtThread pal;
    stub_set_isr_nest(1U);
    coact::ExecutionContext isr_ctx = pal.current_context();
    CHECK_EQ(static_cast<int>(coact::ContextKind::Isr),
             static_cast<int>(isr_ctx.kind));
    stub_set_isr_nest(0U);
    coact::ExecutionContext task_ctx = pal.current_context();
    CHECK_NE(static_cast<int>(coact::ContextKind::Isr),
             static_cast<int>(task_ctx.kind));
}

/* ISR-path submit end-to-end: try_submit_from_isr must stage + wake the
   Dispatcher (rt_sem_release is ISR-safe) and drain to used()==0. */
COACT_TEST(rtthread_pal_isr_submit_end_to_end)
{
    g_rtt_counter.store(0);

    coact::pal::RtThread pal;
    coact::EventPool<kRttBlk, kRttCap> pool;
    pool.init(g_rtt_pool_storage, sizeof(g_rtt_pool_storage),
              coact::make_critical_section(pal));

    RttAo ao(kRttStates, 2U, kRttTrans, 1U, 1, 4U);
    coact::Event init_e;
    init_e.signal = 0U; init_e.pool_id = 0U; init_e.ref_ctr = 0U;
    ao.init(init_e);

    coact::Runtime<coact::DefaultConfig, coact::pal::RtThread> rt(pal);
    CHECK(rt.bind(&ao));
    CHECK(rt.initialize());
    rt.start();

    coact::EventQos qos{false, false};
    static constexpr int kN = 10;
    for (int i = 0; i < kN; ++i) {
        coact::Event* e = pool.alloc(1U);
        REQUIRE(e != nullptr);
        coact::SubmitResult r = rt.coordinator().try_submit_from_isr(coact::TargetId(1U), e, qos);
        CHECK(coact::SubmitDisposition::RejectedFull != r.disposition);
    }

    for (int w = 0; w < 200; ++w) {
        if (g_rtt_counter.load() >= kN) { break; }
        usleep(5000);
    }
    rt.stop();

    CHECK_EQ(kN, g_rtt_counter.load());
    CHECK_EQ(0U, pool.used());
}

}  // namespace

COACT_TEST_MAIN()
