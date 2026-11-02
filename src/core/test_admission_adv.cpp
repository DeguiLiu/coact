// Adversarial admission tests (design 4.1/4.2/5.3/8.1).
//
// These tests are derived from the frozen API contract, not from the current
// implementation. Each one is written to fail on a plausible implementation
// shortcut (lenient window, non-atomic pending reservation, qos.critical
// bypass, shared state across targets, per-submission storage) instead of
// mirroring whatever the coordinator happens to do.
//
// Host-only; heap accounting is local to this TU.
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "coact/admission.hpp"
#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/coordinator.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/monitor.hpp"
#include "coact/pal_posix.hpp"
#include "coact/policy.hpp"
#include "coact/pool.hpp"
#include "coact/runtime.hpp"
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

/* ------------------------------------------------------------------ *
 * Heap accounting: requirement 7 says the submit path is fixed-storage
 * and must not allocate. Counting global new is the only way to prove
 * it from outside the library.
 * ------------------------------------------------------------------ */
std::atomic<size_t> g_alloc_count{0U};

void* operator new(std::size_t n)
{
    g_alloc_count.fetch_add(1U, std::memory_order_relaxed);
    void* p = std::malloc((0U == n) ? 1U : n);
    if (nullptr == p) {
        std::abort();
    }
    return p;
}

void* operator new[](std::size_t n)
{
    return ::operator new(n);
}

void operator delete(void* p) noexcept
{
    std::free(p);
}

void operator delete[](void* p) noexcept
{
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
    std::free(p);
}

namespace {

/* Small Config so every partition is easy to saturate on purpose. */
struct AdvCfg {
    enum : uint8_t {
        kMaxAo = 4U,
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
        kLowMaxWaitMs = 10U,
        /* Runtime::start() pushes this to the PAL; the suite drives a real
         * Dispatcher thread in the Runtime plumbing case. */
        kDispatcherStackBytes = 4096U
    };
    enum : uint64_t {
        kDirectBudgetNs = 50000ULL,
        kRtcBudgetNs = 1000000ULL
    };
    /* Opt into the rejection ring (design 4.1). A Config that does not
     * declare this gets no snapshot storage at all. */
    static constexpr uint8_t kAdmissionSnapshotSlots = 8U;
};

/* Large Normal partition for the concurrency tests: the admission gate, not
 * the ring capacity, must be the only reason a submission is refused. */
struct BigCfg {
    enum : uint8_t {
        kMaxAo = 2U,
        kMaxStateDepth = 6U,
        kMaxDirectDepth = 4U,
        kBatchSizeMax = 8U
    };
    enum : uint16_t {
        kHighCapacity = 8U,
        kNormalCapacity = 1024U,
        kLowCapacity = 64U,
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

/* Virtual clock. Each host thread owns its sample, so concurrent submits never
 * observe a torn or re-entrant time value. Never reads the real clock. */
struct VirtualClock {
    static thread_local uint64_t tls_now;
    static thread_local int tls_signals;

    uint64_t monotonic_ns() const noexcept { return tls_now; }
    void signal_dispatcher_from_task() noexcept { ++tls_signals; }
    void signal_dispatcher_from_isr() noexcept { ++tls_signals; }
    void enter_direct() noexcept {}
    void leave_direct() noexcept {}
};

thread_local uint64_t VirtualClock::tls_now = 0U;
thread_local int VirtualClock::tls_signals = 0;

static void clock_at(uint64_t now) noexcept
{
    VirtualClock::tls_now = now;
}

/* Minimal HSM: one internal transition, so a direct dispatch always succeeds
 * and never leaves the AO mid-transition. */
struct Ctx {
    int dummy;
};

static void ctx_entry(Ctx&) {}
static void ctx_exit(Ctx&) {}
static bool ctx_ok(const Ctx&, const coact::Event&) { return true; }
static void ctx_action(Ctx&, const coact::Event&) {}

static const coact::StateDef<Ctx> kStates[] = {
    { -1, nullptr, nullptr },
    {  0, ctx_entry, ctx_exit },
};
static const coact::TransitionDef<Ctx> kTrans[] = {
    { 1, 1U, 1, coact::TransitionKind::Internal, ctx_ok, ctx_action },
};

struct StageTraits {
    static coact::LogicalPrio logical_prio() { return 10U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

struct Stage2Traits {
    static coact::LogicalPrio logical_prio() { return 12U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

struct DirectTraits {
    static coact::LogicalPrio logical_prio() { return 11U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return true; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

/* High class: the critical-reserve code path must not be able to raise the
 * backlog cap, and qos.critical must not bypass it either. */
struct HighStageTraits {
    static coact::LogicalPrio logical_prio() { return 20U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::High; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using CtxHsm = coact::Hsm<Ctx>;
using StageAo = coact::Ao<Ctx, CtxHsm, StageTraits>;
using Stage2Ao = coact::Ao<Ctx, CtxHsm, Stage2Traits>;
using DirectAo = coact::Ao<Ctx, CtxHsm, DirectTraits>;
using HighStageAo = coact::Ao<Ctx, CtxHsm, HighStageTraits>;

using AdvStage = coact::Staging<AdvCfg, coact::BoundedMpscQueue>;
using BigStage = coact::Staging<BigCfg, coact::BoundedMpscQueue>;

static AdvStage make_stage()
{
    return AdvStage(coact::CriticalSection{nullptr,
        [](void*) -> coact::CriticalSection::Token { return 0U; },
        [](void*, coact::CriticalSection::Token) {}});
}

static BigStage make_big_stage()
{
    return BigStage(coact::CriticalSection{nullptr,
        [](void*) -> coact::CriticalSection::Token { return 0U; },
        [](void*, coact::CriticalSection::Token) {}});
}

static coact::Event init_event()
{
    coact::Event e{};
    e.signal = 0U;
    e.pool_id = 0U;
    e.ref_ctr = 0U;
    return e;
}

template <typename CoordT>
static coact::SubmitResult submit_task(CoordT& coord, coact::TargetId target,
                                       coact::Event* e, uint64_t now,
                                       bool critical = false)
{
    clock_at(now);
    return coord.submit_from_task(target, e, coact::EventQos{critical, false});
}

template <typename CoordT>
static coact::SubmitResult submit_isr(CoordT& coord, coact::TargetId target,
                                      coact::Event* e, uint64_t now,
                                      bool critical = false)
{
    clock_at(now);
    return coord.try_submit_from_isr(target, e, coact::EventQos{critical, false});
}

/* Pop one slot and account it as a completed dispatch. */
template <typename StageT>
static bool drain_completed(StageT& staging, coact::AoBase& ao, uint64_t now)
{
    staging.begin_batch();
    coact::StagingSlot slot{};
    if (!staging.dequeue_one(slot, now)) {
        return false;
    }
    ao.pending().decrement();
    coact::event_gc(slot.event);
    return true;
}

/* Pop one slot that was injected directly into staging (no pending account). */
template <typename StageT>
static bool drain_raw(StageT& staging, uint64_t now)
{
    staging.begin_batch();
    coact::StagingSlot slot{};
    if (!staging.dequeue_one(slot, now)) {
        return false;
    }
    coact::event_gc(slot.event);
    return true;
}

/* Fill the whole Normal partition through Staging directly. These slots are
 * occupying capacity without a coordinator pending account, which is exactly
 * what a Dispatcher-retained slot looks like to a producer. */
static unsigned fill_normal(AdvStage& staging, coact::Event* e, unsigned count)
{
    unsigned pushed = 0U;
    for (unsigned i = 0U; i < count; ++i) {
        if (!staging.enqueue(coact::TargetId(1U), e,
                             coact::PriorityClass::Normal, 0U)) {
            break;
        }
        ++pushed;
    }
    return pushed;
}

/* Pool-backed submissions prove the reference is consumed exactly once:
 * after a dropped/direct submit the pool must be fully reclaimed. */
static constexpr uint16_t kAdvBlk = 16U;
static constexpr uint16_t kAdvPoolCap = 32U;
using AdvPool = coact::EventPool<kAdvBlk, kAdvPoolCap>;
alignas(16) static unsigned char g_pool_storage[kAdvBlk * (kAdvPoolCap + 1U)];
/* Compile-time rule set for admission_adv_t1_interval_boundary (table). */
ADM_CASE(AdmT1Interval, AdvStage,
         VirtualClock, coact::Breaker<AdvCfg>,
         AdvCfg, ADM_RULE(1U, 100U, 0U));

/* =====================================================================
 * T1: interval boundary. First always admitted; gap == min admitted;
 * gap < min rejected with DroppedRateLimit + reason kArrivalTooSoon;
 * a rejection must not advance the window.
 * ===================================================================== */
COACT_TEST(admission_adv_t1_interval_boundary)
{

    DirectAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    ao.init(ie);

    AdvStage staging = make_stage();
    coact::AoRegistry<AdvCfg> registry;
    coact::Monitor<AdvCfg> monitor;
    AdvCfg cfg{};
    coact::Breaker<AdvCfg> breaker(cfg);
    VirtualClock pal;
    AdmT1IntervalCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), ao, ao.logical_prio()));

    AdvPool pool;
    REQUIRE(pool.init(g_pool_storage, sizeof(g_pool_storage)));

    const coact::TargetId t1(1U);
    struct Step {
        uint64_t now;
        coact::SubmitDisposition expected;
        uint16_t reason;
    };
    /* min == 100. Every rejection keeps the window at 0 (and then at 100/200),
     * so the following boundary submit is admitted. */
    const Step steps[] = {
        {   0U, coact::SubmitDisposition::Direct, 0U },
        {  99U, coact::SubmitDisposition::DroppedRateLimit,
                static_cast<uint16_t>(coact::kArrivalTooSoon) },
        { 100U, coact::SubmitDisposition::Direct, 0U },
        { 199U, coact::SubmitDisposition::DroppedRateLimit,
                static_cast<uint16_t>(coact::kArrivalTooSoon) },
        { 200U, coact::SubmitDisposition::Direct, 0U },
    };
    for (const Step& s : steps) {
        coact::Event* e = pool.alloc(1U);
        REQUIRE(e != nullptr);
        const coact::SubmitResult r = submit_task(coord, t1, e, s.now);
        CHECK_EQ(static_cast<int>(s.expected), static_cast<int>(r.disposition));
        CHECK_EQ(s.reason, r.reason);
        /* Direct consumes the reference; a rejection consumes exactly one. */
        CHECK_EQ(pool.used(), static_cast<uint16_t>(0U));
    }
    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(0U));
    CHECK_EQ(static_cast<int>(breaker.level()),
             static_cast<int>(coact::BreakerLevel::Normal));
}
/* Compile-time rule set for admission_adv_t1_zero_interval_disables (table). */
ADM_CASE(AdmT1Zero, AdvStage,
         VirtualClock, coact::Breaker<AdvCfg>,
         AdvCfg, ADM_RULE(1U, 0U, 0U));

/* min == 0 means "disabled", never "reject everything". */
COACT_TEST(admission_adv_t1_zero_interval_disables)
{

    DirectAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    ao.init(ie);

    AdvStage staging = make_stage();
    coact::AoRegistry<AdvCfg> registry;
    coact::Monitor<AdvCfg> monitor;
    AdvCfg cfg{};
    coact::Breaker<AdvCfg> breaker(cfg);
    VirtualClock pal;
    AdmT1ZeroCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), ao, ao.logical_prio()));

    coact::Event e{};
    e.signal = 1U;
    for (int i = 0; i < 3; ++i) {
        const coact::SubmitResult r =
            submit_task(coord, coact::TargetId(1U), &e, 0U);
        CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Direct),
                 static_cast<int>(r.disposition));
    }
}
/* Compile-time rule set for admission_adv_t2_rollback_after_enqueue_failure (table). */
ADM_CASE(AdmT2Rollback, AdvStage,
         VirtualClock, coact::Breaker<AdvCfg>,
         AdvCfg, ADM_RULE(1U, 100U, 0U));

/* =====================================================================
 * T2: rollback of the interval reservation when the commit fails.
 *
 * A failed enqueue after a successful interval reservation must restore
 * the window, otherwise the next legal submit (exactly min later) is
 * rejected for a submission that never happened.
 * ===================================================================== */
COACT_TEST(admission_adv_t2_rollback_after_enqueue_failure)
{

    StageAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    ao.init(ie);

    AdvStage staging = make_stage();
    coact::AoRegistry<AdvCfg> registry;
    coact::Monitor<AdvCfg> monitor;
    AdvCfg cfg{};
    coact::Breaker<AdvCfg> breaker(cfg);
    VirtualClock pal;
    AdmT2RollbackCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), ao, ao.logical_prio()));

    coact::Event ev{};
    ev.signal = 1U;
    const coact::TargetId t1(1U);

    /* t=0: the one successful admission that opens the window. */
    const coact::SubmitResult r0 = submit_task(coord, t1, &ev, 0U);
    REQUIRE_EQ(r0.disposition, coact::SubmitDisposition::Queued);
    REQUIRE_EQ(ao.pending().load(), static_cast<uint16_t>(1U));

    /* Occupy the remaining Normal cells (capacity 8) without a pending
     * account, as a Dispatcher-retained backlog would. */
    coact::Event filler{};
    filler.signal = 2U;
    REQUIRE_EQ(fill_normal(staging, &filler, AdvCfg::kNormalCapacity - 1U),
               static_cast<unsigned>(AdvCfg::kNormalCapacity - 1U));

    /* t=99: below the window; must not move the window. */
    const coact::SubmitResult r99 = submit_task(coord, t1, &ev, 99U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::DroppedRateLimit),
             static_cast<int>(r99.disposition));
    CHECK_EQ(static_cast<uint16_t>(coact::kArrivalTooSoon), r99.reason);

    /* t=100: the reservation succeeds but the enqueue fails. */
    const coact::SubmitResult r100 = submit_task(coord, t1, &ev, 100U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::RejectedFull),
             static_cast<int>(r100.disposition));
    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(1U));
    /* Overflow trips L2; only critical traffic can still reach the gate. */
    CHECK_EQ(static_cast<int>(coact::BreakerLevel::BrokenL2),
             static_cast<int>(breaker.level()));

    /* Free the slot the t=0 admission occupies (FIFO head). */
    REQUIRE(drain_completed(staging, ao, 100U));
    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(0U));

    /* Rollback required: the window is still at 0, so exactly min later is
     * admissible. Without rollback this returns DroppedRateLimit. */
    const coact::SubmitResult retry = submit_task(coord, t1, &ev, 100U, true);
    REQUIRE_EQ(retry.disposition, coact::SubmitDisposition::Queued);
    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(1U));

    /* Free one filler so the next successful enqueue has a cell. */
    REQUIRE(drain_raw(staging, 100U));

    /* Successful admission at 100 moved the window: 199 is too soon. */
    const coact::SubmitResult r199 = submit_task(coord, t1, &ev, 199U, true);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::DroppedRateLimit),
             static_cast<int>(r199.disposition));
    CHECK_EQ(static_cast<uint16_t>(coact::kArrivalTooSoon), r199.reason);

    /* Exactly min after the successful retry: admitted again. */
    const coact::SubmitResult r200 = submit_task(coord, t1, &ev, 200U, true);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(r200.disposition));
    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(2U));
}

/* ArrivalGate is the transaction state machine. Its public state must only
 * ever describe successful admissions. */
COACT_TEST(admission_adv_t2_gate_state_only_on_success)
{
    const coact::CriticalSection cs = coact::detail::noop_cs();
    coact::ArrivalGate gate;

    /* A time of 0 is a legal admitted timestamp: admitted_ever() must be true
     * even though last_admit_ns() is 0. It is set by commit, not by reserve,
     * so the observable state describes admissions and nothing else. */
    CHECK(gate.try_reserve(cs, 0U, coact::MinArrivalNs{100U}));
    CHECK(!gate.admitted_ever(cs));            // reserved, not admitted
    CHECK_EQ(gate.holds(cs), static_cast<uint16_t>(1U));
    gate.commit(cs, 0U);
    CHECK(gate.admitted_ever(cs));
    CHECK_EQ(gate.last_admit_ns(cs), static_cast<uint64_t>(0U));

    /* Rejection leaves the window untouched. */
    CHECK(!gate.try_reserve(cs, 99U, coact::MinArrivalNs{100U}));
    CHECK_EQ(gate.last_admit_ns(cs), static_cast<uint64_t>(0U));

    /* Boundary is inclusive. */
    CHECK(gate.try_reserve(cs, 100U, coact::MinArrivalNs{100U}));
    gate.commit(cs, 100U);
    CHECK_EQ(gate.last_admit_ns(cs), static_cast<uint64_t>(100U));

    /* Rolling back a later reservation leaves the earlier commit alone. */
    CHECK(gate.try_reserve(cs, 200U, coact::MinArrivalNs{100U}));
    gate.rollback(cs);
    CHECK(gate.admitted_ever(cs));
    CHECK_EQ(gate.last_admit_ns(cs), static_cast<uint64_t>(100U));
    CHECK_EQ(gate.holds(cs), static_cast<uint16_t>(0U));
    CHECK(gate.try_reserve(cs, 200U, coact::MinArrivalNs{100U}));
    gate.rollback(cs);

    /* Rolling back the ONLY reservation returns to never-admitted: no
     * submission ever succeeded, so admitted_ever() must be false. */
    coact::ArrivalGate fresh;
    CHECK(fresh.try_reserve(cs, 7U, coact::MinArrivalNs{100U}));
    CHECK(!fresh.admitted_ever(cs));
    fresh.rollback(cs);
    CHECK(!fresh.admitted_ever(cs));
    CHECK_EQ(fresh.last_admit_ns(cs), static_cast<uint64_t>(0U));
    /* ...and the next submit is a first submit again immediately. */
    CHECK(fresh.try_reserve(cs, 8U, coact::MinArrivalNs{100U}));
}
/* Compile-time rule set for admission_adv_t2_rollback_after_lost_direct_lease (table). */
ADM_CASE(AdmT2RollbackAfter, AdvStage,
         VirtualClock, coact::Breaker<AdvCfg>,
         AdvCfg, ADM_RULE(1U, 100U, 0U));

/* Contract 2 verbatim: an enqueue failure after losing the direct lease must
 * roll the window back. The AO holds its lease, so the direct path cannot run
 * and the submission reaches staging, where the lane is full. */
COACT_TEST(admission_adv_t2_rollback_after_lost_direct_lease)
{

    DirectAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    ao.init(ie);

    AdvStage staging = make_stage();
    coact::AoRegistry<AdvCfg> registry;
    coact::Monitor<AdvCfg> monitor;
    AdvCfg cfg{};
    coact::Breaker<AdvCfg> breaker(cfg);
    VirtualClock pal;
    AdmT2RollbackAfterCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), ao, ao.logical_prio()));

    coact::Event ev{};
    ev.signal = 1U;
    coact::Event filler{};
    filler.signal = 2U;
    REQUIRE_EQ(fill_normal(staging, &filler, AdvCfg::kNormalCapacity),
               static_cast<unsigned>(AdvCfg::kNormalCapacity));

    /* Occupy the direct lease: the coordinator sees non-Idle and falls to
     * staging. This is the "lost the direct race" path, not a re-entry. */
    REQUIRE(ao.lease().try_acquire(coact::AoRunState::RunningDirect));
    const coact::SubmitResult first =
        submit_task(coord, coact::TargetId(1U), &ev, 0U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::RejectedFull),
             static_cast<int>(first.disposition));
    CHECK_EQ(static_cast<uint16_t>(0U), first.reason);  /* full lane, not cap */
    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(0U));
    CHECK_EQ(static_cast<int>(coact::BreakerLevel::BrokenL2),
             static_cast<int>(breaker.level()));
    ao.lease().release(coact::AoRunState::RunningDirect);

    REQUIRE(drain_raw(staging, 0U));

    /* The interval reservation of the failed submission must be gone: this is
     * still the first successful admission, so t=0 is admissible even though
     * a consumed window would put it 0 ns after the prior reservation. */
    const coact::SubmitResult retry =
        submit_task(coord, coact::TargetId(1U), &ev, 0U, true);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(retry.disposition));
    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(1U));
}

/* A rollback must never move the window, in either order. Restoring the word a
 * reservation observed instead resurrects a timestamp whose own submission
 * already failed: A reserves 100, B reserves 200, A fails first (its compare
 * no longer matches), B fails and restores 100, and the gate then claims an
 * admission at 100 that never happened and stays closed until 200. */
COACT_TEST(admission_adv_t2_rollback_does_not_clobber_later_admission)
{
    const coact::CriticalSection cs = coact::detail::noop_cs();
    const coact::MinArrivalNs min_ns{100U};

    /* One commit and one failure: the commit is the window, the failure is
     * invisible. */
    coact::ArrivalGate gate;
    CHECK(gate.try_reserve(cs, 100U, min_ns));
    CHECK(gate.try_reserve(cs, 200U, min_ns));
    gate.commit(cs, 200U);
    CHECK_EQ(gate.last_admit_ns(cs), static_cast<uint64_t>(200U));
    gate.rollback(cs);                         // the 100 reservation fails
    CHECK_EQ(gate.last_admit_ns(cs), static_cast<uint64_t>(200U));
    CHECK_EQ(gate.holds(cs), static_cast<uint16_t>(0U));
    CHECK(!gate.try_reserve(cs, 250U, min_ns));
    CHECK(gate.try_reserve(cs, 300U, min_ns));
    gate.rollback(cs);

    /* Both fail: back to never-admitted, and the interval they occupied is
     * reusable instead of being held closed by a phantom admission. */
    coact::ArrivalGate both;
    CHECK(both.try_reserve(cs, 100U, min_ns));
    CHECK(both.try_reserve(cs, 200U, min_ns));
    both.rollback(cs);
    both.rollback(cs);
    CHECK(!both.admitted_ever(cs));
    CHECK_EQ(both.last_admit_ns(cs), static_cast<uint64_t>(0U));
    CHECK_EQ(both.holds(cs), static_cast<uint16_t>(0U));
    CHECK(both.try_reserve(cs, 100U, min_ns));
    CHECK(both.try_reserve(cs, 101U, min_ns) == false);
}
/* Compile-time rule set for admission_adv_t3_backlog_cap (table). */
ADM_CASE(AdmT3Backlog, AdvStage,
         VirtualClock, coact::Breaker<AdvCfg>,
         AdvCfg, ADM_RULE(1U, 0U, 2U));

/* =====================================================================
 * T3: backlog cap. max_backlog counts accepted-but-unfinished queued
 * events; the cap hit is RejectedFull + reason kBacklogFull and must not
 * touch pending, the breaker, or ref counts, and critical must not bypass.
 * ===================================================================== */
COACT_TEST(admission_adv_t3_backlog_cap)
{

    StageAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    ao.init(ie);

    AdvStage staging = make_stage();
    coact::AoRegistry<AdvCfg> registry;
    coact::Monitor<AdvCfg> monitor;
    AdvCfg cfg{};
    coact::Breaker<AdvCfg> breaker(cfg);
    VirtualClock pal;
    AdmT3BacklogCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), ao, ao.logical_prio()));

    AdvPool pool;
    REQUIRE(pool.init(g_pool_storage, sizeof(g_pool_storage)));

    const coact::TargetId t1(1U);
    for (int i = 0; i < 2; ++i) {
        coact::Event* e = pool.alloc(1U);
        REQUIRE(e != nullptr);
        const coact::SubmitResult r = submit_task(coord, t1, e, 0U);
        REQUIRE_EQ(r.disposition, coact::SubmitDisposition::Queued);
    }
    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(2U));
    CHECK_EQ(pool.used(), static_cast<uint16_t>(2U));

    /* Third: cap hit. The queue still has room, so only the cap can refuse. */
    coact::Event* third = pool.alloc(1U);
    REQUIRE(third != nullptr);
    const coact::SubmitResult r3 = submit_task(coord, t1, third, 0U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::RejectedFull),
             static_cast<int>(r3.disposition));
    CHECK_EQ(static_cast<uint16_t>(coact::kBacklogFull), r3.reason);
    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(2U));
    CHECK_EQ(pool.used(), static_cast<uint16_t>(2U));  /* ref consumed once */
    CHECK_EQ(static_cast<int>(coact::BreakerLevel::Normal),
             static_cast<int>(breaker.level()));

    /* critical=true must not bypass the cap. */
    coact::Event* critical = pool.alloc(1U);
    REQUIRE(critical != nullptr);
    const coact::SubmitResult rc = submit_task(coord, t1, critical, 0U, true);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::RejectedFull),
             static_cast<int>(rc.disposition));
    CHECK_EQ(static_cast<uint16_t>(coact::kBacklogFull), rc.reason);
    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(2U));
    CHECK_EQ(pool.used(), static_cast<uint16_t>(2U));
    CHECK_EQ(static_cast<int>(coact::BreakerLevel::Normal),
             static_cast<int>(breaker.level()));

    /* Every cap rejection is captured in the fixed snapshot ring. */
    CHECK_EQ(monitor.admission_snapshot_count(), static_cast<uint32_t>(2U));
    coact::Monitor<AdvCfg>::AdmissionSnapshot snap{};
    REQUIRE(snap_get(monitor, 1U, snap));
    CHECK_EQ(snap.seq, static_cast<uint32_t>(1U));
    CHECK_EQ(snap.target, static_cast<uint8_t>(1U));
    CHECK_EQ(static_cast<int>(snap.disposition),
             static_cast<int>(coact::SubmitDisposition::RejectedFull));
    CHECK_EQ(snap.reason, static_cast<uint16_t>(coact::kBacklogFull));
    CHECK_EQ(snap.now_ns, static_cast<uint64_t>(0U));
    CHECK_EQ(snap.budget_ns, static_cast<uint64_t>(1000000U));

    /* Drain both: no leaked reservation may survive. */
    REQUIRE(drain_completed(staging, ao, 0U));
    REQUIRE(drain_completed(staging, ao, 0U));
    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(0U));

    coact::Event* after = pool.alloc(1U);
    REQUIRE(after != nullptr);
    const coact::SubmitResult r4 = submit_task(coord, t1, after, 0U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(r4.disposition));
    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(1U));
}
/* Compile-time rule set for admission_adv_t3_pending_increment_is_atomic (table). */
ADM_CASE(AdmT3Pending, AdvStage,
         VirtualClock, coact::Breaker<AdvCfg>,
         AdvCfg, ADM_RULE(1U, 0U, 2U));

/* The cap check and the pending reservation are one CAS. With limit 2 and
 * no drain, exactly two concurrent submissions may be accepted; anything
 * more proves a load()-then-increment() window. */
COACT_TEST(admission_adv_t3_pending_increment_is_atomic)
{

    StageAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    ao.init(ie);

    AdvStage staging = make_stage();
    coact::AoRegistry<AdvCfg> registry;
    coact::Monitor<AdvCfg> monitor;
    AdvCfg cfg{};
    coact::Breaker<AdvCfg> breaker(cfg);
    VirtualClock pal;
    AdmT3PendingCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), ao, ao.logical_prio()));

    constexpr int kThreads = 8;
    constexpr int kPerThread = 25;
    std::atomic<int> accepted{0};
    std::atomic<int> wrong{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&coord, &accepted, &wrong]() {
            coact::Event ev{};
            ev.signal = 1U;
            for (int k = 0; k < kPerThread; ++k) {
                const coact::SubmitResult r = submit_task(
                    coord, coact::TargetId(1U), &ev, 0U, (k % 2) != 0);
                if (coact::SubmitDisposition::Queued == r.disposition) {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                }
                else if ((coact::SubmitDisposition::RejectedFull != r.disposition)
                         || (static_cast<uint16_t>(coact::kBacklogFull) != r.reason)) {
                    wrong.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }

    CHECK_EQ(wrong.load(), 0);
    CHECK_EQ(accepted.load(), 2);
    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(2U));
}
/* Compile-time rule set for admission_adv_t3_cap_hit_rolls_back_window (table). */
ADM_CASE(AdmT3Cap, AdvStage,
         VirtualClock, coact::Breaker<AdvCfg>,
         AdvCfg, ADM_RULE(1U, 100U, 1U));

/* A cap hit that happens after the interval reservation must roll the window
 * back: the submission was never accepted, so it cannot consume the gap. */
COACT_TEST(admission_adv_t3_cap_hit_rolls_back_window)
{

    StageAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    ao.init(ie);

    AdvStage staging = make_stage();
    coact::AoRegistry<AdvCfg> registry;
    coact::Monitor<AdvCfg> monitor;
    AdvCfg cfg{};
    coact::Breaker<AdvCfg> breaker(cfg);
    VirtualClock pal;
    AdmT3CapCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), ao, ao.logical_prio()));

    coact::Event ev{};
    ev.signal = 1U;
    const coact::TargetId t1(1U);

    REQUIRE_EQ(submit_task(coord, t1, &ev, 0U).disposition,
               coact::SubmitDisposition::Queued);
    /* t=100: interval OK, then the cap refuses. */
    const coact::SubmitResult capped = submit_task(coord, t1, &ev, 100U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::RejectedFull),
             static_cast<int>(capped.disposition));
    CHECK_EQ(static_cast<uint16_t>(coact::kBacklogFull), capped.reason);
    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(1U));

    /* Free the seat; the window is still at 0 so 100 is exactly legal. */
    REQUIRE(drain_completed(staging, ao, 100U));
    const coact::SubmitResult retry = submit_task(coord, t1, &ev, 100U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(retry.disposition));
    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(1U));
}

/* A policy rejection happens before the interval gate: it must not consume
 * the window either (design 4.1 "拒绝...不得消耗间隔"). */
struct PolicyOnceCtx {
    int calls;
};

static coact::PolicyResult policy_reject_first(void* context, coact::TargetId,
                                               const coact::Event&,
                                               const coact::EventQos&, uint64_t)
{
    PolicyOnceCtx* ctx = static_cast<PolicyOnceCtx*>(context);
    if (0 == ctx->calls) {
        ++ctx->calls;
        return coact::PolicyResult{false, false, 1U};
    }
    ++ctx->calls;
    return coact::PolicyResult{true, false, 0U};
}

static bool policy_no_merge(void*, coact::Event&, const coact::Event&)
{
    return false;
}
/* Compile-time rule set for admission_adv_t3_policy_rejection_does_not_consume_window (table). */
ADM_CASE(AdmT3Policy, AdvStage,
         VirtualClock, coact::Breaker<AdvCfg>,
         AdvCfg, ADM_RULE(1U, 100U, 0U));

COACT_TEST(admission_adv_t3_policy_rejection_does_not_consume_window)
{
    const coact::PolicyOps ops{&policy_reject_first, &policy_no_merge};
    PolicyOnceCtx ctx{0};

    StageAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    ao.init(ie);

    AdvStage staging = make_stage();
    coact::AoRegistry<AdvCfg> registry;
    coact::Monitor<AdvCfg> monitor;
    AdvCfg cfg{};
    coact::Breaker<AdvCfg> breaker(cfg);
    VirtualClock pal;
    AdmT3PolicyCoord coord(staging, registry, monitor, breaker, pal, &ops, &ctx);
    REQUIRE(registry.bind_at(coact::TargetId(1U), ao, ao.logical_prio()));

    coact::Event ev{};
    ev.signal = 1U;
    const coact::TargetId t1(1U);

    /* Policy rejection: existing semantics (DroppedRateLimit, reason 1). */
    const coact::SubmitResult rejected = submit_task(coord, t1, &ev, 500U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::DroppedRateLimit),
             static_cast<int>(rejected.disposition));
    CHECK_EQ(static_cast<uint16_t>(1U), rejected.reason);

    /* Same instant, policy now accepts: this is the first admission, so the
     * gate must not have been advanced by the policy rejection. */
    const coact::SubmitResult admitted = submit_task(coord, t1, &ev, 500U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(admitted.disposition));
    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(1U));
}

/* Frozen PendingCounter unit contract: limit 0 is unlimited, limit N stops at
 * N, and the counter never underflows through the cap path. */
COACT_TEST(admission_adv_t3_pending_counter_contract)
{
    coact::PendingCounter p;
    CHECK_EQ(p.load(), static_cast<uint16_t>(0U));
    CHECK(p.try_increment(2U));
    CHECK(p.try_increment(2U));
    CHECK(!p.try_increment(2U));
    CHECK_EQ(p.load(), static_cast<uint16_t>(2U));

    /* limit 0 = unlimited */
    CHECK(p.try_increment(0U));
    CHECK_EQ(p.load(), static_cast<uint16_t>(3U));
    p.decrement();
    p.decrement();
    p.decrement();
    CHECK_EQ(p.load(), static_cast<uint16_t>(0U));
    CHECK(p.try_increment(0U));
    CHECK_EQ(p.load(), static_cast<uint16_t>(1U));
    p.decrement();
}
/* Compile-time rule set for admission_adv_t4_concurrent_accept_bound (table). */
ADM_CASE(AdmT4Concurrent, BigStage,
         VirtualClock, coact::Breaker<BigCfg>,
         BigCfg, ADM_RULE(1U, 100U, 0U));

/* =====================================================================
 * T4: concurrency. Logical clock only; accepted count may never exceed
 * 1 + floor(span / min), and every refusal must be the interval gate.
 * ===================================================================== */
COACT_TEST(admission_adv_t4_concurrent_accept_bound)
{
    constexpr uint64_t kMin = 100U;
    constexpr uint64_t kStep = 25U;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 100;
    /* The bound arithmetic below and the compile-time rule must agree. */
    static_assert(AdmT4ConcurrentAdm::kRules.rows[0].min_arrival.value == kMin,
                  "rule set interval diverged from kMin");

    StageAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    ao.init(ie);

    static BigStage staging = make_big_stage();
    coact::AoRegistry<BigCfg> registry;
    coact::Monitor<BigCfg> monitor;
    BigCfg cfg{};
    coact::Breaker<BigCfg> breaker(cfg);
    VirtualClock pal;
    AdmT4ConcurrentCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), ao, ao.logical_prio()));

    std::atomic<uint64_t> ticker{0U};
    std::atomic<int> accepted{0};
    std::atomic<int> wrong{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&]() {
            coact::Event ev{};
            ev.signal = 1U;
            for (int k = 0; k < kPerThread; ++k) {
                const uint64_t tick =
                    ticker.fetch_add(1U, std::memory_order_relaxed);
                const coact::SubmitResult r = submit_task(
                    coord, coact::TargetId(1U), &ev, tick * kStep);
                if (coact::SubmitDisposition::Queued == r.disposition) {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                }
                else if ((coact::SubmitDisposition::DroppedRateLimit
                          != r.disposition)
                         || (static_cast<uint16_t>(coact::kArrivalTooSoon)
                             != r.reason)) {
                    wrong.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }

    const uint64_t consumed = ticker.load(std::memory_order_relaxed);
    REQUIRE(consumed >= 2U);
    const uint64_t span = (consumed - 1U) * kStep;
    const uint64_t bound = 1U + (span / kMin);
    const int accepted_count = accepted.load();

    std::printf("T4 concurrency: accepted=%d bound=%llu (span=%llu, min=%llu, "
                "submissions=%llu)\n",
                accepted_count, static_cast<unsigned long long>(bound),
                static_cast<unsigned long long>(span),
                static_cast<unsigned long long>(kMin),
                static_cast<unsigned long long>(consumed));
    CHECK_EQ(wrong.load(), 0);
    CHECK(accepted_count >= 1);
    CHECK(static_cast<uint64_t>(accepted_count) <= bound);
    /* Every accepted event is still resident: pending mirrors the count and
     * no phantom reservation leaked into the cap. */
    CHECK_EQ(static_cast<int>(ao.pending().load()), accepted_count);
}

/* Gate-level monotonicity: admitted timestamps are spaced by at least min,
 * and the published last_admit_ns() never falls behind the largest admitted
 * timestamp. */
COACT_TEST(admission_adv_t4_gate_monotonic)
{
    constexpr uint64_t kMin = 100U;
    constexpr uint64_t kStep = 37U;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 60;

    /* Eight producers on one gate: the spin backend is what AdmissionSpinSync
     * binds on an SMP host, so this exercises the same serialization the
     * policy uses instead of a test-private lock. */
    coact::SpinCriticalSection spin;
    const coact::CriticalSection cs = coact::make_spin_critical_section(spin);
    coact::ArrivalGate gate;
    std::atomic<uint64_t> ticker{0U};
    std::mutex m;
    std::vector<uint64_t> admitted;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&gate, &cs, &ticker, &m, &admitted]() {
            for (int k = 0; k < kPerThread; ++k) {
                const uint64_t now =
                    ticker.fetch_add(1U, std::memory_order_relaxed) * kStep;
                if (gate.try_reserve(cs, now, coact::MinArrivalNs{kMin})) {
                    /* The submit path commits an admission it kept; a
                     * reservation left in flight would stay the barrier. */
                    gate.commit(cs, now);
                    std::lock_guard<std::mutex> lock(m);
                    admitted.push_back(now);
                }
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }

    const uint64_t consumed = ticker.load(std::memory_order_relaxed);
    const uint64_t span = (consumed - 1U) * kStep;
    const uint64_t bound = 1U + (span / kMin);
    std::sort(admitted.begin(), admitted.end());

    CHECK(!admitted.empty());
    CHECK(admitted.size() <= bound);
    /* Monotonicity at the byte level: every pair of admitted arrivals in
     * time order is at least min apart. */
    for (size_t i = 1U; i < admitted.size(); ++i) {
        CHECK(admitted[i] - admitted[i - 1U] >= kMin);
    }
    /* The published timestamp is exactly the newest successful arrival. */
    CHECK(gate.admitted_ever(cs));
    CHECK_EQ(gate.last_admit_ns(cs), admitted.back());
    CHECK_EQ(gate.holds(cs), static_cast<uint16_t>(0U));
}
/* Compile-time rule set for admission_adv_t5_isr_interval_and_task_interleave (table). */
ADM_CASE(AdmT5Isr, AdvStage,
         VirtualClock, coact::Breaker<AdvCfg>,
         AdvCfg, ADM_RULE(1U, 100U, 0U));

/* =====================================================================
 * T5: ISR path shares the same window as the task path.
 * ===================================================================== */
COACT_TEST(admission_adv_t5_isr_interval_and_task_interleave)
{

    StageAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    ao.init(ie);

    AdvStage staging = make_stage();
    coact::AoRegistry<AdvCfg> registry;
    coact::Monitor<AdvCfg> monitor;
    AdvCfg cfg{};
    coact::Breaker<AdvCfg> breaker(cfg);
    VirtualClock pal;
    AdmT5IsrCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), ao, ao.logical_prio()));

    coact::Event ev{};
    ev.signal = 1U;
    const coact::TargetId t1(1U);
    const uint16_t too_soon = static_cast<uint16_t>(coact::kArrivalTooSoon);

    /* Same interval rule on the ISR path. */
    const coact::SubmitResult i0 = submit_isr(coord, t1, &ev, 0U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(i0.disposition));
    const coact::SubmitResult i99 = submit_isr(coord, t1, &ev, 99U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::DroppedRateLimit),
             static_cast<int>(i99.disposition));
    CHECK_EQ(too_soon, i99.reason);
    const coact::SubmitResult i100 = submit_isr(coord, t1, &ev, 100U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(i100.disposition));

    /* Identical clock, different producers: only one may win the window. */
    const coact::SubmitResult task200 = submit_task(coord, t1, &ev, 200U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(task200.disposition));
    const coact::SubmitResult isr200 = submit_isr(coord, t1, &ev, 200U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::DroppedRateLimit),
             static_cast<int>(isr200.disposition));
    CHECK_EQ(too_soon, isr200.reason);

    /* And the other order at the next window. */
    const coact::SubmitResult isr300 = submit_isr(coord, t1, &ev, 300U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(isr300.disposition));
    const coact::SubmitResult task300 = submit_task(coord, t1, &ev, 300U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::DroppedRateLimit),
             static_cast<int>(task300.disposition));
    CHECK_EQ(too_soon, task300.reason);

    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(4U));
}

/* =====================================================================
 * T6: default-off invariance (admission == nullptr).
 *
 * Baseline is the pre-change coordinator at git 73fe452:
 *   M6 guard: BreakerLevel::BrokenL2 <= level && !qos.critical ->
 *             DroppedOverload (reason 0), event_gc, pending untouched.
 *   enqueue failure: pending.increment(); on false pending.decrement();
 *             event_gc; record_overflow(); breaker.on_overflow() -> L2;
 *             RejectedFull (reason 0).
 * Nothing in this file may change those dispositions, pending or breaker.
 * ===================================================================== */
COACT_TEST(admission_adv_t6_default_off_staged)
{
    StageAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    ao.init(ie);

    AdvStage staging = make_stage();
    coact::AoRegistry<AdvCfg> registry;
    coact::Monitor<AdvCfg> monitor;
    AdvCfg cfg{};
    coact::Breaker<AdvCfg> breaker(cfg);
    VirtualClock pal;
    /* admission left at its default (nullptr): the historical path. */
    coact::DispatchCoordinator<AdvStage, VirtualClock, coact::Breaker<AdvCfg>>
        coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), ao, ao.logical_prio()));

    AdvPool pool;
    REQUIRE(pool.init(g_pool_storage, sizeof(g_pool_storage)));

    /* Normal capacity is 8: 8 Queued, 9th RejectedFull (and L2), 10th
     * DroppedOverload because L2 refuses non-critical traffic. */
    coact::SubmitDisposition expected[10];
    for (int i = 0; i < 8; ++i) {
        expected[i] = coact::SubmitDisposition::Queued;
    }
    expected[8] = coact::SubmitDisposition::RejectedFull;
    expected[9] = coact::SubmitDisposition::DroppedOverload;

    for (int i = 0; i < 10; ++i) {
        coact::Event* e = pool.alloc(1U);
        REQUIRE(e != nullptr);
        const coact::SubmitResult r =
            submit_task(coord, coact::TargetId(1U), e, 0U);
        CHECK_EQ(static_cast<int>(expected[i]),
                 static_cast<int>(r.disposition));
        CHECK_EQ(static_cast<uint16_t>(0U), r.reason);
    }

    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(8U));
    CHECK_EQ(pool.used(), static_cast<uint16_t>(8U));
    CHECK_EQ(coact_test::relaxed(monitor.global().overflow),
             static_cast<uint32_t>(1U));
    CHECK_EQ(static_cast<int>(breaker.level()),
             static_cast<int>(coact::BreakerLevel::BrokenL2));
}

COACT_TEST(admission_adv_t6_default_off_direct)
{
    DirectAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    ao.init(ie);

    AdvStage staging = make_stage();
    coact::AoRegistry<AdvCfg> registry;
    coact::Monitor<AdvCfg> monitor;
    AdvCfg cfg{};
    coact::Breaker<AdvCfg> breaker(cfg);
    VirtualClock pal;
    coact::DispatchCoordinator<AdvStage, VirtualClock, coact::Breaker<AdvCfg>>
        coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), ao, ao.logical_prio()));

    AdvPool pool;
    REQUIRE(pool.init(g_pool_storage, sizeof(g_pool_storage)));

    /* Ten direct submissions at the same instant: without a table every one
     * is directly dispatched (the baseline has no arrival concept). */
    for (int i = 0; i < 10; ++i) {
        coact::Event* e = pool.alloc(1U);
        REQUIRE(e != nullptr);
        const coact::SubmitResult r =
            submit_task(coord, coact::TargetId(1U), e, 0U);
        CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Direct),
                 static_cast<int>(r.disposition));
        CHECK_EQ(pool.used(), static_cast<uint16_t>(0U));
    }
    CHECK_EQ(ao.pending().load(), static_cast<uint16_t>(0U));
    CHECK_EQ(static_cast<int>(breaker.level()),
             static_cast<int>(coact::BreakerLevel::Normal));
}
/* Compile-time rule set for admission_adv_t7_cross_target_isolation (table). */
ADM_CASE(AdmT7Cross, AdvStage,
         VirtualClock, coact::BreakerBank<AdvCfg>,
         AdvCfg, ADM_RULE(1U, 100U, 1U), ADM_RULE(2U, 100U, 0U));

/* =====================================================================
 * T7: per-target isolation of both limits.
 * ===================================================================== */
COACT_TEST(admission_adv_t7_cross_target_isolation)
{

    StageAo a1(kStates, 2U, kTrans, 1U, 1, 4U);
    Stage2Ao a2(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    a1.init(ie);
    a2.init(ie);

    AdvStage staging = make_stage();
    coact::AoRegistry<AdvCfg> registry;
    coact::Monitor<AdvCfg> monitor;
    AdvCfg cfg{};
    coact::BreakerBank<AdvCfg> breakers(cfg);
    VirtualClock pal;
    AdmT7CrossCoord coord(staging, registry, monitor, breakers, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), a1, a1.logical_prio()));
    REQUIRE(registry.bind_at(coact::TargetId(2U), a2, a2.logical_prio()));

    coact::Event ev{};
    ev.signal = 1U;
    const coact::TargetId t1(1U);
    const coact::TargetId t2(2U);

    /* Open both windows at different times. */
    REQUIRE_EQ(submit_task(coord, t1, &ev, 0U).disposition,
               coact::SubmitDisposition::Queued);
    REQUIRE_EQ(submit_task(coord, t2, &ev, 50U).disposition,
               coact::SubmitDisposition::Queued);

    /* Target 1 rejected at 50: it must not move target 2's window. */
    const coact::SubmitResult r1 = submit_task(coord, t1, &ev, 50U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::DroppedRateLimit),
             static_cast<int>(r1.disposition));
    CHECK_EQ(static_cast<uint16_t>(coact::kArrivalTooSoon), r1.reason);

    /* Target 2 at 150 is exactly min after its own admission (50). */
    const coact::SubmitResult ok2 = submit_task(coord, t2, &ev, 150U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(ok2.disposition));

    /* Target 1 at 100 hits its own backlog cap (1) -> reason 5. */
    const coact::SubmitResult cap1 = submit_task(coord, t1, &ev, 100U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::RejectedFull),
             static_cast<int>(cap1.disposition));
    CHECK_EQ(static_cast<uint16_t>(coact::kBacklogFull), cap1.reason);

    /* Target 2's window is untouched by target 1's cap rejection: 249 is
     * exactly min after its own 150 admission and must be accepted. */
    const coact::SubmitResult too_soon2 = submit_task(coord, t2, &ev, 249U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::DroppedRateLimit),
             static_cast<int>(too_soon2.disposition));
    CHECK_EQ(static_cast<uint16_t>(coact::kArrivalTooSoon), too_soon2.reason);
    const coact::SubmitResult ok2b = submit_task(coord, t2, &ev, 250U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(ok2b.disposition));

    /* Target 2's backlog cap is disabled; two queued events are fine. */
    const coact::SubmitResult ok2c = submit_task(coord, t2, &ev, 350U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(ok2c.disposition));

    /* Target 1's window did move on its successful admission at 0 and is
     * still open at 100; only the cap blocked it. Drain then retry. */
    REQUIRE(drain_completed(staging, a1, 100U));
    const coact::SubmitResult retry1 = submit_task(coord, t1, &ev, 100U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(retry1.disposition));

    CHECK_EQ(a1.pending().load(), static_cast<uint16_t>(1U));
    CHECK_EQ(a2.pending().load(), static_cast<uint16_t>(4U));
    CHECK_EQ(static_cast<int>(breakers.level(t1)),
             static_cast<int>(coact::BreakerLevel::Normal));
    CHECK_EQ(static_cast<int>(breakers.level(t2)),
             static_cast<int>(coact::BreakerLevel::Normal));
}
/* Compile-time rule set for admission_adv_t7_absent_target_limits_disabled (table). */
ADM_CASE(AdmT7AbsentA, AdvStage,
         VirtualClock, coact::Breaker<AdvCfg>,
         AdvCfg, ADM_RULE(1U, 1000U, 1U));
/* Enabled policy with no rules at all: every limit off. Declared without
 * ADM_CASE because C++17 requires at least one argument for "...". */
struct AdmT7AbsentBRules {
    static constexpr auto kRules = coact::make_rules();
};
using AdmT7AbsentBAdm =
    coact::StaticAdmission<AdmT7AbsentBRules, AdvCfg,
                           coact::AdmissionSpinSync>;
using AdmT7AbsentBCoord =
    coact::DispatchCoordinator<AdvStage, VirtualClock,
                               coact::Breaker<AdvCfg>, AdmT7AbsentBAdm>;

/* A target with no row (or an empty table) keeps both limits disabled. */
COACT_TEST(admission_adv_t7_absent_target_limits_disabled)
{
    DirectAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    ao.init(ie);
    coact::Event ev{};
    ev.signal = 1U;

    /* Table present, but target 2 is absent: unlimited everything. */
    {
        AdvStage staging = make_stage();
        coact::AoRegistry<AdvCfg> registry;
        coact::Monitor<AdvCfg> monitor;
        AdvCfg cfg{};
        coact::Breaker<AdvCfg> breaker(cfg);
        VirtualClock pal;
        AdmT7AbsentACoord coord(staging, registry, monitor, breaker, pal);
        REQUIRE(registry.bind_at(coact::TargetId(2U), ao, ao.logical_prio()));
        for (int i = 0; i < 3; ++i) {
            CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Direct),
                     static_cast<int>(
                         submit_task(coord, coact::TargetId(2U), &ev, 0U)
                             .disposition));
        }
    }

    /* Non-null table with count 0 behaves like no table at all. */
    {
        AdvStage staging = make_stage();
        coact::AoRegistry<AdvCfg> registry;
        coact::Monitor<AdvCfg> monitor;
        AdvCfg cfg{};
        coact::Breaker<AdvCfg> breaker(cfg);
        VirtualClock pal;
        AdmT7AbsentBCoord coord(staging, registry, monitor, breaker, pal);
        REQUIRE(registry.bind_at(coact::TargetId(1U), ao, ao.logical_prio()));
        for (int i = 0; i < 3; ++i) {
            CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Direct),
                     static_cast<int>(
                         submit_task(coord, coact::TargetId(1U), &ev, 0U)
                             .disposition));
        }
    }
}
/* Compile-time rule set for admission_adv_t8_snapshot_ring (table). */
ADM_CASE(AdmT8Snapshot, AdvStage,
         VirtualClock, coact::Breaker<AdvCfg>,
         AdvCfg, ADM_RULE(1U, 100U, 0U));

/* =====================================================================
 * T8: fixed snapshot ring. Nine rejections fill/overwrite the 8 slots:
 * seq 1 is gone, seq 2..9 remain, seq 0 and 10 are out of range.
 * ===================================================================== */
COACT_TEST(admission_adv_t8_snapshot_ring)
{

    DirectAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    ao.init(ie);

    AdvStage staging = make_stage();
    coact::AoRegistry<AdvCfg> registry;
    coact::Monitor<AdvCfg> monitor;
    AdvCfg cfg{};
    coact::Breaker<AdvCfg> breaker(cfg);
    VirtualClock pal;
    AdmT8SnapshotCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), ao, ao.logical_prio()));

    coact::Event ev{};
    ev.signal = 1U;
    const coact::TargetId t1(1U);

    /* One successful admission (must NOT be snapshotted) at t=0. */
    REQUIRE_EQ(submit_task(coord, t1, &ev, 0U).disposition,
               coact::SubmitDisposition::Direct);

    /* Nine interval rejections at t=1..9. */
    for (uint64_t now = 1U; now <= 9U; ++now) {
        const coact::SubmitResult r = submit_task(coord, t1, &ev, now);
        REQUIRE_EQ(r.disposition, coact::SubmitDisposition::DroppedRateLimit);
        REQUIRE_EQ(r.reason,
                   static_cast<uint16_t>(coact::kArrivalTooSoon));
    }

    CHECK_EQ(monitor.admission_snapshot_count(), static_cast<uint32_t>(9U));
    CHECK_EQ(static_cast<int>(coact::Monitor<AdvCfg>::kAdmissionSnapshotSlots),
             static_cast<int>(8));

    coact::Monitor<AdvCfg>::AdmissionSnapshot out{};
    CHECK(!snap_get(monitor, 0U, out));
    CHECK(!snap_get(monitor, 10U, out));
    /* seq 1 fell out of the 8-slot ring: a lookup must either fail or return
     * that exact record, never whatever now occupies the slot. */
    if (snap_get(monitor, 1U, out)) {
        CHECK_EQ(out.seq, static_cast<uint32_t>(1U));
    }

    for (uint32_t seq = 2U; seq <= 9U; ++seq) {
        REQUIRE(snap_get(monitor, seq, out));
        CHECK_EQ(out.seq, seq);
        CHECK_EQ(out.target, static_cast<uint8_t>(1U));
        CHECK_EQ(static_cast<int>(out.disposition),
                 static_cast<int>(coact::SubmitDisposition::DroppedRateLimit));
        CHECK_EQ(out.reason,
                 static_cast<uint16_t>(coact::kArrivalTooSoon));
        CHECK_EQ(out.now_ns, static_cast<uint64_t>(seq));
        CHECK_EQ(out.budget_ns, static_cast<uint64_t>(1000000U));
    }

    /* The three failure modes are distinct facts. A bool return would flatten
     * "nothing recorded yet", "never existed" and "already overwritten" into
     * one silent false, and a diagnostics reader cannot act on that. */
    using SnapshotError = coact::Monitor<AdvCfg>::SnapshotError;
    {
        coact::Monitor<AdvCfg> fresh;
        const auto empty = fresh.admission_snapshot(1U);
        CHECK(!empty);
        CHECK_EQ(static_cast<int>(empty.error()),
                 static_cast<int>(SnapshotError::kEmpty));
    }
    const auto zero_seq = monitor.admission_snapshot(0U);
    CHECK(!zero_seq);
    CHECK_EQ(static_cast<int>(zero_seq.error()),
             static_cast<int>(SnapshotError::kOutOfRange));
    const auto beyond = monitor.admission_snapshot(10U);
    CHECK(!beyond);
    CHECK_EQ(static_cast<int>(beyond.error()),
             static_cast<int>(SnapshotError::kOutOfRange));
    const auto evicted = monitor.admission_snapshot(1U);
    CHECK(!evicted);
    CHECK_EQ(static_cast<int>(evicted.error()),
             static_cast<int>(SnapshotError::kEvicted));
    /* seq 2 is the oldest surviving record and must still be readable. */
    const auto oldest = monitor.admission_snapshot(2U);
    CHECK(static_cast<bool>(oldest));
    CHECK_EQ(oldest.value().seq, static_cast<uint32_t>(2U));
}
/* Compile-time rule set for admission_adv_t8_snapshot_covers_every_rejection (table). */
ADM_CASE(AdmT8SnapshotCovers, AdvStage,
         VirtualClock, coact::Breaker<AdvCfg>,
         AdvCfg, ADM_RULE(1U, 100U, 0U));

/* Strict reading of "the snapshot records a rejection": a rejection caused by
 * a full staging lane is a rejection too, and must not vanish from the ring
 * (T8 counts rejections, it does not limit the count to interval misses). */
COACT_TEST(admission_adv_t8_snapshot_covers_every_rejection)
{

    StageAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    ao.init(ie);

    AdvStage staging = make_stage();
    coact::AoRegistry<AdvCfg> registry;
    coact::Monitor<AdvCfg> monitor;
    AdvCfg cfg{};
    coact::Breaker<AdvCfg> breaker(cfg);
    VirtualClock pal;
    AdmT8SnapshotCoversCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), ao, ao.logical_prio()));

    coact::Event ev{};
    ev.signal = 1U;
    const coact::TargetId t1(1U);

    REQUIRE_EQ(submit_task(coord, t1, &ev, 0U).disposition,
               coact::SubmitDisposition::Queued);
    coact::Event filler{};
    filler.signal = 2U;
    REQUIRE_EQ(fill_normal(staging, &filler, AdvCfg::kNormalCapacity - 1U),
               static_cast<unsigned>(AdvCfg::kNormalCapacity - 1U));

    /* Rejection 1: interval. */
    REQUIRE_EQ(submit_task(coord, t1, &ev, 1U).disposition,
               coact::SubmitDisposition::DroppedRateLimit);
    /* Rejection 2: lane full. */
    REQUIRE_EQ(submit_task(coord, t1, &ev, 100U).disposition,
               coact::SubmitDisposition::RejectedFull);

    if (monitor.admission_snapshot_count() != 2U) {
        std::printf("observed: admission_snapshot_count=%u after "
                    "1 interval rejection + 1 full-lane rejection at t=100\n",
                    static_cast<unsigned>(monitor.admission_snapshot_count()));
    }
    CHECK_EQ(monitor.admission_snapshot_count(), static_cast<uint32_t>(2U));
    coact::Monitor<AdvCfg>::AdmissionSnapshot snap{};
    CHECK(snap_get(monitor, 1U, snap));
    CHECK(snap_get(monitor, 2U, snap));
}
/* Compile-time rule set for admission_adv_t9_critical_and_direct_bypass (table). */
ADM_CASE(AdmT9Critical, AdvStage,
         VirtualClock, coact::BreakerBank<AdvCfg>,
         AdvCfg, ADM_RULE(1U, 0U, 1U));

/* =====================================================================
 * T9: hostile callers.
 * ===================================================================== */
COACT_TEST(admission_adv_t9_critical_and_direct_bypass)
{
    /* High-class target with a backlog cap of 1: qos.critical must not be
     * able to claim the reserved High cell past the cap. */

    HighStageAo high(kStates, 2U, kTrans, 1U, 1, 4U);
    DirectAo direct(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    high.init(ie);
    direct.init(ie);

    AdvStage staging = make_stage();
    coact::AoRegistry<AdvCfg> registry;
    coact::Monitor<AdvCfg> monitor;
    AdvCfg cfg{};
    coact::BreakerBank<AdvCfg> breakers(cfg);
    VirtualClock pal;
    AdmT9CriticalCoord coord(staging, registry, monitor, breakers, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), high, high.logical_prio()));
    REQUIRE(registry.bind_at(coact::TargetId(2U), direct, direct.logical_prio()));

    coact::Event ev{};
    ev.signal = 1U;

    /* Critical High: first accepted, second refused by the cap (not by the
     * ordinary High reserve accounting). */
    REQUIRE_EQ(submit_task(coord, coact::TargetId(1U), &ev, 0U, true).disposition,
               coact::SubmitDisposition::Queued);
    const coact::SubmitResult second =
        submit_task(coord, coact::TargetId(1U), &ev, 0U, true);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::RejectedFull),
             static_cast<int>(second.disposition));
    CHECK_EQ(static_cast<uint16_t>(coact::kBacklogFull), second.reason);
    CHECK_EQ(high.pending().load(), static_cast<uint16_t>(1U));
    CHECK_EQ(static_cast<int>(breakers.level(coact::TargetId(1U))),
             static_cast<int>(coact::BreakerLevel::Normal));

    /* Direct dispatch is in flight, not queued: the same cap must not count
     * it, so a direct target admits twice at the same instant. */
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Direct),
             static_cast<int>(submit_task(coord, coact::TargetId(2U), &ev, 0U)
                                  .disposition));
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Direct),
             static_cast<int>(submit_task(coord, coact::TargetId(2U), &ev, 0U)
                                  .disposition));
    CHECK_EQ(direct.pending().load(), static_cast<uint16_t>(0U));
}
/* Two targets with distinct limits: the compile-time index must resolve each
 * of them in one lookup, must leave unconfigured targets unlimited, and must
 * never read past the array for an id outside 1..kMaxAo.
 *
 * The three hostile tables this case used to feed the coordinator at runtime
 * (duplicate TargetId, count > kMaxAo, an interval above kMaxMinArrivalNs)
 * cannot be expressed any more: StaticAdmission rejects them where they are
 * declared. They are now compile-time gates - see
 * src/core/admission_invalid_rules_neg.cpp and the five test_admission_*_neg
 * ctest cases, which fail the build if any of them ever compiles. */
struct AdmT9IndexRules {
    static constexpr auto kRules = coact::make_rules(
        ADM_RULE(1U, 1000U, 2U), ADM_RULE(4U, 0U, 7U));
};
using AdmT9Index =
    coact::StaticAdmission<AdmT9IndexRules, AdvCfg,
                           coact::AdmissionSpinSync>;

COACT_TEST(admission_adv_t9_rule_index_is_compile_time_and_bounded)
{
    static_assert(AdvCfg::kMaxAo == 4U, "this test encodes kMaxAo == 4");
    static_assert(AdmT9Index::kRuleCount == 2U, "this test encodes two rows");
    static_assert(sizeof(AdmT9Index) == sizeof(coact::AdmissionSpinSync)
                                            + (AdvCfg::kMaxAo + 1U)
                                                  * sizeof(coact::ArrivalGate),
                  "one ArrivalGate per target plus the sync backend, nothing "
                  "else: no vtable, no runtime rule table, no padding growth");

    VirtualClock index_pal;
    AdmT9Index admission(index_pal);
    CHECK_EQ(admission.limits_for(coact::TargetId(1U)).min_arrival.value,
             static_cast<uint64_t>(1000U));
    CHECK_EQ(admission.backlog_cap(coact::TargetId(1U)),
             static_cast<uint16_t>(2U));
    CHECK_EQ(admission.limits_for(coact::TargetId(4U)).min_arrival.value,
             static_cast<uint64_t>(0U));
    CHECK_EQ(admission.backlog_cap(coact::TargetId(4U)),
             static_cast<uint16_t>(7U));

    /* In-range but unconfigured: both limits off. */
    for (uint8_t raw = 2U; raw <= 3U; ++raw) {
        CHECK_EQ(admission.limits_for(coact::TargetId(raw)).min_arrival.value,
                 static_cast<uint64_t>(0U));
        CHECK_EQ(admission.backlog_cap(coact::TargetId(raw)),
                 static_cast<uint16_t>(0U));
    }
    /* Out of range: same answer, and no out-of-bounds read (ASan clean). */
    const uint8_t oob[] = {0U, 5U, 200U, 255U};
    for (uint8_t raw : oob) {
        CHECK_EQ(admission.limits_for(coact::TargetId(raw)).min_arrival.value,
                 static_cast<uint64_t>(0U));
        CHECK_EQ(admission.backlog_cap(coact::TargetId(raw)),
                 static_cast<uint16_t>(0U));
        /* reserve() must not touch a gate it does not own either. */
        AdmT9Index::Reservation inert{};
        CHECK(admission.reserve(coact::TargetId(raw), 1U, inert));
        CHECK(!inert.active());
    }

    /* The constexpr validator agrees with the static_asserts, and narrowing
       the AO budget below a configured target invalidates the same table. */
    CHECK(AdmT9IndexRules::kRules.valid(AdvCfg::kMaxAo));
    CHECK(!AdmT9IndexRules::kRules.valid(3U));
    CHECK(!AdmT9IndexRules::kRules.valid(0U));
}

/* Compile-time rule set carrying the largest interval a rule may hold. */
ADM_CASE(AdmT9MaxInterval, AdvStage,
         VirtualClock, coact::Breaker<AdvCfg>,
         AdvCfg, ADM_RULE(1U, coact::kMaxMinArrivalNs, 0U));

/* kMaxMinArrivalNs is the top of the legal domain: anything above it is a
 * compile error, so the boundary that still has to hold at runtime is exactly
 * this value. First submission always admitted; one nanosecond short of the
 * interval rejected; exactly the interval admitted. Deadline arithmetic must
 * not wrap into "always admitted" at the top of the range. */
COACT_TEST(admission_adv_t9_max_legal_interval_boundary)
{
    DirectAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    ao.init(ie);

    AdvStage staging = make_stage();
    coact::AoRegistry<AdvCfg> registry;
    coact::Monitor<AdvCfg> monitor;
    AdvCfg cfg{};
    coact::Breaker<AdvCfg> breaker(cfg);
    VirtualClock pal;
    AdmT9MaxIntervalCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), ao, ao.logical_prio()));

    coact::Event ev{};
    ev.signal = 1U;
    const coact::TargetId t1(1U);
    const uint64_t kFirst = 5U;
    const uint64_t kMin = coact::kMaxMinArrivalNs;

    /* First is always admitted, however large the configured interval is. */
    REQUIRE_EQ(submit_task(coord, t1, &ev, kFirst).disposition,
               coact::SubmitDisposition::Direct);
    /* gap 1 is short by kMin - 1. */
    const coact::SubmitResult r6 = submit_task(coord, t1, &ev, kFirst + 1U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::DroppedRateLimit),
             static_cast<int>(r6.disposition));
    CHECK_EQ(static_cast<uint16_t>(coact::kArrivalTooSoon), r6.reason);
    /* gap == kMin - 1: still one nanosecond short, still rejected. */
    const coact::SubmitResult rshort =
        submit_task(coord, t1, &ev, kFirst + kMin - 1U);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::DroppedRateLimit),
             static_cast<int>(rshort.disposition));
    CHECK_EQ(static_cast<uint16_t>(coact::kArrivalTooSoon), rshort.reason);
    /* gap == kMin exactly: admitted, and the two rejections consumed nothing. */
    const coact::SubmitResult rexact =
        submit_task(coord, t1, &ev, kFirst + kMin);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Direct),
             static_cast<int>(rexact.disposition));
    /* The gate now holds the exact arrival, observable through the policy. */
    CHECK_EQ(coord.admission_policy().last_admit_ns(t1), kFirst + kMin);
}

/* The gate stores the timestamp it was given: there is no +1 encoding to
 * saturate, so UINT64_MAX is representable and a saturated clock cannot read
 * back as "never admitted". */
COACT_TEST(admission_adv_t9_arrival_gate_extreme_timestamp)
{
    const coact::CriticalSection cs = coact::detail::noop_cs();
    const uint64_t kMax = std::numeric_limits<uint64_t>::max();
    coact::ArrivalGate gate;

    CHECK(gate.try_reserve(cs, kMax, coact::MinArrivalNs{0U}));  /* min disabled */
    gate.commit(cs, kMax);
    CHECK(gate.admitted_ever(cs));
    /* Strict contract: last_admit_ns() is the last committed `now`. */
    CHECK_EQ(gate.last_admit_ns(cs), kMax);
    /* A second reservation at kMax cannot satisfy a positive gap. */
    CHECK(!gate.try_reserve(cs, kMax, coact::MinArrivalNs{1U}));
    /* Neither can one below it: a clock that moved back is refused. */
    CHECK(!gate.try_reserve(cs, kMax - 1U, coact::MinArrivalNs{1U}));
}
/* Compile-time rule set for admission_adv_submit_path_does_not_allocate (table). */
ADM_CASE(AdmSubmitPath, AdvStage,
         VirtualClock, coact::Breaker<AdvCfg>,
         AdvCfg, ADM_RULE(1U, 0U, 0U));

/* Requirement 7: the submit path is fixed-storage; it must not allocate. */
COACT_TEST(admission_adv_submit_path_does_not_allocate)
{

    DirectAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    ao.init(ie);

    AdvStage staging = make_stage();
    coact::AoRegistry<AdvCfg> registry;
    coact::Monitor<AdvCfg> monitor;
    AdvCfg cfg{};
    coact::Breaker<AdvCfg> breaker(cfg);
    VirtualClock pal;
    AdmSubmitPathCoord coord(staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind_at(coact::TargetId(1U), ao, ao.logical_prio()));

    coact::Event ev{};
    ev.signal = 1U;
    /* Warm-up outside the measured window. */
    (void)submit_task(coord, coact::TargetId(1U), &ev, 0U);

    const size_t before = g_alloc_count.load(std::memory_order_relaxed);
    for (uint64_t i = 0U; i < 200U; ++i) {
        (void)submit_task(coord, coact::TargetId(1U), &ev, i * 1000U);
    }
    const size_t after = g_alloc_count.load(std::memory_order_relaxed);
    CHECK_EQ(after - before, static_cast<size_t>(0U));

    /* One fixed gate per target: a per-submission object would not fit. */
    CHECK(sizeof(coact::ArrivalGate) <= 24U);
}

/* =====================================================================
 * The zero-cost claim, measured instead of asserted in prose: the default
 * policy carries no state, an enabled policy costs exactly one gate word per
 * target, and the coordinator grows by no more than that.
 * ===================================================================== */
COACT_TEST(admission_adv_disabled_policy_is_zero_cost)
{
    using PlainCoord =
        coact::DispatchCoordinator<AdvStage, VirtualClock,
                                   coact::Breaker<AdvCfg>>;
    using GatedCoord =
        coact::DispatchCoordinator<AdvStage, VirtualClock,
                                   coact::Breaker<AdvCfg>, AdmT1IntervalAdm>;
    static_assert(std::is_empty<coact::NoAdmission>::value,
                  "the default policy must carry no state");
    static_assert(std::is_empty<coact::InertReservation>::value,
                  "the disabled reservation must carry no state");

    /* The gate is four plain fields, no atomics: two timestamps, a hold count
     * and the "has committed" flag, padded to three words. */
    CHECK_EQ(sizeof(coact::ArrivalGate), static_cast<size_t>(24U));
    /* An enabled policy costs one gate per target plus the critical-section
     * hook triple, and nothing else: no vtable, no runtime rule table. */
    CHECK_EQ(sizeof(AdmT1IntervalAdm),
             sizeof(coact::AdmissionSpinSync)
                 + static_cast<size_t>(AdvCfg::kMaxAo + 1U)
                       * sizeof(coact::ArrivalGate));
    CHECK(sizeof(PlainCoord) <= sizeof(GatedCoord));
    /* The enabled build pays for the gate array and the sync hook. */
    CHECK(sizeof(GatedCoord) - sizeof(PlainCoord) <= sizeof(AdmT1IntervalAdm));
    /* The rule table lives in the type: no runtime copy, no per-submit scan. */
    CHECK_EQ(AdmT1IntervalAdm::kRuleCount, static_cast<size_t>(1U));
    CHECK_EQ(AdmT1IntervalAdm::kRules.rows[0].min_arrival.value,
             static_cast<uint64_t>(100U));
    std::printf("footprint: plain_coord=%zu gated_coord=%zu policy=%zu "
                "gate=%zu (kMaxAo=%u)\n",
                sizeof(PlainCoord), sizeof(GatedCoord),
                sizeof(AdmT1IntervalAdm), sizeof(coact::ArrivalGate),
                static_cast<unsigned>(AdvCfg::kMaxAo));
}

/* =====================================================================
 * The rejection ring is opt-in storage, not part of the "disabled costs
 * nothing" claim. A Config that does not declare kAdmissionSnapshotSlots
 * gets an empty ring: no array, no counter, and record() compiles away.
 * ===================================================================== */
COACT_TEST(admission_adv_snapshot_ring_is_opt_in)
{
    using DefaultMonitor = coact::Monitor<coact::DefaultConfig>;
    static_assert(DefaultMonitor::kAdmissionSnapshotSlots == 0U,
                  "the default Config must not carry snapshot storage");
    static_assert(AdvCfg::kAdmissionSnapshotSlots == 8U,
                  "this suite opts into eight slots");
    static_assert(std::is_empty<coact::AdmissionSnapshotRing<0U>>::value,
                  "a disabled ring must be empty");
    static_assert(sizeof(coact::AdmissionSnapshot::seq) == sizeof(uint64_t),
                  "the sequence must not wrap at 2^32 records");
    static_assert(
        std::is_same<decltype(std::declval<const DefaultMonitor&>()
                                  .admission_snapshot_count()),
                     uint64_t>::value,
        "the monotonic count must be 64-bit");

    /* Cost is explicit and belongs to the Config that asked for it. */
    CHECK_EQ(sizeof(coact::AdmissionSnapshotRing<0U>), static_cast<size_t>(1U));
    CHECK(sizeof(coact::AdmissionSnapshotRing<8U>)
          >= static_cast<size_t>(8U) * sizeof(coact::AdmissionSnapshot));
    std::printf("snapshot storage: disabled_ring=%zu enabled_ring(8)=%zu "
                "record=%zu monitor(default,off)=%zu monitor(adv,on)=%zu\n",
                sizeof(coact::AdmissionSnapshotRing<0U>),
                sizeof(coact::AdmissionSnapshotRing<8U>),
                sizeof(coact::AdmissionSnapshot), sizeof(DefaultMonitor),
                sizeof(coact::Monitor<AdvCfg>));

    DefaultMonitor off;
    off.record_admission_snapshot(coact::TargetId(1U),
                                 coact::SubmitDisposition::DroppedRateLimit,
                                 coact::kArrivalTooSoon, 1000U, 500U);
    CHECK_EQ(off.admission_snapshot_count(), static_cast<uint32_t>(0U));
    const auto disabled = off.admission_snapshot(1U);
    CHECK(!disabled);
    CHECK_EQ(static_cast<int>(disabled.error()),
             static_cast<int>(coact::SnapshotError::kDisabled));

    /* The same call on a Config that opted in records and reads back. */
    coact::Monitor<AdvCfg> on;
    on.record_admission_snapshot(coact::TargetId(2U),
                                coact::SubmitDisposition::RejectedFull,
                                coact::kBacklogFull, 4000U, 900U);
    CHECK_EQ(on.admission_snapshot_count(), static_cast<uint32_t>(1U));
    const auto got = on.admission_snapshot(1U);
    REQUIRE(static_cast<bool>(got));
    CHECK_EQ(got.value().target, static_cast<uint8_t>(2U));
    CHECK_EQ(got.value().reason, static_cast<uint16_t>(coact::kBacklogFull));
    CHECK_EQ(got.value().now_ns, static_cast<uint64_t>(4000U));
    CHECK_EQ(got.value().budget_ns, static_cast<uint64_t>(900U));
}

/* =====================================================================
 * A snapshot read is consistent or it fails. The writer derives every field
 * from the record's own seq, so a read that mixed two records breaks the
 * relation; the seq check before and after the fields must turn that into
 * SnapshotError::kTorn instead of handing back a plausible-looking mix.
 * ===================================================================== */
COACT_TEST(admission_adv_snapshot_read_is_consistent_or_torn)
{
    coact::Monitor<AdvCfg> monitor;
    std::atomic<bool> done{false};
    std::atomic<uint64_t> consistent{0U};
    std::atomic<uint64_t> torn{0U};
    std::atomic<uint64_t> mixed{0U};
    static constexpr uint32_t kRecords = 40000U;

    std::thread writer([&monitor, &done]() {
        for (uint32_t i = 1U; i <= kRecords; ++i) {
            monitor.record_admission_snapshot(
                coact::TargetId(static_cast<uint8_t>(1U + (i % 2U))),
                coact::SubmitDisposition::DroppedRateLimit,
                static_cast<uint16_t>(4U + (i % 3U)),
                static_cast<uint64_t>(i) * 1000U,
                static_cast<uint64_t>(i) * 10U);
        }
        done.store(true, std::memory_order_release);
    });

    std::thread reader([&monitor, &done, &consistent, &torn, &mixed]() {
        while (!done.load(std::memory_order_acquire)) {
            const uint32_t total = monitor.admission_snapshot_count();
            if (0U == total) {
                continue;
            }
            const auto got = monitor.admission_snapshot(total);
            if (!got) {
                if (coact::SnapshotError::kTorn == got.error()) {
                    torn.fetch_add(1U, std::memory_order_relaxed);
                }
                continue;
            }
            const coact::AdmissionSnapshot& s = got.value();
            consistent.fetch_add(1U, std::memory_order_relaxed);
            const uint64_t seq = s.seq;
            if ((s.now_ns != (seq * 1000U)) || (s.budget_ns != (seq * 10U))
                || (s.target != static_cast<uint8_t>(1U + (seq % 2U)))
                || (s.reason != static_cast<uint16_t>(4U + (seq % 3U)))) {
                mixed.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    });

    writer.join();
    reader.join();

    CHECK(consistent.load(std::memory_order_relaxed) > 0U);
    CHECK_EQ(mixed.load(std::memory_order_relaxed), static_cast<uint64_t>(0U));
    std::printf("snapshot race: consistent=%llu torn=%llu mixed=%llu\n",
                static_cast<unsigned long long>(consistent.load()),
                static_cast<unsigned long long>(torn.load()),
                static_cast<unsigned long long>(mixed.load()));
}

/* Compile-time rule set for admission_adv_runtime_forwards_admission_policy.
 * One second minimum interval: two back-to-back submits cannot both be
 * admitted, and the test does not depend on host scheduling. */
ADM_CASE(AdmRuntime, AdvStage, VirtualClock, coact::Breaker<AdvCfg>,
         AdvCfg, ADM_RULE(1U, 1000000000ULL, 0U));

/* =====================================================================
 * Runtime must carry the policy. A board that declares rules should not
 * have to hand-assemble a Coordinator to get them, and the gate it binds
 * must have a real critical section rather than a null hook.
 * ===================================================================== */
COACT_TEST(admission_adv_runtime_forwards_admission_policy)
{
    using GatedAdm =
        coact::StaticAdmission<AdmRuntimeRules, AdvCfg, coact::AdmissionSpinSync>;
    using GatedRuntime = coact::Runtime<AdvCfg, coact::pal::Posix,
                                       coact::HostSmpProfile, GatedAdm>;
    using PlainRuntime = coact::Runtime<AdvCfg, coact::pal::Posix>;

    static_assert(std::is_same<PlainRuntime::AdmissionType,
                              coact::NoAdmission>::value,
                  "the default Runtime keeps admission compiled out");
    static_assert(std::is_same<GatedRuntime::AdmissionType, GatedAdm>::value,
                  "Runtime must forward the policy to its Coordinator");
    static_assert(std::is_same<decltype(std::declval<GatedRuntime&>()
                                            .admission_policy()),
                              const GatedAdm&>::value,
                  "Runtime must expose the policy it forwarded");

    StageAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    const coact::Event ie = init_event();
    ao.init(ie);

    AdvPool pool;
    REQUIRE(pool.init(g_pool_storage, sizeof(g_pool_storage)));

    coact::pal::Posix pal;
    GatedRuntime rt(pal);
    REQUIRE(rt.bind(&ao));
    REQUIRE(rt.initialize());
    rt.start();

    CHECK_EQ(rt.admission_policy()
                 .limits_for(coact::TargetId(1U))
                 .min_arrival.value,
             static_cast<uint64_t>(1000000000ULL));
    /* Bound, not defaulted: a null save hook would serialize nothing. */
    CHECK(rt.admission_policy().critical_section().save != nullptr);
    CHECK(rt.admission_policy().critical_section().restore != nullptr);

    coact::EventQos qos{false, false};
    coact::Event* first = pool.alloc(1U);
    coact::Event* second = pool.alloc(1U);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    const coact::SubmitResult r1 =
        rt.coordinator().submit_from_task(coact::TargetId(1U), first, qos);
    const coact::SubmitResult r2 =
        rt.coordinator().submit_from_task(coact::TargetId(1U), second, qos);

    CHECK(static_cast<int>(coact::SubmitDisposition::DroppedRateLimit)
          != static_cast<int>(r1.disposition));
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::DroppedRateLimit),
             static_cast<int>(r2.disposition));
    CHECK_EQ(static_cast<uint16_t>(coact::kArrivalTooSoon), r2.reason);
    /* Neither submission left a reservation in flight. */
    CHECK_EQ(rt.admission_policy().holds(coact::TargetId(1U)),
             static_cast<uint16_t>(0U));

    rt.stop();
    CHECK_EQ(pool.used(), static_cast<uint16_t>(0U));
}

}  // namespace

COACT_TEST_MAIN()
