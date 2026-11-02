// coact P1 admission tests: per-target arrival interval, backlog cap and the
// fixed admission snapshot ring. Host-portable: a mock PAL exposes a
// controllable monotonic clock; no POSIX PAL, no fork, no pthread.
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <cstdint>
#include <type_traits>
#include <utility>

#include "coact/admission.hpp"
#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/coordinator.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/monitor.hpp"
#include "coact/policy.hpp"
#include "coact/pool.hpp"
#include "coact/staging.hpp"

/* ------------------------------------------------------------------ *
 * Compile-time admission rule sets (design 4.1/4.2, admission.hpp).
 * A rule set is a type now, not a runtime table: ADM_CASE declares the
 * rules, the admission policy and the coordinator type for one case on
 * two lines, so the bodies below stay readable. Both macros expand to
 * plain namespace-scope declarations and exist only in this TU.
 * ------------------------------------------------------------------ */
#define ADM_RULE(Target, MinNs, Cap)                                   \
    coact::TargetAdmission {                                           \
        coact::TargetId(Target), coact::MinArrivalNs { MinNs },        \
            coact::MaxBacklog { Cap }                                  \
    }

#define ADM_CASE(Name, StageT, PalT, BrkT, Cfg, ...)                   \
    struct Name##Rules {                                               \
        static constexpr auto kRules = coact::make_rules(__VA_ARGS__); \
    };                                                                 \
    using Name##Adm =                                                  \
        coact::StaticAdmission<Name##Rules, Cfg,                      \
                               coact::AdmissionSpinSync>;              \
    using Name##Coord =                                                \
        coact::DispatchCoordinator<StageT, PalT, BrkT, Name##Adm>

/* Test helper: unwrap the Expected snapshot so the assertions read the way
 * they did with the old bool/out-parameter form, while a case that cares can
 * still inspect the specific SnapshotError. */
template <typename MonitorT>
bool snap_get(const MonitorT& mon, uint32_t seq,
              typename MonitorT::AdmissionSnapshot& out)
{
    const auto got = mon.admission_snapshot(seq);
    if (!got) {
        return false;
    }
    out = got.value();
    return true;
}

namespace {

/* Two targets are enough for the table-hit / table-miss cases. */
struct SmallCfg {
    enum : uint8_t {
        kMaxAo = 2U,
        kMaxStateDepth = 6U,
        kMaxDirectDepth = 4U,
        kBatchSizeMax = 4U
    };
    enum : uint16_t {
        kHighCapacity = 2U,
        kNormalCapacity = 8U,
        kLowCapacity = 8U,
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
    /* Opt into the rejection ring (design 4.1). A Config that does not
     * declare it gets an empty ring and no storage. */
    static constexpr uint8_t kAdmissionSnapshotSlots = 8U;
};

/* One High cell: filling it and popping it drives the enqueue-failure path. */
struct TinyCfg {
    enum : uint8_t {
        kMaxAo = 1U,
        kMaxStateDepth = 6U,
        kMaxDirectDepth = 4U,
        kBatchSizeMax = 4U
    };
    enum : uint16_t {
        kHighCapacity = 1U,
        kNormalCapacity = 4U,
        kLowCapacity = 4U,
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
    static constexpr uint8_t kAdmissionSnapshotSlots = 8U;
};

struct ManualClockPal {
    uint64_t now = 0U;
    int signals = 0;
    uint64_t monotonic_ns() const noexcept { return now; }
    void signal_dispatcher_from_task() noexcept { ++signals; }
    void signal_dispatcher_from_isr() noexcept { ++signals; }
    void enter_direct() noexcept {}
    void leave_direct() noexcept {}
};

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
using CtxHsm = coact::Hsm<Ctx>;

struct NormalStagedTraits {
    static coact::LogicalPrio   logical_prio()   { return 10U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
using NormalStagedAo = coact::Ao<Ctx, CtxHsm, NormalStagedTraits>;

struct NormalDirectTraits {
    static coact::LogicalPrio   logical_prio()   { return 11U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return true; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
using NormalDirectAo = coact::Ao<Ctx, CtxHsm, NormalDirectTraits>;

struct HighStagedTraits {
    static coact::LogicalPrio   logical_prio()   { return 20U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::High; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
using HighStagedAo = coact::Ao<Ctx, CtxHsm, HighStagedTraits>;

using StageT = coact::Staging<SmallCfg, coact::BoundedMpscQueue>;
using TinyStageT = coact::Staging<TinyCfg, coact::BoundedMpscQueue>;

static StageT make_staging()
{
    return StageT(coact::CriticalSection{nullptr,
        [](void*) -> coact::CriticalSection::Token { return 0U; },
        [](void*, coact::CriticalSection::Token) {}});
}

static TinyStageT make_tiny_staging()
{
    return TinyStageT(coact::CriticalSection{nullptr,
        [](void*) -> coact::CriticalSection::Token { return 0U; },
        [](void*, coact::CriticalSection::Token) {}});
}

static coact::Event static_evt(uint16_t signal)
{
    coact::Event e{};
    e.signal = signal;
    e.pool_id = 0U;
    e.ref_ctr = 0U;
    return e;
}

/* Dynamic-event storage for the reference-count checks. */
static constexpr uint16_t kBlk = 8U;
static constexpr uint16_t kCap = 16U;
using TestPool = coact::EventPool<kBlk, kCap>;
alignas(8) static unsigned char g_pool_storage[kBlk * (kCap + 1U)];

/* =========================================================================
 * The gate itself: first arrival is free, exactly the minimum interval is
 * admitted, a shorter one is rejected without moving the window, and the
 * window moves only on commit. A reservation that is never committed leaves
 * the gate exactly as it was.
 * ========================================================================= */
COACT_TEST(admission_arrival_gate_units)
{
    const coact::CriticalSection cs = coact::detail::noop_cs();
    coact::ArrivalGate gate;
    CHECK(!gate.admitted_ever(cs));
    CHECK_EQ(gate.last_admit_ns(cs), 0U);
    CHECK_EQ(gate.holds(cs), 0U);

    /* Reserved, not committed: the hold closes the window, the committed
     * timestamp has not moved yet. */
    CHECK(gate.try_reserve(cs, 1000U, coact::MinArrivalNs{100U}));
    CHECK_EQ(gate.last_admit_ns(cs), 0U);
    CHECK_EQ(gate.holds(cs), 1U);

    /* A second producer inside the interval is refused by the in-flight hold,
     * which is what stops a burst from passing the check before any commit. */
    CHECK(!gate.try_reserve(cs, 1099U, coact::MinArrivalNs{100U}));
    CHECK_EQ(gate.holds(cs), 1U);

    /* That reservation fails downstream: the gate is back to "never". */
    gate.rollback(cs);
    CHECK(!gate.admitted_ever(cs));
    CHECK_EQ(gate.last_admit_ns(cs), 0U);
    CHECK_EQ(gate.holds(cs), 0U);

    /* Committed: the window moves, and exactly at the minimum interval. */
    CHECK(gate.try_reserve(cs, 1000U, coact::MinArrivalNs{100U}));
    gate.commit(cs, 1000U);
    CHECK(gate.admitted_ever(cs));
    CHECK_EQ(gate.last_admit_ns(cs), 1000U);
    CHECK_EQ(gate.holds(cs), 0U);
    CHECK(!gate.try_reserve(cs, 1099U, coact::MinArrivalNs{100U}));
    CHECK_EQ(gate.last_admit_ns(cs), 1000U);   // rejection leaves the window
    CHECK(gate.try_reserve(cs, 1100U, coact::MinArrivalNs{100U}));
    gate.commit(cs, 1100U);
    CHECK_EQ(gate.last_admit_ns(cs), 1100U);

    /* A clock that moved backwards must never be admitted (unsigned wrap). */
    CHECK(!gate.try_reserve(cs, 1000U, coact::MinArrivalNs{1U}));
    CHECK_EQ(gate.last_admit_ns(cs), 1100U);

    /* min == 0 reserves unconditionally and records the sample. */
    CHECK(gate.try_reserve(cs, 2000U, coact::MinArrivalNs{0U}));
    gate.commit(cs, 2000U);
    CHECK_EQ(gate.last_admit_ns(cs), 2000U);
}

/* =========================================================================
 * The rollback hole the counted-hold model closes: overlapping reservations
 * that all fail must leave no timestamp behind, in either rollback order.
 * Restoring the observed word instead resurrects the earlier failed
 * reservation and keeps the window closed until that timestamp plus the
 * minimum interval.
 * ========================================================================= */
COACT_TEST(admission_gate_rollback_order_leaves_no_stale_window)
{
    const coact::CriticalSection cs = coact::detail::noop_cs();
    const coact::MinArrivalNs min_ns{100U};

    /* Both fail, newest rolled back first. */
    coact::ArrivalGate gate;
    REQUIRE(gate.try_reserve(cs, 1000U, min_ns));
    REQUIRE(gate.try_reserve(cs, 1100U, min_ns));
    gate.rollback(cs);                        // the 1100 reservation
    CHECK_EQ(gate.holds(cs), 1U);
    gate.rollback(cs);                        // the 1000 reservation
    CHECK_EQ(gate.holds(cs), 0U);
    CHECK(!gate.admitted_ever(cs));
    CHECK_EQ(gate.last_admit_ns(cs), 0U);
    /* The whole window is reusable: t=1000 is a first arrival again. */
    CHECK(gate.try_reserve(cs, 1000U, min_ns));
    gate.rollback(cs);

    /* One commit between two rollbacks: the committed arrival survives, the
     * failed ones leave nothing. */
    coact::ArrivalGate mixed;
    REQUIRE(mixed.try_reserve(cs, 2000U, min_ns));
    REQUIRE(mixed.try_reserve(cs, 2100U, min_ns));
    mixed.commit(cs, 2100U);                  // the newest succeeds
    mixed.rollback(cs);                       // the oldest fails
    CHECK_EQ(mixed.last_admit_ns(cs), 2100U);
    CHECK_EQ(mixed.holds(cs), 0U);
    CHECK(!mixed.try_reserve(cs, 2150U, min_ns));
    CHECK(mixed.try_reserve(cs, 2200U, min_ns));

    /* Out-of-order commit: an earlier arrival committed after a later one
     * must not move the window back. */
    coact::ArrivalGate reorder;
    REQUIRE(reorder.try_reserve(cs, 3000U, min_ns));
    REQUIRE(reorder.try_reserve(cs, 3100U, min_ns));
    reorder.commit(cs, 3100U);
    reorder.commit(cs, 3000U);
    CHECK_EQ(reorder.last_admit_ns(cs), 3100U);
    CHECK_EQ(reorder.holds(cs), 0U);
}

/* =========================================================================
 * reserve() promises an inert reservation for a target without an interval
 * rule. If the caller reuses a handle, the previous hold must be released
 * rather than carried over to the interval-free target.
 * ========================================================================= */
struct AdmReserveStaleRules {
    static constexpr auto kRules = coact::make_rules(ADM_RULE(2U, 1000U, 0U));
};

COACT_TEST(admission_reserve_without_interval_clears_stale_handle)
{
    using Adm = coact::StaticAdmission<AdmReserveStaleRules, SmallCfg,
                                       coact::AdmissionSpinSync>;

    ManualClockPal pal;
    Adm admission(pal);
    coact::ArrivalReservation handle;

    REQUIRE(admission.reserve(coact::TargetId(2U), 1000U, handle));
    CHECK(handle.active());
    CHECK_EQ(admission.holds(coact::TargetId(2U)), static_cast<uint16_t>(1U));

    /* Target 1 has no rule: the call returns an inert handle and the hold it
       replaced is released. */
    CHECK(admission.reserve(coact::TargetId(1U), 1000U, handle));
    CHECK(!handle.active());
    CHECK_EQ(admission.holds(coact::TargetId(2U)), static_cast<uint16_t>(0U));
    CHECK_EQ(admission.last_admit_ns(coact::TargetId(2U)),
             static_cast<uint64_t>(0U));
}

/* A rejected reserve must not leave the caller's old handle active: the
   failure path clears it, which rolls the stale hold back exactly once. */
COACT_TEST(admission_failed_reserve_clears_stale_handle)
{
    using Adm = coact::StaticAdmission<AdmReserveStaleRules, SmallCfg,
                                       coact::AdmissionSpinSync>;

    ManualClockPal pal;
    Adm admission(pal);
    coact::ArrivalReservation handle;

    REQUIRE(admission.reserve(coact::TargetId(2U), 1000U, handle));
    CHECK(handle.active());
    CHECK(!admission.reserve(coact::TargetId(2U), 1500U, handle));
    CHECK(!handle.active());
    CHECK_EQ(admission.holds(coact::TargetId(2U)), static_cast<uint16_t>(0U));
}

/* =========================================================================
 * ArrivalReservation: the submit path has six early returns after the gate,
 * so the window is handed back by leaving scope and kept only by commit().
 * Move-only, so one reservation cannot end up rolling the window back twice.
 * ========================================================================= */
COACT_TEST(admission_reservation_rolls_back_on_scope_exit)
{
    static_assert(
        !std::is_copy_constructible<coact::ArrivalReservation>::value,
        "a reservation must not be copyable");
    static_assert(!std::is_copy_assignable<coact::ArrivalReservation>::value,
                  "a reservation must not be copy-assignable");

    const coact::CriticalSection cs = coact::detail::noop_cs();
    coact::ArrivalGate gate;
    const coact::MinArrivalNs min_ns{100U};

    /* Failed submission: leaving scope uncommitted drops the hold. */
    {
        REQUIRE(gate.try_reserve(cs, 1000U, min_ns));
        coact::ArrivalReservation resv(gate, cs, 1000U);
        CHECK(resv.active());
        CHECK_EQ(gate.holds(cs), 1U);
        CHECK_EQ(gate.last_admit_ns(cs), 0U);   // nothing committed yet
    }
    CHECK(!gate.admitted_ever(cs));
    CHECK_EQ(gate.last_admit_ns(cs), 0U);
    CHECK_EQ(gate.holds(cs), 0U);

    /* Committed: the window stays advanced after the scope ends. */
    {
        REQUIRE(gate.try_reserve(cs, 2000U, min_ns));
        coact::ArrivalReservation resv(gate, cs, 2000U);
        resv.commit();
        CHECK(!resv.active());
        CHECK_EQ(gate.last_admit_ns(cs), 2000U);
    }
    CHECK(gate.admitted_ever(cs));
    CHECK_EQ(gate.last_admit_ns(cs), 2000U);
    CHECK_EQ(gate.holds(cs), 0U);

    /* A stale rollback must never clobber a later, committed admission. */
    {
        REQUIRE(gate.try_reserve(cs, 3000U, min_ns));
        coact::ArrivalReservation stale(gate, cs, 3000U);
        REQUIRE(gate.try_reserve(cs, 3100U, min_ns));
        coact::ArrivalReservation fresh(gate, cs, 3100U);
        fresh.commit();
        stale.rollback();
        CHECK_EQ(gate.last_admit_ns(cs), 3100U);
        CHECK_EQ(gate.holds(cs), 0U);
    }

    /* Move transfers ownership: the source stops guarding the window. */
    {
        REQUIRE(gate.try_reserve(cs, 4000U, min_ns));
        coact::ArrivalReservation src(gate, cs, 4000U);
        coact::ArrivalReservation dst(std::move(src));
        CHECK(!src.active());
        CHECK(dst.active());
        dst.commit();
    }
    CHECK_EQ(gate.last_admit_ns(cs), 4000U);

    /* Move-assignment onto a live reservation rolls the old one back first. */
    {
        REQUIRE(gate.try_reserve(cs, 5000U, min_ns));
        coact::ArrivalReservation live(gate, cs, 5000U);
        coact::ArrivalReservation empty;
        empty = std::move(live);
        CHECK(!live.active());
        CHECK(empty.active());
        CHECK_EQ(gate.holds(cs), 1U);   // moved, not rolled back
        CHECK_EQ(gate.last_admit_ns(cs), 4000U);
        empty.commit();
    }
    CHECK_EQ(gate.last_admit_ns(cs), 5000U);
}
/* Compile-time rule set for admission_arrival_interval_admits_first_and_exact_minimum (table). */
ADM_CASE(AdmArrivalInterval, StageT,
         ManualClockPal, coact::Breaker<SmallCfg>,
         SmallCfg, ADM_RULE(1U, 100U, 0U));

/* =========================================================================
 * Coordinator: first submit is admitted with no window; a submit exactly at
 * last + min is admitted; last + min - 1 is dropped as DroppedRateLimit with
 * kArrivalTooSoon and leaves the window at the last admitted arrival.
 * ========================================================================= */
COACT_TEST(admission_arrival_interval_admits_first_and_exact_minimum)
{
    NormalStagedAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    ao.init(static_evt(0U));

    StageT staging = make_staging();
    coact::AoRegistry<SmallCfg> registry;
    coact::Monitor<SmallCfg> monitor;
    coact::Breaker<SmallCfg> breaker{SmallCfg{}};
    ManualClockPal pal;
    AdmArrivalIntervalCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind(&ao, ao.logical_prio()));

    const coact::EventQos qos{false, false};
    coact::Event e1 = static_evt(1U);
    pal.now = 1000U;
    const coact::SubmitResult r1 =
        coord.submit_from_task(coact::TargetId(1U), &e1, qos);
    CHECK_EQ(r1.disposition, coact::SubmitDisposition::Queued);

    coact::Event e2 = static_evt(1U);
    pal.now = 1099U;
    const coact::SubmitResult r2 =
        coord.submit_from_task(coact::TargetId(1U), &e2, qos);
    CHECK_EQ(r2.disposition, coact::SubmitDisposition::DroppedRateLimit);
    CHECK_EQ(r2.reason, static_cast<uint16_t>(coact::kArrivalTooSoon));
    CHECK_EQ(coact_test::relaxed(
                 monitor.ao(coact::TargetId(1U))
                     .rejections[static_cast<size_t>(
                         coact::RejectReason::kC8ArrivalInterval)]),
             1U);
    CHECK_EQ(coact_test::relaxed(monitor.global().disposition_rate_limit), 1U);

    /* Exactly last + min: admitted, which also proves the 1099 rejection did
       not advance the window. */
    coact::Event e3 = static_evt(1U);
    pal.now = 1100U;
    const coact::SubmitResult r3 =
        coord.submit_from_task(coact::TargetId(1U), &e3, qos);
    CHECK_EQ(r3.disposition, coact::SubmitDisposition::Queued);

    /* Snapshot carries the rejected sample and the target's RTC budget. */
    CHECK_EQ(monitor.admission_snapshot_count(), 1U);
    coact::Monitor<SmallCfg>::AdmissionSnapshot snap{};
    CHECK(snap_get(monitor, 1U, snap));
    CHECK_EQ(snap.target, 1U);
    CHECK_EQ(snap.disposition,
             static_cast<uint8_t>(coact::SubmitDisposition::DroppedRateLimit));
    CHECK_EQ(snap.reason, static_cast<uint16_t>(coact::kArrivalTooSoon));
    CHECK_EQ(snap.seq, 1U);
    CHECK_EQ(snap.now_ns, 1099U);
    CHECK_EQ(snap.budget_ns, ao.rtc_budget_ns());
}
/* Compile-time rule set for admission_rejection_consumes_reference_once (table). */
ADM_CASE(AdmRejectionConsumes, StageT,
         ManualClockPal, coact::Breaker<SmallCfg>,
         SmallCfg, ADM_RULE(1U, 100U, 0U));

/* =========================================================================
 * A dropped arrival consumes the caller's reference exactly once.
 * ========================================================================= */
COACT_TEST(admission_rejection_consumes_reference_once)
{
    NormalStagedAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    ao.init(static_evt(0U));

    StageT staging = make_staging();
    coact::AoRegistry<SmallCfg> registry;
    coact::Monitor<SmallCfg> monitor;
    coact::Breaker<SmallCfg> breaker{SmallCfg{}};
    ManualClockPal pal;
    AdmRejectionConsumesCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind(&ao, ao.logical_prio()));

    TestPool pool;
    REQUIRE(pool.init(g_pool_storage, sizeof(g_pool_storage)));
    const coact::EventQos qos{false, false};

    pal.now = 1000U;
    coact::Event* first = pool.alloc(1U);
    REQUIRE(first != nullptr);
    CHECK_EQ(coord.submit_from_task(coact::TargetId(1U), first, qos).disposition,
             coact::SubmitDisposition::Queued);
    CHECK_EQ(pool.used(), 1U);   // the staged reference is still in flight

    for (int i = 0; i < 2; ++i) {
        pal.now = 1050U;
        coact::Event* dropped = pool.alloc(2U);
        REQUIRE(dropped != nullptr);
        CHECK_EQ(dropped->ref_ctr, 1U);
        const coact::SubmitResult r =
            coord.submit_from_task(coact::TargetId(1U), dropped, qos);
        CHECK_EQ(r.disposition, coact::SubmitDisposition::DroppedRateLimit);
        CHECK_EQ(pool.used(), 1U);   // only the queued block remains
    }
}
/* Compile-time rule set for admission_direct_path_commits_window (table). */
ADM_CASE(AdmDirectPath, StageT,
         ManualClockPal, coact::Breaker<SmallCfg>,
         SmallCfg, ADM_RULE(1U, 100U, 0U));

/* =========================================================================
 * Direct dispatch commits the arrival window at the arrival sample, so the
 * next admission still needs a full minimum interval.
 * ========================================================================= */
COACT_TEST(admission_direct_path_commits_window)
{
    NormalDirectAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    ao.init(static_evt(0U));

    StageT staging = make_staging();
    coact::AoRegistry<SmallCfg> registry;
    coact::Monitor<SmallCfg> monitor;
    coact::Breaker<SmallCfg> breaker{SmallCfg{}};
    ManualClockPal pal;
    AdmDirectPathCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind(&ao, ao.logical_prio()));

    const coact::EventQos qos{false, false};
    coact::Event e1 = static_evt(1U);
    pal.now = 1000U;
    CHECK_EQ(coord.submit_from_task(coact::TargetId(1U), &e1, qos).disposition,
             coact::SubmitDisposition::Direct);

    coact::Event e2 = static_evt(1U);
    pal.now = 1099U;
    CHECK_EQ(coord.submit_from_task(coact::TargetId(1U), &e2, qos).disposition,
             coact::SubmitDisposition::DroppedRateLimit);

    coact::Event e3 = static_evt(1U);
    pal.now = 1100U;
    CHECK_EQ(coord.submit_from_task(coact::TargetId(1U), &e3, qos).disposition,
             coact::SubmitDisposition::Direct);
    CHECK_EQ(ao.pending().load(), 0U);
}
/* Compile-time rule set for admission_enqueue_failure_rolls_window_back (table). */
ADM_CASE(AdmEnqueueFailure, TinyStageT,
         ManualClockPal, coact::Breaker<TinyCfg>,
         TinyCfg, ADM_RULE(1U, 100U, 0U));

/* =========================================================================
 * A staged enqueue failure rolls the reservation back: the window stays at
 * the last successful arrival, so a retry exactly min later is admitted.
 * ========================================================================= */
COACT_TEST(admission_enqueue_failure_rolls_window_back)
{
    HighStagedAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    ao.init(static_evt(0U));

    TinyStageT staging = make_tiny_staging();
    coact::AoRegistry<TinyCfg> registry;
    coact::Monitor<TinyCfg> monitor;
    coact::Breaker<TinyCfg> breaker{TinyCfg{}};
    ManualClockPal pal;
    AdmEnqueueFailureCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind(&ao, ao.logical_prio()));

    const coact::EventQos qos{false, false};
    coact::Event e1 = static_evt(1U);
    pal.now = 400U;
    CHECK_EQ(coord.submit_from_task(coact::TargetId(1U), &e1, qos).disposition,
             coact::SubmitDisposition::Queued);   // High cell 1/1 taken

    /* The interval window is free here, but the only High cell is occupied. */
    coact::Event e2 = static_evt(1U);
    pal.now = 500U;
    const coact::SubmitResult full =
        coord.submit_from_task(coact::TargetId(1U), &e2, qos);
    CHECK_EQ(full.disposition, coact::SubmitDisposition::RejectedFull);
    CHECK_EQ(full.reason, 0U);
    CHECK_EQ(coact_test::relaxed(monitor.global().overflow), 1U);

    /* Free the cell (the dispatcher would also decrement pending; the cap is
       disabled here, so the extra pending count is irrelevant). */
    coact::StagingSlot slot{};
    staging.begin_batch();
    REQUIRE(staging.dequeue_one(slot, pal.now));

    /* Overflow tripped L2, so use a critical retry to pass the overload guard.
    500 == last_admit(400) + min(100); had the failed 500 reserved, this equal
    timestamp would be rejected. */
    coact::Event e3 = static_evt(1U);
    const coact::EventQos critical{true, false};
    pal.now = 500U;
    CHECK_EQ(coord.submit_from_task(coact::TargetId(1U), &e3, critical).disposition,
             coact::SubmitDisposition::Queued);
}
/* Compile-time rule set for admission_backlog_cap_rejects_third_and_ignores_critical (table). */
ADM_CASE(AdmBacklogCap, StageT,
         ManualClockPal, coact::Breaker<SmallCfg>,
         SmallCfg, ADM_RULE(1U, 0U, 2U));

/* =========================================================================
 * Backlog cap: two admitted, the third rejected with RejectedFull /
 * kBacklogFull, pending stays at the cap, the breaker never transitions, and
 * qos.critical does not bypass the cap.
 * ========================================================================= */
COACT_TEST(admission_backlog_cap_rejects_third_and_ignores_critical)
{
    NormalStagedAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    ao.init(static_evt(0U));

    StageT staging = make_staging();
    coact::AoRegistry<SmallCfg> registry;
    coact::Monitor<SmallCfg> monitor;
    coact::Breaker<SmallCfg> breaker{SmallCfg{}};
    ManualClockPal pal;
    AdmBacklogCapCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind(&ao, ao.logical_prio()));

    const coact::EventQos qos{false, false};
    for (int i = 0; i < 2; ++i) {
        coact::Event e = static_evt(1U);
        pal.now = static_cast<uint64_t>(i);
        CHECK_EQ(coord.submit_from_task(coact::TargetId(1U), &e, qos).disposition,
                 coact::SubmitDisposition::Queued);
    }
    CHECK_EQ(ao.pending().load(), 2U);

    coact::Event third = static_evt(1U);
    pal.now = 10U;
    const coact::SubmitResult r =
        coord.submit_from_task(coact::TargetId(1U), &third, qos);
    CHECK_EQ(r.disposition, coact::SubmitDisposition::RejectedFull);
    CHECK_EQ(r.reason, static_cast<uint16_t>(coact::kBacklogFull));
    CHECK_EQ(ao.pending().load(), 2U);              // not incremented
    CHECK_EQ(breaker.level(), coact::BreakerLevel::Normal);
    CHECK_EQ(coact_test::relaxed(monitor.global().overflow), 0U);

    /* qos.critical must not bypass the per-target cap. */
    coact::Event critical_evt = static_evt(1U);
    const coact::EventQos critical{true, false};
    pal.now = 20U;
    const coact::SubmitResult rc =
        coord.submit_from_task(coact::TargetId(1U), &critical_evt, critical);
    CHECK_EQ(rc.disposition, coact::SubmitDisposition::RejectedFull);
    CHECK_EQ(rc.reason, static_cast<uint16_t>(coact::kBacklogFull));
    CHECK_EQ(ao.pending().load(), 2U);
    CHECK_EQ(breaker.level(), coact::BreakerLevel::Normal);

    CHECK_EQ(coact_test::relaxed(
                 monitor.ao(coact::TargetId(1U))
                     .rejections[static_cast<size_t>(
                         coact::RejectReason::kC9BacklogCap)]),
             2U);
    CHECK_EQ(monitor.admission_snapshot_count(), 2U);
    coact::Monitor<SmallCfg>::AdmissionSnapshot snap{};
    CHECK(snap_get(monitor, 2U, snap));
    CHECK_EQ(snap.reason, static_cast<uint16_t>(coact::kBacklogFull));
    CHECK_EQ(snap.disposition,
             static_cast<uint8_t>(coact::SubmitDisposition::RejectedFull));
    CHECK_EQ(snap.now_ns, 20U);   // a configured cap samples the clock
}

/* =========================================================================
 * "Admission disabled" is three different types now, and all three must
 * reproduce the historical submit sequence exactly: the default NoAdmission
 * policy, an enabled policy with an empty rule set, and an enabled policy
 * whose only row sets both limits to zero. Runs nine Normal submits against a
 * capacity of 8: the ninth is the ordinary partition overflow.
 * ========================================================================= */
struct AdmNoRulesSet {
    static constexpr auto kRules = coact::make_rules();
};
struct AdmZeroRulesSet {
    static constexpr auto kRules = coact::make_rules(ADM_RULE(1U, 0U, 0U));
};
using AdmNoRules =
    coact::StaticAdmission<AdmNoRulesSet, SmallCfg, coact::AdmissionSpinSync>;
using AdmZeroRules =
    coact::StaticAdmission<AdmZeroRulesSet, SmallCfg, coact::AdmissionSpinSync>;

template <typename AdmT>
static void run_plain_sequence(coact::Monitor<SmallCfg>& monitor,
                               coact::SubmitDisposition* out, int count)
{
    NormalStagedAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    ao.init(static_evt(0U));
    StageT staging = make_staging();
    coact::AoRegistry<SmallCfg> registry;
    coact::Breaker<SmallCfg> breaker{SmallCfg{}};
    ManualClockPal pal;
    coact::DispatchCoordinator<StageT, ManualClockPal,
                               coact::Breaker<SmallCfg>, AdmT>
        coord(staging, registry, monitor, breaker, pal);
    if (!registry.bind(&ao, ao.logical_prio())) {
        return;
    }
    const coact::EventQos qos{false, false};
    for (int i = 0; i < count; ++i) {
        coact::Event e = static_evt(1U);
        pal.now = static_cast<uint64_t>(i);
        out[i] = coord.submit_from_task(coact::TargetId(1U), &e, qos).disposition;
    }
}

COACT_TEST(admission_disabled_matches_historical_path)
{
    constexpr int kCount = 9;
    coact::SubmitDisposition defaulted[kCount]{};
    coact::SubmitDisposition empty_rules[kCount]{};
    coact::SubmitDisposition zero_limits[kCount]{};
    coact::Monitor<SmallCfg> mon_default;
    coact::Monitor<SmallCfg> mon_empty;
    coact::Monitor<SmallCfg> mon_zero;

    run_plain_sequence<coact::NoAdmission>(mon_default, defaulted, kCount);
    run_plain_sequence<AdmNoRules>(mon_empty, empty_rules, kCount);
    run_plain_sequence<AdmZeroRules>(mon_zero, zero_limits, kCount);

    for (int i = 0; i < kCount; ++i) {
        CHECK_EQ(defaulted[i], empty_rules[i]);
        CHECK_EQ(defaulted[i], zero_limits[i]);
        if (i < SmallCfg::kNormalCapacity) {
            CHECK_EQ(defaulted[i], coact::SubmitDisposition::Queued);
        }
        else {
            CHECK_EQ(defaulted[i], coact::SubmitDisposition::RejectedFull);
        }
    }

    /* Same outcomes, deliberately different diagnostics. The default policy is
       compiled out and must leave the ring untouched, so an integrator who did
       not ask for admission sees no new records. A policy that is enabled but
       limits nothing still owns the diagnostic contract: the one partition
       overflow is recorded as kStagingFull with the target's budget, which is
       the whole point of turning the feature on. */
    CHECK_EQ(mon_default.admission_snapshot_count(), 0U);
    CHECK_EQ(mon_empty.admission_snapshot_count(), 1U);
    CHECK_EQ(mon_zero.admission_snapshot_count(), 1U);
    const auto overflow_rec = mon_zero.admission_snapshot(1U);
    REQUIRE(static_cast<bool>(overflow_rec));
    CHECK_EQ(overflow_rec.value().target, 1U);
    CHECK_EQ(overflow_rec.value().reason,
             static_cast<uint16_t>(coact::kStagingFull));
    CHECK_EQ(overflow_rec.value().disposition,
             static_cast<uint8_t>(coact::SubmitDisposition::RejectedFull));
    CHECK_EQ(overflow_rec.value().budget_ns, 1000000U);
    /* now_ns stays 0 here: a Normal staged submission takes no clock sample,
       and admission must not add one to the hot path just to fill a diagnostic
       field (design 8.2). The interval gate, a backlog cap, Low aging and the
       M4 policy each sample for their own reason and then fill it in. */
    CHECK_EQ(overflow_rec.value().now_ns, 0U);
}
/* Compile-time rule set for admission_applies_to_isr_and_skips_absent_target (table). */
ADM_CASE(AdmAppliesTo, StageT,
         ManualClockPal, coact::Breaker<SmallCfg>,
         SmallCfg, ADM_RULE(1U, 200U, 0U));

/* =========================================================================
 * The gate lives in submit_internal, so ISR submissions obey the same rule;
 * a target without a row keeps both limits disabled.
 * ========================================================================= */
COACT_TEST(admission_applies_to_isr_and_skips_absent_target)
{
    NormalStagedAo isr_ao(kStates, 2U, kTrans, 1U, 1, 4U);
    HighStagedAo free_ao(kStates, 2U, kTrans, 1U, 1, 4U);
    isr_ao.init(static_evt(0U));
    free_ao.init(static_evt(0U));

    StageT staging = make_staging();
    coact::AoRegistry<SmallCfg> registry;
    coact::Monitor<SmallCfg> monitor;
    coact::Breaker<SmallCfg> breaker{SmallCfg{}};
    ManualClockPal pal;
    AdmAppliesToCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), isr_ao, isr_ao.logical_prio()));
    REQUIRE(registry.bind_at(coact::TargetId(2U), free_ao, free_ao.logical_prio()));

    const coact::EventQos qos{false, false};
    coact::Event i1 = static_evt(1U);
    pal.now = 1000U;
    CHECK_EQ(coord.try_submit_from_isr(coact::TargetId(1U), &i1, qos).disposition,
             coact::SubmitDisposition::Queued);

    coact::Event i2 = static_evt(1U);
    pal.now = 1199U;
    const coact::SubmitResult early =
        coord.try_submit_from_isr(coact::TargetId(1U), &i2, qos);
    CHECK_EQ(early.disposition, coact::SubmitDisposition::DroppedRateLimit);
    CHECK_EQ(early.reason, static_cast<uint16_t>(coact::kArrivalTooSoon));

    coact::Event i3 = static_evt(1U);
    pal.now = 1200U;
    CHECK_EQ(coord.try_submit_from_isr(coact::TargetId(1U), &i3, qos).disposition,
             coact::SubmitDisposition::Queued);

    /* Target 2 has no row: no interval, so back-to-back arrivals queue. */
    coact::Event f1 = static_evt(1U);
    pal.now = 1201U;
    CHECK_EQ(coord.try_submit_from_isr(coact::TargetId(2U), &f1, qos).disposition,
             coact::SubmitDisposition::Queued);
    coact::Event f2 = static_evt(1U);
    pal.now = 1202U;
    CHECK_EQ(coord.submit_from_task(coact::TargetId(2U), &f2, qos).disposition,
             coact::SubmitDisposition::Queued);
}

/* =========================================================================
 * Snapshot ring: monotonic count, readable record, out-of-range rejection and
 * the ninth record overwriting the first slot.
 * ========================================================================= */
COACT_TEST(admission_snapshot_ring_wraps_at_eight_slots)
{
    coact::Monitor<SmallCfg> monitor;
    coact::Monitor<SmallCfg>::AdmissionSnapshot snap{};
    CHECK_EQ(monitor.admission_snapshot_count(), 0U);
    CHECK(!snap_get(monitor, 0U, snap));
    CHECK(!snap_get(monitor, 1U, snap));

    for (uint32_t i = 1U; i <= 9U; ++i) {
        monitor.record_admission_snapshot(
            coact::TargetId(static_cast<uint8_t>(i)),
            coact::SubmitDisposition::DroppedRateLimit,
            static_cast<uint16_t>(100U + i),
            static_cast<uint64_t>(i) * 10U,
            static_cast<uint64_t>(i) * 100U);
    }
    CHECK_EQ(monitor.admission_snapshot_count(), 9U);
    CHECK(!snap_get(monitor, 10U, snap));

    CHECK(snap_get(monitor, 9U, snap));
    CHECK_EQ(snap.seq, 9U);
    CHECK_EQ(snap.target, 9U);
    CHECK_EQ(snap.reason, 109U);
    CHECK_EQ(snap.now_ns, 90U);
    CHECK_EQ(snap.budget_ns, 900U);

    /* Slot index is (seq - 1) % 8: the 9th record reused slot 1 and evicted
       seq 1, which must no longer resolve to the new occupant. */
    CHECK(!snap_get(monitor, 1U, snap));
    for (uint32_t seq = 2U; seq <= 9U; ++seq) {
        CHECK(snap_get(monitor, seq, snap));
        CHECK_EQ(snap.seq, seq);
    }
}

}  // namespace

COACT_TEST_MAIN()
