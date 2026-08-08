// coact coordinator wakeup optimization test (flame finding): the Dispatcher
// must not be woken while it is actively draining (it will pick up newly
// enqueued events in the same batch). The Staging carries a dispatcher_active
// flag: producers signal only when the Dispatcher is idle, and the Dispatcher
// clears it just before sleeping after re-checking the queues. A mock PAL
// counts the signals.
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
 * A submit while the Dispatcher is actively draining must NOT wake it; a
 * submit while it is idle must wake it exactly once.
 * ========================================================================= */
COACT_TEST(coordinator_signals_only_when_dispatcher_idle)
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
    coact::DispatchCoordinator<StageT, CountingPal> coord(
        staging, registry, monitor, breaker, pal);
    CHECK(registry.bind(&ao, ao.logical_prio()));

    coact::Event e{};
    e.signal = 1U; e.pool_id = 0U; e.ref_ctr = 0U;
    const coact::EventQos qos{false, false};

    /* Dispatcher mid-batch (active): no wakeup. */
    staging.mark_dispatcher_active();
    coact::SubmitResult r1 = coord.submit_from_task(1U, &e, qos);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(r1.disposition));
    CHECK_EQ(0, pal.signals);

    /* Dispatcher idle: exactly one wakeup. */
    staging.mark_dispatcher_idle();
    coact::SubmitResult r2 = coord.submit_from_task(1U, &e, qos);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(r2.disposition));
    CHECK_EQ(1, pal.signals);
}

}  // namespace

COACT_TEST_MAIN()
