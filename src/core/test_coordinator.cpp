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

/* Mock PAL that counts Dispatcher wakeup signals instead of waking a thread.
   in_thread drives the dispatcher-context answer: the coordinator must never
   take the direct path from the Dispatcher thread (nested RTC guard). */
struct CountingPal {
    int signals = 0;
    bool in_thread = false;
    uint64_t monotonic_ns() const noexcept { return 0ULL; }
    coact::ExecutionContext current_context() const noexcept
    {
        return {coact::ContextKind::Task, 10U, 0U, true};
    }
    void signal_dispatcher_from_task() noexcept { ++signals; }
    void signal_dispatcher_from_isr() noexcept { ++signals; }
    void enter_direct() noexcept {}
    void leave_direct() noexcept {}
    static bool in_dispatcher_thread() noexcept { return false; }
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

/* AO that always loses the direct race: dispatch_direct() returns false while
   the lease still reports Idle, so the coordinator measures the failed
   acquisition window and must record the contention elapsed time. */
struct ContentionAo final : coact::AoBase {
    ContentionAo() noexcept : coact::AoBase(1000ULL) {}

    void dispatch(const coact::Event&) noexcept override {}
    bool try_dispatch_queued(const coact::Event&) noexcept override { return false; }
    bool dispatch_direct(const coact::Event&) noexcept override { return false; }
    coact::LogicalPrio logical_prio() const noexcept override { return 13U; }
    coact::PriorityClass priority_class() const noexcept override
    {
        return coact::PriorityClass::Normal;
    }
    bool direct_eligible() const noexcept override { return true; }
    bool isr_direct_safe() const noexcept override { return false; }
    coact::ExecutionLease& lease() noexcept override { return lease_; }
    coact::PendingCounter& pending() noexcept override { return pending_; }

    coact::ExecutionLease lease_;
    coact::PendingCounter pending_;
};

struct OverBudgetPal {
    int signals = 0;
    uint64_t now = 0;
    uint64_t monotonic_ns() noexcept
    {
        const uint64_t v = now;
        now += 2000ULL;  // each sample advances past the 1000ns budget
        return v;
    }
    coact::ExecutionContext current_context() const noexcept
    {
        return {coact::ContextKind::Task, 10U, 0U, true};
    }
    void signal_dispatcher_from_task() noexcept { ++signals; }
    void signal_dispatcher_from_isr() noexcept { ++signals; }
    void enter_direct() noexcept {}
    void leave_direct() noexcept {}
    static bool in_dispatcher_thread() noexcept { return false; }
};

struct MaxDirectDepthPal {
    int signals = 0;
    uint64_t monotonic_ns() const noexcept { return 0ULL; }
    coact::ExecutionContext current_context() const noexcept
    {
        return {coact::ContextKind::Task, 10U, SmallCfg::kMaxDirectDepth, true};
    }
    void signal_dispatcher_from_task() noexcept { ++signals; }
    void signal_dispatcher_from_isr() noexcept { ++signals; }
    void enter_direct() noexcept {}
    void leave_direct() noexcept {}
    static bool in_dispatcher_thread() noexcept { return false; }
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

COACT_TEST(coordinator_direct_contention_failure_records_elapsed)
{
    ContentionAo ao;

    StageT staging = make_staging();
    coact::AoRegistry<SmallCfg> registry;
    coact::Monitor<SmallCfg> monitor;
    SmallCfg cfg{};
    coact::Breaker<SmallCfg> breaker(cfg);
    OverBudgetPal pal;
    coact::DispatchCoordinator<StageT, OverBudgetPal,
                               coact::Breaker<SmallCfg>> coord(
        staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind(&ao, ao.logical_prio()));

    coact::Event e{};
    e.signal = 1U; e.pool_id = 0U; e.ref_ctr = 0U;
    const coact::EventQos qos{false, false};
    const coact::SubmitResult r =
        coord.submit_from_task(coact::TargetId(1U), &e, qos);

    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(r.disposition));
    CHECK_EQ(coact_test::relaxed(
                 monitor.ao(coact::TargetId(1U)).lease_contention), 1U);
    CHECK_EQ(coact_test::relaxed(
                 monitor.ao(coact::TargetId(1U)).lease_contention_duration_ns),
             2000ULL);
}

COACT_TEST(coordinator_direct_depth_limit_forces_staging)
{
    coact::Event init_e{};
    DirectAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    ao.init(init_e);

    StageT staging = make_staging();
    coact::AoRegistry<SmallCfg> registry;
    coact::Monitor<SmallCfg> monitor;
    SmallCfg cfg{};
    coact::Breaker<SmallCfg> breaker(cfg);
    MaxDirectDepthPal pal;
    coact::DispatchCoordinator<StageT, MaxDirectDepthPal,
                               coact::Breaker<SmallCfg>> coord(
        staging, registry, monitor, breaker, pal);
    CHECK(registry.bind(&ao, ao.logical_prio()));

    staging.arm_dispatcher_wait();
    coact::Event e{};
    e.signal = 1U;
    const coact::SubmitResult result =
        coord.submit_from_task(coact::TargetId(1U), &e,
                               coact::EventQos{false, false});
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(result.disposition));
    CHECK_EQ(1, pal.signals);
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
 * Nested-dispatch guard (RTC layer-1 hole): a submit issued while running on
 * the Dispatcher thread (i.e. from inside an AO handler) must never take the
 * direct path - it would run the target handler inline on the same stack,
 * bypassing serialization. The coordinator consults PalT::in_dispatcher_thread.
 * ========================================================================= */
struct DispatcherContextPal {
    int signals = 0;
    uint64_t monotonic_ns() const noexcept { return 0ULL; }
    coact::ExecutionContext current_context() const noexcept
    {
        return {coact::ContextKind::Dispatcher, 0U, 0U, false};
    }
    void signal_dispatcher_from_task() noexcept { ++signals; }
    void signal_dispatcher_from_isr() noexcept { ++signals; }
    void enter_direct() noexcept {}
    void leave_direct() noexcept {}
    static bool in_dispatcher_thread() noexcept { return true; }
};

COACT_TEST(coordinator_dispatcher_context_forces_staging)
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
    DispatcherContextPal pal;
    coact::DispatchCoordinator<StageT, DispatcherContextPal,
                               coact::Breaker<SmallCfg>> coord(
        staging, registry, monitor, breaker, pal);
    CHECK(registry.bind(&ao, ao.logical_prio()));

    /* A direct-eligible AO with an Idle lease: only the dispatcher-context
       guard must keep this out of dispatch_direct. */
    staging.arm_dispatcher_wait();
    coact::Event e{};
    e.signal = 1U; e.pool_id = 0U; e.ref_ctr = 0U;
    const coact::EventQos qos{false, false};
    const coact::SubmitResult r =
        coord.submit_from_task(coact::TargetId(1U), &e, qos);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(r.disposition));
    CHECK_EQ(1, pal.signals);
}

COACT_TEST(coordinator_task_context_keeps_direct)
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
    CountingPal pal;
    coact::DispatchCoordinator<StageT, CountingPal,
                               coact::Breaker<SmallCfg>> coord(
        staging, registry, monitor, breaker, pal);
    CHECK(registry.bind(&ao, ao.logical_prio()));

    coact::Event e{};
    e.signal = 1U; e.pool_id = 0U; e.ref_ctr = 0U;
    const coact::EventQos qos{false, false};
    const coact::SubmitResult r =
        coord.submit_from_task(coact::TargetId(1U), &e, qos);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Direct),
             static_cast<int>(r.disposition));
}

COACT_TEST(coordinator_submit_queued_from_task_bypasses_direct)
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
    CountingPal pal;
    coact::DispatchCoordinator<StageT, CountingPal,
                               coact::Breaker<SmallCfg>> coord(
        staging, registry, monitor, breaker, pal);
    CHECK(registry.bind(&ao, ao.logical_prio()));

    staging.arm_dispatcher_wait();
    coact::Event e{};
    e.signal = 1U; e.pool_id = 0U; e.ref_ctr = 0U;
    const coact::EventQos qos{false, false};
    const coact::SubmitResult r =
        coord.submit_queued_from_task(coact::TargetId(1U), &e, qos);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(r.disposition));
    CHECK_EQ(1, pal.signals);
}

// ---------------------------------------------------------------------------
// TraceOps coordinator instrumentation (design trace §3.2)
// ---------------------------------------------------------------------------

struct CapturedTrace {
    uint16_t last_source{0};
    uint32_t last_target_raw{0};
    uint16_t last_signal{0};
    uint8_t last_disposition{0};
    uint32_t last_reason{0};
    uint64_t last_elapsed{0};
    uint8_t last_path{0};
    uint8_t last_timeout{0};
    uint8_t last_kind{0};
    bool last_from_isr{false};
};

static void capture_submit(void* ctx, uint16_t source_id, coact::TargetId target,
                           uint16_t signal, uint8_t disposition,
                           uint32_t reason, bool from_isr) noexcept
{
    CapturedTrace* cap = static_cast<CapturedTrace*>(ctx);
    cap->last_source = source_id;
    cap->last_target_raw = target.raw();
    cap->last_signal = signal;
    cap->last_disposition = disposition;
    cap->last_reason = reason;
    cap->last_from_isr = from_isr;
}

static void capture_dispatch(void* ctx, coact::TargetId target,
                             uint64_t elapsed_ns, uint8_t path,
                             uint8_t timeout) noexcept
{
    CapturedTrace* cap = static_cast<CapturedTrace*>(ctx);
    cap->last_target_raw = target.raw();
    cap->last_elapsed = elapsed_ns;
    cap->last_path = path;
    cap->last_timeout = timeout;
}

static void capture_lease(void* ctx, coact::TargetId target, uint8_t kind,
                          uint64_t elapsed_ns) noexcept
{
    CapturedTrace* cap = static_cast<CapturedTrace*>(ctx);
    cap->last_target_raw = target.raw();
    cap->last_kind = kind;
    cap->last_elapsed = elapsed_ns;
}

COACT_TEST(coordinator_trace_submit_records_disposition)
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
    CountingPal pal;
    coact::DispatchCoordinator<StageT, CountingPal,
                               coact::Breaker<SmallCfg>> coord(
        staging, registry, monitor, breaker, pal);
    CHECK(registry.bind(&ao, ao.logical_prio()));

    CapturedTrace captured{};
    coact::TraceOps ops{};
    ops.on_submit = &capture_submit;
    ops.ctx = &captured;
    monitor.bind_trace(ops);

    coact::Event e{};
    e.signal = 7U; e.pool_id = 0U; e.ref_ctr = 0U;
    const coact::EventQos qos{false, false};
    const coact::SubmitResult r =
        coord.submit_from_task(coact::TargetId(1U), &e, qos);

    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Direct),
             static_cast<int>(r.disposition));
    CHECK_EQ(captured.last_target_raw, 1U);
    CHECK_EQ(captured.last_signal, 7U);
    CHECK_EQ(captured.last_disposition,
             static_cast<uint8_t>(coact::SubmitDisposition::Direct));
    CHECK_EQ(captured.last_reason, 0U);
}

COACT_TEST(coordinator_trace_lease_contention_records_elapsed)
{
    ContentionAo ao;

    StageT staging = make_staging();
    coact::AoRegistry<SmallCfg> registry;
    coact::Monitor<SmallCfg> monitor;
    SmallCfg cfg{};
    coact::Breaker<SmallCfg> breaker(cfg);
    OverBudgetPal pal;
    coact::DispatchCoordinator<StageT, OverBudgetPal,
                               coact::Breaker<SmallCfg>> coord(
        staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind(&ao, ao.logical_prio()));

    CapturedTrace captured{};
    coact::TraceOps ops{};
    ops.on_lease_contention = &capture_lease;
    ops.ctx = &captured;
    monitor.bind_trace(ops);

    coact::Event e{};
    e.signal = 1U; e.pool_id = 0U; e.ref_ctr = 0U;
    const coact::EventQos qos{false, false};
    const coact::SubmitResult r =
        coord.submit_from_task(coact::TargetId(1U), &e, qos);

    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(r.disposition));
    CHECK_EQ(captured.last_target_raw, 1U);
    CHECK_EQ(captured.last_kind, 1U);
    CHECK_EQ(captured.last_elapsed, 2000ULL);
}

COACT_TEST(coordinator_trace_direct_success_records_dispatch)
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

    CapturedTrace captured{};
    coact::TraceOps ops{};
    ops.on_submit = &capture_submit;
    ops.on_dispatch = &capture_dispatch;
    ops.ctx = &captured;
    monitor.bind_trace(ops);

    coact::Event e{};
    e.signal = 7U; e.pool_id = 0U; e.ref_ctr = 0U;
    const coact::EventQos qos{false, false};
    const coact::SubmitResult r =
        coord.submit_from_task(coact::TargetId(1U), &e, qos);

    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Direct),
             static_cast<int>(r.disposition));
    CHECK_EQ(captured.last_path, 0U);
    CHECK_EQ(captured.last_target_raw, 1U);
    /* OverBudgetPal advances 2000ns per sample, past the 1000ns RTC budget:
       the direct dispatch must report timeout=1. */
    CHECK_EQ(captured.last_timeout, 1U);
    CHECK(coact_test::relaxed(
              monitor.ao(coact::TargetId(1U)).direct_duration_ns) > 0U);
}

COACT_TEST(coordinator_trace_submit_from_isr_carries_isr_flag)
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
    OverBudgetPal pal;
    coact::DispatchCoordinator<StageT, OverBudgetPal,
                               coact::Breaker<SmallCfg>> coord(
        staging, registry, monitor, breaker, pal);
    CHECK(registry.bind(&ao, ao.logical_prio()));

    CapturedTrace captured{};
    coact::TraceOps ops{};
    ops.on_submit = &capture_submit;
    ops.ctx = &captured;
    monitor.bind_trace(ops);

    staging.arm_dispatcher_wait();
    coact::Event e{};
    e.signal = 7U; e.pool_id = 0U; e.ref_ctr = 0U;
    const coact::EventQos qos{false, false};
    const coact::SubmitResult r =
        coord.try_submit_from_isr(coact::TargetId(1U), &e, qos);

    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(r.disposition));
    CHECK_EQ(captured.last_signal, 7U);
    CHECK_EQ(captured.last_disposition,
             static_cast<uint8_t>(coact::SubmitDisposition::Queued));
    CHECK(captured.last_from_isr == true);
}

}  // namespace

COACT_TEST_MAIN()
