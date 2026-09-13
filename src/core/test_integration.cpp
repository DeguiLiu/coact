// coact end-to-end integration test: event pool -> submit -> Dispatcher ->
// AO action -> event gc. Multi-AO, multi-event.
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
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

namespace {

/* Global counters incremented by AO actions (no context access needed). */
static std::atomic<int> g_counter_a{0};
static std::atomic<int> g_counter_b{0};

struct IntCtx {};

static void noop_entry(IntCtx&) {}
static void noop_exit(IntCtx&)  {}
static void action_a(IntCtx&, const coact::Event&)
{
    g_counter_a.fetch_add(1, std::memory_order_relaxed);
}
static void action_b(IntCtx&, const coact::Event&)
{
    g_counter_b.fetch_add(1, std::memory_order_relaxed);
}
static bool always(const IntCtx&, const coact::Event&) { return true; }

/* Two separate state/transition tables so signal 1 goes to AO-A's action
   and signal 2 goes to AO-B's action. */
static const coact::StateDef<IntCtx> kStates[] = {
    /* 0: root */ { -1, nullptr, nullptr },
    /* 1: S0   */ {  0, noop_entry, noop_exit },
};
static const coact::TransitionDef<IntCtx> kTransA[] = {
    { 1, 1U, 1, coact::TransitionKind::Internal, always, action_a },
};
static const coact::TransitionDef<IntCtx> kTransB[] = {
    { 1, 2U, 1, coact::TransitionKind::Internal, always, action_b },
};

struct TraitsA {
    static coact::LogicalPrio   logical_prio()   { return 10U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
struct TraitsB {
    static coact::LogicalPrio   logical_prio()   { return 11U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
/* Low-priority, non-direct: every submit is staged in the Low partition, so
   the event can only be served after the Dispatcher wakes from its idle wait. */
struct TraitsLow {
    static coact::LogicalPrio   logical_prio()   { return 12U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Low; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using IntHsm = coact::Hsm<IntCtx>;
using AoA    = coact::Ao<IntCtx, IntHsm, TraitsA>;
using AoB    = coact::Ao<IntCtx, IntHsm, TraitsB>;
using AoLow  = coact::Ao<IntCtx, IntHsm, TraitsLow>;

/* Block size must match pool_block_align result (alignof(max_align_t)=16 on
   x86_64) so that kCAP blocks actually fit in the storage array. */
static constexpr uint16_t kBS  = 16U;
static constexpr uint16_t kCAP = 64U;
/* Thread-safe pool: the main thread allocates while the Dispatcher thread
   reclaims (event_gc) concurrently. */
using IntPool = coact::EventPool<kBS, kCAP>;
alignas(16) static unsigned char g_pool_storage[kBS * kCAP + kBS];  /* +1 block margin */

/* =========================================================================
 * Integration test: 2 AOs, 50 events each, verify all counted and gc'd.
 * ========================================================================= */
COACT_TEST(integration_two_ao_fifty_events_each)
{
    g_counter_a.store(0);
    g_counter_b.store(0);

    AoA ao_a(kStates, 2U, kTransA, 1U, 1, 4U);
    AoB ao_b(kStates, 2U, kTransB, 1U, 1, 4U);

    coact::Event init_e;
    init_e.signal  = 0U;
    init_e.pool_id = 0U;
    init_e.ref_ctr = 0U;
    ao_a.init(init_e);
    ao_b.init(init_e);

    IntPool pool;
    pool.init(g_pool_storage, sizeof(g_pool_storage));

    coact::pal::Posix pal;
    coact::Runtime<coact::DefaultConfig, coact::pal::Posix> rt(pal);

    CHECK(rt.bind(&ao_a));
    CHECK(rt.bind(&ao_b));
    CHECK(rt.initialize());
    rt.start();

    coact::EventQos qos{false, false};
    static constexpr int kN = 25;  /* 25 × 2 AOs = 50 events < kCAP=64 */
    for (int i = 0; i < kN; ++i) {
        coact::Event* ea = pool.alloc(1U);
        REQUIRE(ea != nullptr);
        rt.coordinator().submit_from_task(coact::TargetId(1U), ea, qos);

        coact::Event* eb = pool.alloc(2U);
        REQUIRE(eb != nullptr);
        rt.coordinator().submit_from_task(coact::TargetId(2U), eb, qos);
    }

    /* Wait up to 1 s for dispatcher to drain. */
    for (int w = 0; w < 200; ++w) {
        if (g_counter_a.load() >= kN && g_counter_b.load() >= kN) {
            break;
        }
        usleep(5000);
    }

    rt.stop();

    CHECK_EQ(kN, g_counter_a.load());
    CHECK_EQ(kN, g_counter_b.load());
    CHECK_EQ(0U, pool.used());  /* all events gc'd back */
}

/* =========================================================================
 * Idle-wait wakeup: a Low event submitted while the Dispatcher is parked in
 * its (now truly blocking) idle wait must still be served inside
 * kLowMaxWaitMs. Before the fix the idle path polled every kBatchTimeoutMs,
 * which masked a lost wake; after the fix the arm/any_ready latch protocol is
 * the only thing that can wake it, so this is the regression guard.
 * ========================================================================= */
COACT_TEST(dispatcher_idle_wakes_low_event_within_low_max_wait)
{
    g_counter_a.store(0);

    AoLow ao(kStates, 2U, kTransA, 1U, 1, 4U);
    coact::Event init_e;
    init_e.signal  = 0U;
    init_e.pool_id = 0U;
    init_e.ref_ctr = 0U;
    ao.init(init_e);

    IntPool pool;
    pool.init(g_pool_storage, sizeof(g_pool_storage));

    coact::pal::Posix pal;
    coact::Runtime<coact::DefaultConfig, coact::pal::Posix> rt(pal);
    CHECK(rt.bind(&ao));
    CHECK(rt.initialize());
    CHECK(rt.start());

    /* Park the Dispatcher in the idle branch with every queue empty. Under the
       fixed code that is an unbounded PAL wait; a lost wake would strand the
       event below for the whole ctest timeout. */
    usleep(50000);

    coact::Event* e = pool.alloc(1U);
    REQUIRE(e != nullptr);
    const auto t0 = std::chrono::steady_clock::now();
    const coact::SubmitResult r = rt.coordinator().submit_from_task(
        coact::TargetId(1U), e, coact::EventQos{false, false});
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(r.disposition));

    const auto budget =
        std::chrono::milliseconds(coact::DefaultConfig::kLowMaxWaitMs);
    const auto deadline = t0 + budget;
    while ((1 != g_counter_a.load(std::memory_order_acquire))
           && (std::chrono::steady_clock::now() < deadline)) {
        std::this_thread::yield();
    }
    const auto served_after = std::chrono::steady_clock::now() - t0;

    rt.stop();

    CHECK_EQ(1, g_counter_a.load());
    CHECK(served_after < budget);
    CHECK_EQ(0U, pool.used());
}

}  // namespace

COACT_TEST_MAIN()
