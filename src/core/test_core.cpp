// coact core module unit tests: Dispatcher, DispatchCoordinator, Runtime.
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <atomic>
#include <cstring>

#include "coact/ao.hpp"
#include "coact/assert.hpp"
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

/* =========================================================================
 * Shared HSM fixtures (file scope so all tests share them).
 * ========================================================================= */
struct Ctx { int dispatch_count; };

static void s0_entry(Ctx&) {}
static void s0_exit(Ctx&)  {}
static void s1_entry(Ctx&) {}
static void s1_exit(Ctx&)  {}
static void inc_action(Ctx& c, const coact::Event&) { ++c.dispatch_count; }
static bool always_true(const Ctx&, const coact::Event&) { return true; }

static const coact::StateDef<Ctx> kStates[] = {
    /* 0 root */ { -1, nullptr,   nullptr  },
    /* 1 S0   */ {  0, s0_entry,  s0_exit  },
    /* 2 S1   */ {  0, s1_entry,  s1_exit  },
};
static const coact::TransitionDef<Ctx> kTrans[] = {
    { 1, 1U, 2, coact::TransitionKind::External, always_true, inc_action },
    { 2, 2U, 1, coact::TransitionKind::External, always_true, inc_action },
};

/* Traits: direct_eligible = true (for direct-dispatch test). */
struct TraitsDirect {
    static coact::LogicalPrio   logical_prio()   { return 10U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return true; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

/* Traits: direct_eligible = false (for staging / breaker tests). */
struct TraitsStage {
    static coact::LogicalPrio   logical_prio()   { return 11U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using MyHsm   = coact::Hsm<Ctx>;
using AoDirect = coact::Ao<Ctx, MyHsm, TraitsDirect>;
using AoStage  = coact::Ao<Ctx, MyHsm, TraitsStage>;

static coact::Event static_evt(uint16_t sig)
{
    coact::Event e;
    e.signal  = sig;
    e.pool_id = 0U;
    e.ref_ctr = 0U;
    return e;
}

/* Pool storage for tests that need dynamic events. */
static constexpr uint16_t kBlk = 8U;
static constexpr uint16_t kCap = 32U;
using TestPool = coact::EventPool<kBlk, kCap>;
alignas(8) static unsigned char g_pool_storage[kBlk * (kCap + 1U)];

/* Convenience: build a Staging + no-op CriticalSection. */
using StageT = coact::Staging<coact::DefaultConfig, coact::pal::Posix::QueueBackend>;
static StageT make_staging()
{
    return StageT(coact::CriticalSection{
        []() -> coact::CriticalSection::Token { return 0U; },
        [](coact::CriticalSection::Token) {} });
}

namespace {

/* =========================================================================
 * coordinator_unknown_target_rejected
 * ========================================================================= */
COACT_TEST(coordinator_unknown_target_rejected)
{
    coact::pal::Posix pal;
    StageT staging = make_staging();
    coact::AoRegistry  registry;
    coact::Monitor     monitor;
    coact::DefaultConfig cfg;
    coact::Breaker<>     breaker(cfg);
    coact::DispatchCoordinator<StageT, coact::pal::Posix> coord(
        staging, registry, monitor, breaker, pal);

    coact::Event e = static_evt(1U);
    coact::EventQos qos{false, false};
    coact::SubmitResult r = coord.submit_from_task(1U, &e, qos);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::RejectedState),
             static_cast<int>(r.disposition));
}

/* =========================================================================
 * coordinator_normal_submit_queued  (staging path, direct_eligible=false)
 * ========================================================================= */
COACT_TEST(coordinator_normal_submit_queued)
{
    coact::pal::Posix pal;
    StageT staging = make_staging();
    coact::AoRegistry  registry;
    coact::Monitor     monitor;
    coact::DefaultConfig cfg;
    coact::Breaker<>     breaker(cfg);
    coact::DispatchCoordinator<StageT, coact::pal::Posix> coord(
        staging, registry, monitor, breaker, pal);

    AoStage ao(kStates, 3U, kTrans, 2U, 1, 4U);
    coact::Event init_e = static_evt(0U);
    ao.init(init_e);
    CHECK(registry.bind(&ao, ao.logical_prio()));

    TestPool pool;
    pool.init(g_pool_storage, sizeof(g_pool_storage));

    coact::Event* e = pool.alloc(1U);
    REQUIRE(e != nullptr);
    CHECK_EQ(0U, e->ref_ctr);

    coact::EventQos qos{false, false};
    coact::SubmitResult r = coord.submit_from_task(1U, e, qos);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(r.disposition));
    CHECK_EQ(1U, static_cast<unsigned>(e->ref_ctr));
}

/* =========================================================================
 * coordinator_direct_dispatch  (direct_eligible=true, AO starts Idle)
 * ========================================================================= */
COACT_TEST(coordinator_direct_dispatch)
{
    coact::pal::Posix pal;
    StageT staging = make_staging();
    coact::AoRegistry  registry;
    coact::Monitor     monitor;
    coact::DefaultConfig cfg;
    coact::Breaker<>     breaker(cfg);
    coact::DispatchCoordinator<StageT, coact::pal::Posix> coord(
        staging, registry, monitor, breaker, pal);

    AoDirect ao(kStates, 3U, kTrans, 2U, 1, 4U);
    coact::Event init_e = static_evt(0U);
    ao.init(init_e);
    CHECK(registry.bind(&ao, ao.logical_prio()));

    coact::Event e = static_evt(1U);
    coact::EventQos qos{false, false};
    coact::SubmitResult r = coord.submit_from_task(1U, &e, qos);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Direct),
             static_cast<int>(r.disposition));
    CHECK_EQ(static_cast<int>(coact::AoRunState::Idle),
             static_cast<int>(ao.lease().state()));
}

/* =========================================================================
 * dispatcher_stop_exits_run
 * ========================================================================= */
COACT_TEST(dispatcher_stop_exits_run)
{
    coact::pal::Posix pal;
    StageT staging = make_staging();
    coact::AoRegistry  registry;
    coact::Monitor     monitor;
    coact::DefaultConfig cfg;
    coact::Breaker<>     breaker(cfg);
    coact::Dispatcher<StageT, coact::pal::Posix> dispatcher(
        staging, registry, monitor, breaker, pal);

    struct Wrap { coact::Dispatcher<StageT, coact::pal::Posix>* d; };
    Wrap w{&dispatcher};
    pal.start_dispatcher([](void* c) {
        static_cast<Wrap*>(c)->d->run();
    }, &w);

    dispatcher.request_stop();
    pal.join_dispatcher();
    CHECK(true);
}

/* =========================================================================
 * runtime_lifecycle
 * ========================================================================= */
COACT_TEST(runtime_lifecycle)
{
    coact::pal::Posix pal;
    coact::Runtime<coact::DefaultConfig, coact::pal::Posix> rt(pal);

    AoStage ao(kStates, 3U, kTrans, 2U, 1, 4U);
    coact::Event init_e = static_evt(0U);
    ao.init(init_e);

    CHECK(rt.bind(&ao));
    CHECK(rt.initialize());
    rt.start();
    rt.stop();
    CHECK(true);
}

/* =========================================================================
 * coordinator_breaker_l2_drops_noncritical
 * ========================================================================= */
COACT_TEST(coordinator_breaker_l2_drops_noncritical)
{
    coact::pal::Posix pal;
    StageT staging = make_staging();
    coact::AoRegistry  registry;
    coact::Monitor     monitor;
    coact::DefaultConfig cfg;
    coact::Breaker<>     breaker(cfg);
    breaker.on_overflow();   /* drive to L2 */
    coact::DispatchCoordinator<StageT, coact::pal::Posix> coord(
        staging, registry, monitor, breaker, pal);

    AoStage ao(kStates, 3U, kTrans, 2U, 1, 4U);
    coact::Event init_e = static_evt(0U);
    ao.init(init_e);
    CHECK(registry.bind(&ao, ao.logical_prio()));

    coact::Event e = static_evt(1U);
    coact::EventQos qos{false, false};
    coact::SubmitResult r = coord.submit_from_task(1U, &e, qos);
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::DroppedOverload),
             static_cast<int>(r.disposition));
}

}  // namespace

COACT_TEST_MAIN()
