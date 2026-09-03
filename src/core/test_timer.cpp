// coact TimerScheduler test: ManualTickSource-driven poll() mode with a
// minimal AO stub through the full coact pipeline (pool -> submit ->
// Dispatcher -> AO -> gc). Covers periodic, one-shot, cancel, idle, full
// capacity, error paths and monitoring counters.
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <atomic>
#include <cstring>
#include <unistd.h>

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/coordinator.hpp"
#include "coact/dispatcher.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/monitor.hpp"
#include "coact/pal_posix.hpp"
#include "coact/pool.hpp"
#include "coact/queue.hpp"
#include "coact/runtime.hpp"
#include "coact/staging.hpp"
#include "coact/timer.hpp"

namespace {

static std::atomic<int> g_timer_fired{0};

struct TimerCtx {};

static void t_noop_entry(TimerCtx&) {}
static void t_noop_exit(TimerCtx&) {}
static void t_action(TimerCtx&, const coact::Event&)
{
    g_timer_fired.fetch_add(1, std::memory_order_relaxed);
}
static bool t_always(const TimerCtx&, const coact::Event&) { return true; }

static const coact::StateDef<TimerCtx> kTStates[] = {
    /* 0: root */ { -1, nullptr, nullptr },
    /* 1: S0   */ {  0, t_noop_entry, t_noop_exit },
};
static const coact::TransitionDef<TimerCtx> kTTrans[] = {
    { 1, 1U, 1, coact::TransitionKind::Internal, t_always, t_action },
};

struct TimerTraits {
    static coact::LogicalPrio logical_prio() { return 20U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using TimerHsm = coact::Hsm<TimerCtx>;
using TimerAo = coact::Ao<TimerCtx, TimerHsm, TimerTraits>;

using PoolT = coact::EventPool<16U, 64U>;
using Rt = coact::Runtime<coact::DefaultConfig, coact::pal::Posix>;
using Sched =
    coact::TimerScheduler<PoolT, Rt::CoordinatorType, 4U, coact::ManualTickSource>;

alignas(16) static unsigned char g_storage[16U * 64U + 16U];

static void drain(int expected, Rt& rt)
{
    for (int w = 0; w < 2000; ++w) {
        if (g_timer_fired.load() >= expected) {
            break;
        }
        usleep(500);
    }
    rt.stop();
}

/* Test rig: POOL EXHAUSTED case needs the dispatcher stopped, so the rig
   comes in two flavors. Shared setup part for the normal (draining) tests. */
struct Rig {
    TimerAo ao;
    PoolT pool;
    coact::pal::Posix pal;
    Rt rt;
    Sched sched;
    coact::ManualTickSource& clock;

    Rig()
        : ao(kTStates, 2U, kTTrans, 1U, 1, 4U),
          pool(),
          pal(),
          rt(pal),
          sched(pool, rt.coordinator()),
          clock(sched.tick_source())
    {
        g_timer_fired.store(0);
        clock.reset();
        pool.init(g_storage, sizeof(g_storage));
        coact::Event init_e;
        init_e.signal = 0U;
        init_e.pool_id = 0U;
        init_e.ref_ctr = 0U;
        ao.init(init_e);
        (void)rt.bind(&ao);
        (void)rt.initialize();
        rt.start();
    }
};

/* Periodic: register a 10 ms periodic task, advance 4 periods -> exactly
   4 events (catch-up collapses the burst inside one advance). */
COACT_TEST(timer_periodic_manual_tick)
{
    Rig rig;
    coact::EventQos qos{false, false};
    auto id = rig.sched.schedule_periodic(coact::TargetId(1U), 1U, 10U, qos);
    REQUIRE(static_cast<bool>(id));
    CHECK_EQ(1U, rig.sched.task_count());

    for (int i = 0; i < 4; ++i) {
        rig.clock.advance(10U);   // exactly one period per step
        rig.sched.poll();
    }
    drain(4, rig.rt);

    CHECK_EQ(4, g_timer_fired.load());
    CHECK_EQ(4U, rig.sched.fired_count());
    CHECK_EQ(0U, rig.sched.skipped_count());
    CHECK_EQ(0U, rig.pool.used());   /* all events gc'd back */
}

/* Periodic long stall: 10 ms period, advance 45 ms in ONE step -> still
   exactly one event (missed periods are collapsed, no burst). */
COACT_TEST(timer_periodic_catchup_no_burst)
{
    Rig rig;
    coact::EventQos qos{false, false};
    auto id = rig.sched.schedule_periodic(coact::TargetId(1U), 1U, 10U, qos);
    REQUIRE(static_cast<bool>(id));

    rig.clock.advance(45U);
    rig.sched.poll();
    drain(1, rig.rt);

    CHECK_EQ(1, g_timer_fired.load());
    CHECK_EQ(1U, rig.sched.fired_count());
}

/* One-shot: fires exactly once after the delay, then auto-frees its slot
   and a later cancel() reports kNotFound. */
COACT_TEST(timer_once_single_fire)
{
    Rig rig;
    coact::EventQos qos{false, false};
    auto id = rig.sched.schedule_once(coact::TargetId(1U), 1U, 25U, qos);
    REQUIRE(static_cast<bool>(id));
    CHECK_EQ(1U, rig.sched.task_count());

    rig.clock.advance(24U);
    rig.sched.poll();
    rig.clock.advance(1U);    /* t = 25 ms: due */
    rig.sched.poll();
    rig.clock.advance(500U);  /* far past: must NOT fire again */
    rig.sched.poll();
    drain(1, rig.rt);

    CHECK_EQ(1, g_timer_fired.load());
    CHECK_EQ(0U, rig.sched.task_count());   /* slot auto-freed */
    auto cancel_result = rig.sched.cancel(id.value());    CHECK_EQ(false, static_cast<bool>(cancel_result));
    CHECK(static_cast<coact::TimerError>(coact::TimerError::kNotFound) ==
          cancel_result.error());}

/* Cancel: a cancelled periodic task delivers zero events; cancel of an
   unknown id reports kNotFound. */
COACT_TEST(timer_cancel_zero_events)
{
    Rig rig;
    coact::EventQos qos{false, false};
    auto id = rig.sched.schedule_periodic(coact::TargetId(1U), 1U, 10U, qos);
    REQUIRE(static_cast<bool>(id));

    auto cancel_result = rig.sched.cancel(id.value());    REQUIRE(static_cast<bool>(cancel_result));
    CHECK_EQ(0U, rig.sched.task_count());

    rig.clock.advance(100U);
    rig.sched.poll();
    drain(0, rig.rt);

    CHECK_EQ(0, g_timer_fired.load());
    CHECK_EQ(0U, rig.sched.fired_count());

    auto bad = rig.sched.cancel(coact::TimerTaskId(999U));
    CHECK_EQ(false, static_cast<bool>(bad));
    CHECK(static_cast<coact::TimerError>(coact::TimerError::kNotFound) ==
          bad.error());
}

/* Error paths: period 0 rejected; full capacity rejected with kSlotsFull;
   freed slots are reusable. */
COACT_TEST(timer_error_paths_and_slot_reuse)
{
    Rig rig;
    coact::EventQos qos{false, false};

    auto bad = rig.sched.schedule_periodic(coact::TargetId(1U), 1U, 0U, qos);
    CHECK_EQ(false, static_cast<bool>(bad));
    CHECK(static_cast<coact::TimerError>(coact::TimerError::kInvalidPeriod) ==
          bad.error());

    auto last_id = coact::kInvalidTimerTaskId;
    for (int i = 0; i < 4; ++i) {   /* capacity is 4 slots */
        auto id = rig.sched.schedule_once(coact::TargetId(1U), 1U, 100U, qos);
        REQUIRE(static_cast<bool>(id));
        last_id = id.value();
    }
    auto full = rig.sched.schedule_once(coact::TargetId(1U), 1U, 100U, qos);
    CHECK_EQ(false, static_cast<bool>(full));
    CHECK(static_cast<coact::TimerError>(coact::TimerError::kSlotsFull) ==
          full.error());

    auto result = rig.sched.cancel(last_id);
    REQUIRE(static_cast<bool>(result));
    auto reuse = rig.sched.schedule_once(coact::TargetId(1U), 1U, 100U, qos);
    CHECK(static_cast<bool>(reuse));
    CHECK_EQ(4U, rig.sched.task_count());
}

/* Idle gap: nothing scheduled -> ns_to_next_task() is UINT64_MAX; one
   pending 10 ms task -> roughly 10 ms remaining (never 0 before due). */
COACT_TEST(timer_ns_to_next_task)
{
    Rig rig;
    coact::EventQos qos{false, false};
    CHECK_EQ(UINT64_MAX, rig.sched.ns_to_next_task());

    auto id = rig.sched.schedule_periodic(coact::TargetId(1U), 1U, 10U, qos);
    REQUIRE(static_cast<bool>(id));
    const uint64_t remain = rig.sched.ns_to_next_task();
    CHECK((UINT64_MAX != remain));
    CHECK((remain <= 10000000ULL));

    rig.clock.advance(20U);
    CHECK_EQ(0U, rig.sched.ns_to_next_task());   /* overdue */

    (void)rig.sched.cancel(id.value());
    rig.rt.stop();
}

/* Extreme scenario (NOT constructed as a runtime test, per the port plan):
   the pool-exhausted skip path fires when every one of the pool's 64 blocks
   is allocated at expiration time; the trigger is skipped and counted in
   skipped_count(), the task stays scheduled, and the system keeps running
   (no block, no fault, no lost task). Constructing it would need a
   dispatcher-less rig that pre-fills the pool before poll(), which adds a
   test-only scheduler mode for one branch; the branch itself is four lines
   (alloc == nullptr -> skipped_++, continue) reviewed inline here. */

}  // namespace

COACT_TEST_MAIN()
