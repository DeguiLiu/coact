// coact coordinator wakeup optimization test (flame finding): the Staging wake
// latch starts set while the Dispatcher drains, then the Dispatcher clears it
// before its final Ready re-check. Producers publish before setting the latch;
// only the first false-to-true exchange signals. A mock PAL counts signals.
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <cstdint>

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/coordinator.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/monitor.hpp"
#include "coact/pool.hpp"
#include "coact/staging.hpp"

namespace {

/* Small Config so partitions are cheap to reason about. */
struct SmallCfg {
    enum : uint8_t {
        kMaxAo = 2U,
        kMaxStateDepth = 6U,
        kMaxDirectDepth = 4U,
        kBatchSizeMax = 4U
    };
    enum : uint16_t {
        kHighCapacity = 4U,
        kNormalCapacity = 8U,
        kLowCapacity = 16U,
        kCooldownCycles = 3U
    };
    enum : uint32_t {
        kBatchTimeoutMs = 1U,
        kLowMaxWaitMs = 10U
    };
    enum : uint64_t {
        kDirectBudgetNs = 50000ULL,
        kRtcBudgetNs = 1000000ULL
    };
};

/* Mock PAL that counts Dispatcher wakeup signals instead of waking a thread. */
struct CountingPal {
    int signals = 0;
    uint64_t monotonic_ns() const noexcept { return 0ULL; }
    void signal_dispatcher_from_task() noexcept { ++signals; }
    void signal_dispatcher_from_isr() noexcept { ++signals; }
    void enter_direct() noexcept {}
    void leave_direct() noexcept {}
};

/* Minimal staged-only AO (never direct, so the coordinator always enqueues). */
struct Ctx { int dummy; };
static void c_noop_entry(Ctx&) {}
static void c_noop_exit(Ctx&)  {}
static bool c_ok(const Ctx&, const coact::Event&) { return true; }
static void c_noop_action(Ctx&, const coact::Event&) {}
static const coact::StateDef<Ctx> kStates[] = {
    { -1, nullptr, nullptr },
    {  0, c_noop_entry, c_noop_exit },
};
static const coact::TransitionDef<Ctx> kTrans[] = {
    { 1, 1U, 1, coact::TransitionKind::Internal, c_ok, c_noop_action },
};
struct Ctraits {
    static coact::LogicalPrio   logical_prio()   { return 10U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
using CtxHsm = coact::Hsm<Ctx>;
using CtxAo  = coact::Ao<Ctx, CtxHsm, Ctraits>;

using StageT = coact::Staging<SmallCfg, coact::BoundedMpscQueue>;

static StageT make_staging()
{
    return StageT(coact::CriticalSection{nullptr,
        [](void*) -> coact::CriticalSection::Token { return 0U; },
        [](void*, coact::CriticalSection::Token) {} });
}

/* =========================================================================
 * The wake latch starts set while the Dispatcher is active. Once the
 * Dispatcher arms its wait, the first submit signals and later submits
 * coalesce behind the same pending wake.
 * ========================================================================= */
COACT_TEST(coordinator_coalesces_wake_after_dispatcher_arms_wait)
{
    coact::Event init_e{};
    init_e.signal = 0U; init_e.pool_id = 0U; init_e.ref_ctr = 0U;
    CtxAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    ao.init(init_e);

    StageT staging = make_staging();
    coact::AoRegistry<SmallCfg> registry;
    coact::Monitor<SmallCfg> monitor;
    SmallCfg cfg{};
    coact::Breaker<SmallCfg> breaker(cfg);
    CountingPal pal;
    coact::DispatchCoordinator<StageT, CountingPal,
                               coact::Breaker<SmallCfg>> coord(
        staging, registry, monitor, breaker, pal);
    CHECK(registry.bind(&ao, ao.logical_prio()));

    coact::Event e{};
    e.signal = 1U; e.pool_id = 0U; e.ref_ctr = 0U;
    const coact::EventQos qos{false, false};

    /* Dispatcher mid-batch: the set latch suppresses redundant wakeups. */
    coact::SubmitResult r1 = coord.submit_from_task(coact::TargetId(1U), &e, qos);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(r1.disposition));
    CHECK_EQ(0, pal.signals);

    /* Arming the wait clears the latch; exactly the first producer signals. */
    staging.arm_dispatcher_wait();
    coact::SubmitResult r2 = coord.submit_from_task(coact::TargetId(1U), &e, qos);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(r2.disposition));
    CHECK_EQ(1, pal.signals);

    coact::SubmitResult r3 = coord.submit_from_task(coact::TargetId(1U), &e, qos);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(r3.disposition));
    CHECK_EQ(1, pal.signals);
}

/* Direct-eligible AO with a tiny RTC budget + a PAL whose monotonic clock
   advances past the budget on every sample. The coordinator's direct RTC
   sampling must feed the Breaker: 3 consecutive over-budget direct dispatches
   trip L1 (P2-12). */
struct DirectTraits {
    static coact::LogicalPrio logical_prio() { return 11U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return true; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000ULL;
};
using DirectHsm = coact::Hsm<Ctx>;
using DirectAo  = coact::Ao<Ctx, DirectHsm, DirectTraits>;

struct OtherDirectTraits {
    static coact::LogicalPrio logical_prio() { return 12U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return true; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000ULL;
};
using OtherDirectAo = coact::Ao<Ctx, DirectHsm, OtherDirectTraits>;

struct OverBudgetPal {
    int signals = 0;
    uint64_t now = 0;
    uint64_t monotonic_ns() noexcept
    {
        const uint64_t v = now;
        now += 2000ULL;  // each sample advances past the 1000ns budget
        return v;
    }
    void signal_dispatcher_from_task() noexcept { ++signals; }
    void signal_dispatcher_from_isr() noexcept { ++signals; }
    void enter_direct() noexcept {}
    void leave_direct() noexcept {}
};

COACT_TEST(coordinator_direct_over_budget_trips_breaker)
{
    coact::Event init_e{};
    init_e.signal = 0U; init_e.pool_id = 0U; init_e.ref_ctr = 0U;
    DirectAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    ao.init(init_e);

    StageT staging = make_staging();
    coact::AoRegistry<SmallCfg> registry;
    coact::Monitor<SmallCfg> monitor;
    SmallCfg cfg{};
    coact::Breaker<SmallCfg> breaker(cfg);
    OverBudgetPal pal;
    coact::DispatchCoordinator<StageT, OverBudgetPal,
                               coact::Breaker<SmallCfg>> coord(
        staging, registry, monitor, breaker, pal);
    CHECK(registry.bind(&ao, ao.logical_prio()));

    for (int i = 0; i < 3; ++i) {
        coact::Event e{};
        e.signal = 1U; e.pool_id = 0U; e.ref_ctr = 0U;
        const coact::EventQos qos{false, false};
        const coact::SubmitResult r =
            coord.submit_from_task(coact::TargetId(1U), &e, qos);
        CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Direct),
                 static_cast<int>(r.disposition));
    }
    CHECK_EQ(static_cast<int>(coact::BreakerLevel::BrokenL1),
             static_cast<int>(breaker.level()));
}

COACT_TEST(coordinator_target_breaker_does_not_block_other_ao)
{
    coact::Event init_e{};
    DirectAo slow(kStates, 2U, kTrans, 1U, 1, 4U);
    OtherDirectAo other(kStates, 2U, kTrans, 1U, 1, 4U);
    slow.init(init_e);
    other.init(init_e);

    StageT staging = make_staging();
    coact::AoRegistry<SmallCfg> registry;
    coact::Monitor<SmallCfg> monitor;
    coact::BreakerBank<SmallCfg> breakers(SmallCfg{});
    OverBudgetPal pal;
    coact::DispatchCoordinator<StageT, OverBudgetPal,
                               coact::BreakerBank<SmallCfg>> coord(
        staging, registry, monitor, breakers, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), slow, slow.logical_prio()));
    REQUIRE(registry.bind_at(coact::TargetId(2U), other, other.logical_prio()));

    const coact::EventQos qos{false, false};
    for (int i = 0; i < 3; ++i) {
        coact::Event event{};
        event.signal = 1U;
        const coact::SubmitResult result =
            coord.submit_from_task(coact::TargetId(1U), &event, qos);
        REQUIRE_EQ(result.disposition, coact::SubmitDisposition::Direct);
    }
    REQUIRE_EQ(breakers.level(coact::TargetId(1U)),
               coact::BreakerLevel::BrokenL1);

    coact::Event event{};
    event.signal = 1U;
    const coact::SubmitResult result =
        coord.submit_from_task(coact::TargetId(2U), &event, qos);
    CHECK_EQ(result.disposition, coact::SubmitDisposition::Direct);
    CHECK_EQ(breakers.level(coact::TargetId(2U)),
             coact::BreakerLevel::Normal);
}

}  // namespace

COACT_TEST_MAIN()
