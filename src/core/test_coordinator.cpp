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

/* Config that carves one High cell out for critical traffic. SmallCfg
   deliberately declares no reservation constants, so this is the only Config
   in this file whose Staging instantiation exercises the reservation path
   (the void_t probe in StagingReserveConfig picks up both members). */
struct ReserveCfg {
    enum : uint8_t {
        kMaxAo = 2U,
        kMaxStateDepth = 6U,
        kMaxDirectDepth = 4U,
        kBatchSizeMax = 4U
    };
    enum : uint16_t {
        kHighCapacity = 4U,
        kHighCriticalReserve = 1U,
        kNormalCapacity = 8U,
        kNormalReservedCapacity = 0U,
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

/* High-priority AO that is never direct-eligible, so every submission reaches
   the High staging partition (the partition the critical reserve carves out). */
struct HighStageTraits {
    static coact::LogicalPrio   logical_prio()   { return 20U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::High; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
using HighStageAo = coact::Ao<Ctx, CtxHsm, HighStageTraits>;

using StageT = coact::Staging<SmallCfg, coact::BoundedMpscQueue>;
using ReserveStageT = coact::Staging<ReserveCfg, coact::BoundedMpscQueue>;

static StageT make_staging()
{
    return StageT(coact::CriticalSection{nullptr,
        [](void*) -> coact::CriticalSection::Token { return 0U; },
        [](void*, coact::CriticalSection::Token) {} });
}

static ReserveStageT make_reserve_staging()
{
    return ReserveStageT(coact::CriticalSection{nullptr,
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

/* =========================================================================
 * A Config may reserve a slice of the High partition for critical traffic.
 * Ordinary (qos.critical == false) High submissions are capped at
 * kHighCapacity - kHighCriticalReserve, so once ordinary traffic fills its
 * share a critical High event is still admitted into the reserved cell instead
 * of returning RejectedFull. This is what lets a receive wakeup or a close
 * request survive a flood of ordinary High events.
 * ========================================================================= */
COACT_TEST(coordinator_high_critical_reserve_admits_when_ordinary_saturated)
{
    coact::Event init_e{};
    init_e.signal = 0U; init_e.pool_id = 0U; init_e.ref_ctr = 0U;
    HighStageAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    ao.init(init_e);

    ReserveStageT staging = make_reserve_staging();
    coact::AoRegistry<ReserveCfg> registry;
    coact::Monitor<ReserveCfg> monitor;
    ReserveCfg cfg{};
    coact::Breaker<ReserveCfg> breaker(cfg);
    CountingPal pal;
    coact::DispatchCoordinator<ReserveStageT, CountingPal,
                               coact::Breaker<ReserveCfg>> coord(
        staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind(&ao, ao.logical_prio()));

    /* kHighCapacity == 4 minus kHighCriticalReserve == 1 leaves 3 ordinary
       cells; the 3 ordinary submissions below must all be accepted. */
    const coact::EventQos ordinary{false, false};
    for (int i = 0; i < 3; ++i) {
        coact::Event e{};
        e.signal = 1U; e.pool_id = 0U; e.ref_ctr = 0U;
        const coact::SubmitResult r =
            coord.submit_from_task(coact::TargetId(1U), &e, ordinary);
        REQUIRE_EQ(r.disposition, coact::SubmitDisposition::Queued);
    }

    /* The 4th ordinary claim is refused by the reserve, not by a physically
       full queue (one High cell is still free for critical traffic). */
    coact::Event blocked{};
    blocked.signal = 1U; blocked.pool_id = 0U; blocked.ref_ctr = 0U;
    const coact::SubmitResult blocked_r =
        coord.submit_from_task(coact::TargetId(1U), &blocked, ordinary);
    REQUIRE_EQ(blocked_r.disposition, coact::SubmitDisposition::RejectedFull);

    /* Critical High ignores both the reserve cap and the L2 overload guard
       (the overflow above tripped L2, which drops only non-critical events),
       so it claims the reserved cell and is genuinely queued. */
    coact::Event critical{};
    critical.signal = 1U; critical.pool_id = 0U; critical.ref_ctr = 0U;
    const coact::EventQos critical_qos{true, false};
    const coact::SubmitResult critical_r =
        coord.submit_from_task(coact::TargetId(1U), &critical, critical_qos);
    CHECK_EQ(critical_r.disposition, coact::SubmitDisposition::Queued);
}

/* =========================================================================
 * A Config that declares no Normal reservation must reject a ReservedNormal
 * submission outright instead of quietly downgrading it to ordinary traffic,
 * which would let a caller believe it holds a reservation the Config never
 * opened.
 * ========================================================================= */
COACT_TEST(coordinator_reserved_normal_rejected_without_reservation_config)
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
    REQUIRE(registry.bind(&ao, ao.logical_prio()));

    coact::Event e{};
    e.signal = 1U; e.pool_id = 0U; e.ref_ctr = 0U;
    const coact::EventQos qos{false, false};
    const coact::SubmitResult r = coord.submit_from_task(
        coact::TargetId(1U), &e, qos, coact::StagingAdmission::ReservedNormal);
    CHECK_EQ(r.disposition, coact::SubmitDisposition::RejectedState);
}

/* =========================================================================
 * A Config that actually opens a reserved Normal lane (ReserveCfg above leaves
 * kNormalReservedCapacity at zero, so it cannot exercise the lane). Filling the
 * ordinary Normal cap and then submitting ReservedNormal proves the coordinator
 * threads the admission through to staging_.enqueue: if it passed
 * StagingAdmission::Ordinary instead, the submission would consume an ordinary
 * claim and be RejectedFull, never reaching the reserved lane.
 * ========================================================================= */
struct NormalLaneReserveCfg {
    enum : uint8_t {
        kMaxAo = 2U,
        kMaxStateDepth = 6U,
        kMaxDirectDepth = 4U,
        kBatchSizeMax = 4U
    };
    enum : uint16_t {
        kHighCapacity = 4U,
        kHighCriticalReserve = 0U,
        kNormalCapacity = 4U,
        kNormalReservedCapacity = 1U,
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
using NormalLaneStageT =
    coact::Staging<NormalLaneReserveCfg, coact::BoundedMpscQueue>;

static NormalLaneStageT make_normal_lane_staging()
{
    return NormalLaneStageT(coact::CriticalSection{nullptr,
        [](void*) -> coact::CriticalSection::Token { return 0U; },
        [](void*, coact::CriticalSection::Token) {}});
}

COACT_TEST(coordinator_reserved_normal_uses_reserved_lane)
{
    coact::Event init_e{};
    init_e.signal = 0U; init_e.pool_id = 0U; init_e.ref_ctr = 0U;
    CtxAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    ao.init(init_e);

    NormalLaneStageT staging = make_normal_lane_staging();
    coact::AoRegistry<NormalLaneReserveCfg> registry;
    coact::Monitor<NormalLaneReserveCfg> monitor;
    NormalLaneReserveCfg cfg{};
    coact::Breaker<NormalLaneReserveCfg> breaker(cfg);
    CountingPal pal;
    coact::DispatchCoordinator<NormalLaneStageT, CountingPal,
                               coact::Breaker<NormalLaneReserveCfg>> coord(
        staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind(&ao, ao.logical_prio()));

    // Fill the ordinary Normal lane exactly to its cap (capacity - reserved).
    const coact::EventQos ordinary{false, false};
    const unsigned kOrdinaryLimit = NormalLaneReserveCfg::kNormalCapacity
                                  - NormalLaneReserveCfg::kNormalReservedCapacity;
    for (unsigned i = 0U; i < kOrdinaryLimit; ++i) {
        coact::Event e{};
        e.signal = 1U; e.pool_id = 0U; e.ref_ctr = 0U;
        const coact::SubmitResult r =
            coord.submit_from_task(coact::TargetId(1U), &e, ordinary);
        REQUIRE_EQ(r.disposition, coact::SubmitDisposition::Queued);
    }

    // Ordinary lane is full; the reserved lane must still admit this. Under a
    // coordinator that dropped the admission argument it would be RejectedFull.
    coact::Event rn{};
    rn.signal = 1U; rn.pool_id = 0U; rn.ref_ctr = 0U;
    const coact::SubmitResult rr = coord.submit_from_task(
        coact::TargetId(1U), &rn, ordinary,
        coact::StagingAdmission::ReservedNormal);
    CHECK_EQ(rr.disposition, coact::SubmitDisposition::Queued);
}

}  // namespace

COACT_TEST_MAIN()
