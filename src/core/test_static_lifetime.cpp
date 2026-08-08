// coact static-lifetime Runtime test (memory-review P1): a global Runtime must
// live in .bss, NOT on an RT-Thread task stack (~9 KiB default). This test
// proves a static RtThread + static Runtime<BoardCfg,...> + static Ao links and
// runs end-to-end under the pthread-backed RT-Thread stub, and that
// Config::kDispatcherStackBytes reaches the PAL. Also documents the memory
// footprint split (P2: Staging dominates).
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <atomic>
#include <cstdio>
#include <unistd.h>

#define COACT_RTT_STUB
#include "test/rtthread_stub.h"

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/coordinator.hpp"
#include "coact/dispatcher.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/monitor.hpp"
#include "coact/pal_rtthread.hpp"
#include "coact/pool.hpp"
#include "coact/queue.hpp"
#include "coact/runtime.hpp"
#include "coact/staging.hpp"

/* ---------------------------------------------------------------------------
 * Board Config: small capacities plus a non-default Dispatcher stack (8192) so
 * Config -> Runtime -> PAL pass-through is provable.
 * ------------------------------------------------------------------------- */
struct BoardCfg {
    enum : uint8_t {
        kMaxAo = 2U,
        kMaxStateDepth = 6U,
        kMaxDirectDepth = 4U,
        kBatchSizeMax = 4U
    };
    enum : uint16_t {
        kHighCapacity = 8U,
        kNormalCapacity = 16U,
        kLowCapacity = 32U,
        kCooldownCycles = 5U
    };
    enum : uint32_t {
        kBatchTimeoutMs = 1U,
        kLowMaxWaitMs = 10U,
        kDispatcherStackBytes = 8192U
    };
    enum : uint64_t {
        kDirectBudgetNs = 50000ULL,
        kRtcBudgetNs = 1000000ULL
    };
};

/* Static fixture: the AO, PAL, pool and Runtime are globals (the .bss
   deployment pattern recommended for RT-Thread; never a task-stack auto). */
static std::atomic<int> g_counter{0};

struct StaticCtx {};
static void st_noop_entry(StaticCtx&) {}
static void st_noop_exit(StaticCtx&)  {}
static void st_count(StaticCtx&, const coact::Event&)
{
    g_counter.fetch_add(1, std::memory_order_relaxed);
}
static bool st_ok(const StaticCtx&, const coact::Event&) { return true; }

static const coact::StateDef<StaticCtx> kStStates[] = {
    { -1, nullptr, nullptr },
    {  0, st_noop_entry, st_noop_exit },
};
static const coact::TransitionDef<StaticCtx> kStTrans[] = {
    { 1, 1U, 1, coact::TransitionKind::Internal, st_ok, st_count },
};
struct StaticTraits {
    static coact::LogicalPrio   logical_prio()   { return 20U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
using StaticHsm = coact::Hsm<StaticCtx>;
using StaticAo  = coact::Ao<StaticCtx, StaticHsm, StaticTraits>;

static constexpr uint16_t kBlk = 16U;
static constexpr uint16_t kCap = 32U;
alignas(16) static unsigned char g_pool_storage[kBlk * (kCap + 1U)];

static StaticAo g_ao(kStStates, 2U, kStTrans, 1U, 1, 4U);
static coact::pal::RtThread g_pal;
static coact::Runtime<BoardCfg, coact::pal::RtThread> g_rt(g_pal);
static coact::EventPool<kBlk, kCap, coact::PoolMutexLock> g_pool;

namespace {

/* =========================================================================
 * End-to-end through the static Runtime, plus Config->PAL stack pass-through.
 * ========================================================================= */
COACT_TEST(static_runtime_end_to_end)
{
    g_counter.store(0);

    coact::Event init_e;
    init_e.signal = 0U; init_e.pool_id = 0U; init_e.ref_ctr = 0U;
    g_ao.init(init_e);
    g_pool.init(g_pool_storage, sizeof(g_pool_storage));

    CHECK(g_rt.bind(&g_ao));
    CHECK(g_rt.initialize());
    g_rt.start();

    /* Config pass-through (review P4): Runtime pushed BoardCfg's Dispatcher
       stack size into the PAL before starting the thread. */
    CHECK_EQ(static_cast<uint32_t>(BoardCfg::kDispatcherStackBytes),
             g_pal.dispatcher_stack_bytes());

    coact::EventQos qos{false, false};
    /* <= Normal capacity (16): a larger burst would be RejectedFull. */
    static constexpr int kN = 12;
    for (int i = 0; i < kN; ++i) {
        coact::Event* e = g_pool.alloc(1U);
        REQUIRE(e != nullptr);
        g_rt.coordinator().submit_from_task(1U, e, qos);
    }
    for (int w = 0; w < 200; ++w) {
        if (g_counter.load() >= kN) { break; }
        usleep(5000);
    }
    g_rt.stop();

    CHECK_EQ(kN, g_counter.load());
    CHECK_EQ(0U, g_pool.used());
}

/* =========================================================================
 * Memory footprint documentation (review P2: Staging dominates the Runtime).
 * ========================================================================= */
COACT_TEST(static_runtime_memory_footprint)
{
    using RuntimeT = coact::Runtime<BoardCfg, coact::pal::RtThread>;
    using StageT   = coact::Staging<BoardCfg, coact::pal::RtThread::QueueBackend>;

    std::printf("sizeof(Runtime)    = %zu\n", sizeof(RuntimeT));
    std::printf("sizeof(Staging)    = %zu\n", sizeof(StageT));
    std::printf("sizeof(Monitor)    = %zu\n",
                sizeof(coact::Monitor<BoardCfg>));
    std::printf("sizeof(AoRegistry) = %zu\n",
                sizeof(coact::AoRegistry<BoardCfg>));

    /* Staging must dominate: three fixed bounded queues vs. small counter
       arrays. Board Configs reduce memory by lowering k*Capacity here. */
    CHECK(sizeof(StageT) > sizeof(coact::Monitor<BoardCfg>));
    CHECK(sizeof(StageT) > sizeof(coact::AoRegistry<BoardCfg>));
}

}  // namespace

COACT_TEST_MAIN()
