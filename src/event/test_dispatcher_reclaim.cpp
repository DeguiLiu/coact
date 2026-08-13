// coact Dispatcher exactly-once release tests (design §7.4): the Dispatcher is
// the final release-er of every queued event. The three paths that must release
// exactly once are:
//   1. normal dequeue -> dispatch -> gc
//   2. lookup failure (unbound/invalid target): the final reference is still
//      released, no leak
//   3. stop: whatever is still queued (plus the in-flight batch) is flushed and
//      each event released exactly once, leaving every pool used()==0 and every
//      AO pending()==0
//
// The Dispatcher runs on a real POSIX thread (pal::Posix), so the stop path
// exercises the real run() loop including the shutdown drain.
// SPDX-License-Identifier: MIT

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <unistd.h>

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/coordinator.hpp"
#include "coact/dispatcher.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/monitor.hpp"
#include "coact/pal.hpp"
#include "coact/pal_posix.hpp"
#include "coact/pool.hpp"
#include "coact/staging.hpp"

#include "test/test_harness.hpp"

namespace {

using Config = coact::DefaultConfig;
using StagingT = coact::Staging<Config, coact::pal::Posix::QueueBackend>;
using DispatcherT = coact::Dispatcher<StagingT, coact::pal::Posix>;

struct Ctx {
    std::atomic<int> count{0};
};

static void on_evt(Ctx& c, const coact::Event&) noexcept
{
    c.count.fetch_add(1, std::memory_order_relaxed);
}

static const coact::StateDef<Ctx> kStates[] = {
    {-1, nullptr, nullptr, nullptr},
    {0, nullptr, nullptr, nullptr},
};
static const coact::TransitionDef<Ctx> kTrans[] = {
    {1, 1U, 1, coact::TransitionKind::Internal, nullptr, on_evt},
};

struct Traits {
    static coact::LogicalPrio logical_prio() noexcept { return 30U; }
    static coact::PriorityClass priority_class() noexcept
    {
        return coact::PriorityClass::Normal;
    }
    static bool direct_eligible() noexcept { return false; }
    static bool isr_direct_safe() noexcept { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
using TestAo = coact::Ao<Ctx, coact::Hsm<Ctx>, Traits>;

// Trampoline so the POSIX dispatcher thread can call Dispatcher::run().
template <typename D>
static void trampoline(void* ctx) noexcept
{
    static_cast<D*>(ctx)->run();
}

// Shared HostSmpProfile pool: the producer (test thread) allocs while the
// Dispatcher thread reclaims, so it is bound to a real spin critical section
// (design §7.3).
constexpr std::uint16_t kPoolBlock = 16U;
constexpr std::uint16_t kPoolCap = 64U;
alignas(64) static std::uint8_t g_pool_storage[kPoolBlock * kPoolCap];
static coact::SpinCriticalSection g_spin;
static coact::EventPool<kPoolBlock, kPoolCap, coact::HostSmpProfile> g_pool;

// Minimal runtime assembly used by the Dispatcher-level tests: staging +
// registry + monitor + breaker + coordinator + Dispatcher over pal::Posix.
struct Fixture {
    coact::pal::Posix pal;
    Config cfg;
    StagingT staging;
    coact::AoRegistry<Config> registry;
    coact::Monitor<Config> monitor;
    coact::Breaker<Config> breaker;
    coact::DispatchCoordinator<StagingT, coact::pal::Posix> coordinator;
    DispatcherT dispatcher;

    Fixture() noexcept
        : pal(),
          cfg(),
          staging(coact::make_critical_section(pal)),
          registry(),
          monitor(),
          breaker(cfg),
          coordinator(staging, registry, monitor, breaker, pal),
          dispatcher(staging, registry, monitor, breaker, pal)
    {
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Path 1 (normal): dequeue -> dispatch -> gc, exactly once per event.
// ---------------------------------------------------------------------------
COACT_TEST(dispatcher_normal_dispatch_releases_exactly_once)
{
    Fixture fx;
    TestAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    coact::Event init_e{0U, 0U, 0U};
    ao.init(init_e);
    REQUIRE(fx.registry.bind(&ao, Traits::logical_prio()));
    const coact::TargetId tgt = fx.registry.target_of(&ao);
    REQUIRE(tgt != coact::kInvalidTarget);

    g_pool.init(g_pool_storage, sizeof(g_pool_storage),
                coact::make_spin_critical_section(g_spin));

    fx.pal.start_dispatcher(&trampoline<DispatcherT>, &fx.dispatcher);

    constexpr int kN = 20;   // 2.5 batches of kBatchSizeMax=8
    for (int i = 0; i < kN; ++i) {
        coact::Event* e = g_pool.alloc(1U);
        REQUIRE(e != nullptr);
        coact::EventQos qos{false, false};
        coact::SubmitResult r = fx.coordinator.submit_from_task(tgt, e, qos);
        REQUIRE(r.disposition == coact::SubmitDisposition::Queued);
    }

    for (int w = 0; w < 400; ++w) {
        if (ao.context().count.load(std::memory_order_relaxed) >= kN) {
            break;
        }
        usleep(5000);
    }

    fx.dispatcher.request_stop();
    fx.pal.join_dispatcher();

    CHECK_EQ(ao.context().count.load(), kN);
    CHECK_EQ(g_pool.used(), 0U);          // every event recycled exactly once
    CHECK_EQ(ao.pending().load(), 0U);    // the coordinator's increments balanced
}

// ---------------------------------------------------------------------------
// Path 2 (lookup failure): an event whose target is not bound is still
// released by the Dispatcher exactly once - no leak.
// ---------------------------------------------------------------------------
COACT_TEST(dispatcher_lookup_fail_still_releases)
{
    Fixture fx;
    // No AO is bound: any target is unbound. Use a value past the registry
    // capacity so lookup() must return nullptr.
    g_pool.init(g_pool_storage, sizeof(g_pool_storage),
                coact::make_spin_critical_section(g_spin));

    coact::Event* e = g_pool.alloc(0x22U);
    REQUIRE(e != nullptr);
    coact::event_ref_inc(e);   // the queued reference
    const coact::TargetId bogus =
        coact::TargetId(Config::kMaxAo + 1U);
    REQUIRE(fx.staging.enqueue(bogus, e, coact::PriorityClass::Normal,
                               fx.pal.monotonic_ns()));

    fx.pal.start_dispatcher(&trampoline<DispatcherT>, &fx.dispatcher);
    usleep(20000);   // let the dispatcher drain the (normal-path) batch

    fx.dispatcher.request_stop();
    fx.pal.join_dispatcher();

    // Released regardless of the lookup result.
    CHECK_EQ(g_pool.used(), 0U);
}

// ---------------------------------------------------------------------------
// Dispatcher thread identity (R1). pal::Posix::in_dispatcher_thread() is a real
// thread-local check: true only on the coact Dispatcher thread, false on every
// other thread. No RAII scope (cmdfw's DispatchedSlotGuard) can forge it, so a
// forged guard cannot open the cmdfw single-writer gate. The Dispatcher template
// forwards the same check to the PAL.
// ---------------------------------------------------------------------------
struct ProbeCtx {
    std::atomic<bool>* seen;
    std::atomic<bool>* dispatcher;
};

static void probe_action(ProbeCtx& c, const coact::Event&) noexcept
{
    c.dispatcher->store(coact::pal::Posix::in_dispatcher_thread(),
                        std::memory_order_relaxed);
    c.seen->store(true, std::memory_order_relaxed);
}

static const coact::StateDef<ProbeCtx> kProbeStates[] = {
    {-1, nullptr, nullptr, nullptr},
    {0, nullptr, nullptr, nullptr},
};
static const coact::TransitionDef<ProbeCtx> kProbeTrans[] = {
    {1, 1U, 1, coact::TransitionKind::Internal, nullptr, probe_action},
};

COACT_TEST(dispatcher_thread_identity_is_real_and_non_forgeable)
{
    // Non-Dispatcher thread (test main): the identity MUST be false. A test
    // could construct any RAII scope, but no scope can flip coact's thread-local
    // Dispatcher marker - this is the forged-guard RED case.
    REQUIRE(!coact::pal::Posix::in_dispatcher_thread());
    REQUIRE(!DispatcherT::in_dispatcher_thread());

    // On the real Dispatcher thread the identity MUST be true (production
    // cmdfw actions run here and legitimately open the slot gate).
    Fixture fx;
    std::atomic<bool> seen{false};
    std::atomic<bool> dispatcher_seen{false};
    coact::Ao<ProbeCtx, coact::Hsm<ProbeCtx>, Traits> ao(
        kProbeStates, 2U, kProbeTrans, 1U, 1, 4U);
    ao.context().seen = &seen;
    ao.context().dispatcher = &dispatcher_seen;
    coact::Event init_e{0U, 0U, 0U};
    ao.init(init_e);
    REQUIRE(fx.registry.bind(&ao, Traits::logical_prio()));
    const coact::TargetId tgt = fx.registry.target_of(&ao);
    REQUIRE(tgt != coact::kInvalidTarget);

    g_pool.init(g_pool_storage, sizeof(g_pool_storage),
                coact::make_spin_critical_section(g_spin));

    coact::Event* e = g_pool.alloc(1U);
    REQUIRE(e != nullptr);
    coact::EventQos qos{false, false};
    coact::SubmitResult r = fx.coordinator.submit_from_task(tgt, e, qos);
    REQUIRE(r.disposition == coact::SubmitDisposition::Queued);

    fx.pal.start_dispatcher(&trampoline<DispatcherT>, &fx.dispatcher);
    for (int w = 0; w < 400; ++w) {
        if (seen.load(std::memory_order_relaxed)) {
            break;
        }
        usleep(5000);
    }
    fx.dispatcher.request_stop();
    fx.pal.join_dispatcher();

    REQUIRE(seen.load(std::memory_order_relaxed));
    CHECK(dispatcher_seen.load(std::memory_order_relaxed));
    CHECK_EQ(g_pool.used(), 0U);
}

// ---------------------------------------------------------------------------
// Path 3 (stop): queued + in-flight events are all released exactly once. The
// stop is requested before the dispatcher starts so the queue is guaranteed
// non-empty at stop - the shutdown drain must release every buffered event.
// ---------------------------------------------------------------------------
COACT_TEST(dispatcher_stop_drains_queued_releases_each_once)
{
    Fixture fx;
    TestAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    coact::Event init_e{0U, 0U, 0U};
    ao.init(init_e);
    REQUIRE(fx.registry.bind(&ao, Traits::logical_prio()));
    const coact::TargetId tgt = fx.registry.target_of(&ao);
    REQUIRE(tgt != coact::kInvalidTarget);

    g_pool.init(g_pool_storage, sizeof(g_pool_storage),
                coact::make_spin_critical_section(g_spin));

    // Five batches' worth, all queued before the dispatcher starts.
    constexpr int kN = 40;
    for (int i = 0; i < kN; ++i) {
        coact::Event* e = g_pool.alloc(1U);
        REQUIRE(e != nullptr);
        coact::EventQos qos{false, false};
        coact::SubmitResult r = fx.coordinator.submit_from_task(tgt, e, qos);
        REQUIRE(r.disposition == coact::SubmitDisposition::Queued);
    }

    // Stop before the dispatcher runs: the shutdown drain must release the
    // whole queue (deterministic under either interleaving).
    fx.dispatcher.request_stop();
    fx.pal.start_dispatcher(&trampoline<DispatcherT>, &fx.dispatcher);
    fx.pal.join_dispatcher();

    CHECK_EQ(g_pool.used(), 0U);          // nothing leaked
    CHECK_EQ(ao.pending().load(), 0U);    // pending increments balanced by drains
}

COACT_TEST_MAIN()
