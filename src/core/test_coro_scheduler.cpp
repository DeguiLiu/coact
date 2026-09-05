// coact::coro TimerFacade tests (plan Task 6): ManualTickSource poll()
// drive, one-shot, periodic, cancel, slot-full and pool-exhausted skip.
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <unistd.h>

#include "coact/ao.hpp"
#include "coact/coro/scheduler.hpp"
#include "coact/hsm.hpp"
#include "coact/pal_posix.hpp"
#include "coact/pool.hpp"
#include "coact/runtime.hpp"
#include "coact/timer.hpp"

namespace {

static std::atomic<int32_t> g_fired{0};

struct Ctx {};
static void noop_entry(Ctx&) {}
static void noop_exit(Ctx&) {}
static bool always(const Ctx&, const coact::Event&) { return true; }
static void on_tick(Ctx&, const coact::Event&)
{
    g_fired.fetch_add(1, std::memory_order_relaxed);
}

static const coact::StateDef<Ctx> kStates[] = {
    { -1, nullptr, nullptr },
    {  0, noop_entry, noop_exit },
};
static const coact::TransitionDef<Ctx> kTrans[] = {
    { 1, 1U, 1, coact::TransitionKind::Internal, always, on_tick },
};

struct Traits {
    static coact::LogicalPrio logical_prio() { return 20U; }
    static coact::PriorityClass priority_class()
    {
        return coact::PriorityClass::Normal;
    }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using WorkerAo = coact::Ao<Ctx, coact::Hsm<Ctx>, Traits>;
using PoolT = coact::EventPool<16U, 64U>;
using Rt = coact::Runtime<coact::DefaultConfig, coact::pal::Posix>;
using Facade = coact::coro::TimerFacade<PoolT, Rt::CoordinatorType, 4U,
                                        coact::ManualTickSource>;

alignas(16) static unsigned char g_storage[16U * 64U + 16U];

static void drain(int32_t expected)
{
    for (int32_t w = 0; w < 2000; ++w) {
        if (g_fired.load(std::memory_order_relaxed) >= expected) {
            break;
        }
        usleep(500);
    }
}

struct Rig {
    WorkerAo ao;
    PoolT pool;
    coact::pal::Posix pal;
    Rt rt;
    Facade sched;
    coact::ManualTickSource& clock;

    Rig()
        : ao(kStates, 2U, kTrans, 1U, 1, 4U),
          pool(),
          pal(),
          rt(pal),
          sched(pool, rt.coordinator()),
          clock(sched.tick_source())
    {
        g_fired.store(0, std::memory_order_relaxed);
        clock.reset();
        pool.init(g_storage, sizeof(g_storage), coact::detail::noop_cs());
        coact::Event init_e{};
        init_e.signal = 0U;
        ao.init(init_e);
        (void)rt.bind(&ao);
        (void)rt.initialize();
        (void)rt.start();
    }

    ~Rig() { rt.stop(); }
};

}  // namespace

COACT_TEST(facade_one_shot_fires_once)
{
    Rig rig;
    coact::EventQos qos{false, false};
    auto id = rig.sched.schedule_once(coact::TargetId(1U), 1U, 25U, qos);
    REQUIRE(static_cast<bool>(id));

    rig.clock.advance(25U);
    rig.sched.poll();
    drain(1);
    CHECK_EQ(1, g_fired.load());
    CHECK_EQ(1U, rig.sched.fired_count());

    rig.clock.advance(25U);
    rig.sched.poll();
    drain(1);
    CHECK_EQ(1, g_fired.load());
    CHECK_EQ(0U, rig.sched.task_count());
}

COACT_TEST(facade_periodic_five_ticks)
{
    Rig rig;
    coact::EventQos qos{false, false};
    auto id = rig.sched.schedule_periodic(coact::TargetId(1U), 1U, 10U, qos);
    REQUIRE(static_cast<bool>(id));

    for (int32_t i = 0; i < 5; ++i) {
        rig.clock.advance(10U);
        rig.sched.poll();
        drain(i + 1);
    }
    CHECK_EQ(5, g_fired.load());
    CHECK_EQ(5U, rig.sched.fired_count());
    REQUIRE(static_cast<bool>(rig.sched.cancel(id.value())));
}

COACT_TEST(facade_cancel_stops_further_fires)
{
    Rig rig;
    coact::EventQos qos{false, false};
    auto id = rig.sched.schedule_periodic(coact::TargetId(1U), 1U, 10U, qos);
    REQUIRE(static_cast<bool>(id));

    rig.clock.advance(10U);
    rig.sched.poll();
    drain(1);
    CHECK_EQ(1, g_fired.load());

    REQUIRE(static_cast<bool>(rig.sched.cancel(id.value())));
    rig.clock.advance(30U);
    rig.sched.poll();
    drain(1);
    CHECK_EQ(1, g_fired.load());
    CHECK_EQ(0U, rig.sched.task_count());
}

COACT_TEST(facade_slots_full_then_reuse)
{
    Rig rig;
    coact::EventQos qos{false, false};
    coact::TimerTaskId last = coact::kInvalidTimerTaskId;
    for (int32_t i = 0; i < 4; ++i) {
        auto id = rig.sched.schedule_once(coact::TargetId(1U), 1U, 100U, qos);
        REQUIRE(static_cast<bool>(id));
        last = id.value();
    }
    auto full = rig.sched.schedule_once(coact::TargetId(1U), 1U, 100U, qos);
    CHECK(!static_cast<bool>(full));
    CHECK(coact::TimerError::kSlotsFull == full.error());
    REQUIRE(static_cast<bool>(rig.sched.cancel(last)));
    auto reuse = rig.sched.schedule_once(coact::TargetId(1U), 1U, 100U, qos);
    CHECK(static_cast<bool>(reuse));
}

COACT_TEST(facade_pool_exhaustion_increments_skipped)
{
    Rig rig;
    coact::EventQos qos{false, false};
    coact::Event* held[64]{};
    uint16_t n = 0U;
    for (; n < 64U; ++n) {
        held[n] = rig.pool.alloc(1U);
        if (nullptr == held[n]) {
            break;
        }
    }
    CHECK((n > 0U));

    auto id = rig.sched.schedule_once(coact::TargetId(1U), 1U, 10U, qos);
    REQUIRE(static_cast<bool>(id));
    rig.clock.advance(10U);
    rig.sched.poll();
    CHECK((rig.sched.skipped_count() >= 1U));
    CHECK_EQ(0, g_fired.load());

    for (uint16_t i = 0U; i < n; ++i) {
        coact::event_gc(held[i]);
    }
}

COACT_TEST_MAIN()
