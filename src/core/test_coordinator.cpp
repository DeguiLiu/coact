// coact coordinator wakeup optimization test (flame finding): the Staging wake
// latch starts set while the Dispatcher drains, then the Dispatcher clears it
// before its final Ready re-check. Producers publish before setting the latch;
// only the first false-to-true exchange signals. A mock PAL counts signals.
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/coordinator.hpp"
#include "coact/dispatcher.hpp"
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

/* =========================================================================
 * Idle-wait wakeup: a Low event submitted while the Dispatcher is parked in
 * its unbounded idle wait must still be served inside kLowMaxWaitMs, and it
 * must be served on EVERY idle entry - not only the first. The integration TU
 * covers this against the POSIX PAL, but it is skipped under
 * COACT_PORTABLE_ONLY, so this is the copy that runs on the shipping Windows
 * runner. The second round is what catches a latch that is armed once and
 * never re-armed.
 *
 * BlockingPal parks the Dispatcher thread on a condition variable, so a lost
 * wake strands the event for the whole ctest timeout exactly as on the real
 * PALs. timeout_ms == 0 follows the PAL convention of waiting forever; the
 * deferred and stop-drain paths still use bounded nonzero waits.
 * ========================================================================= */
struct BlockingPal {
    std::mutex mtx;
    std::condition_variable cv;
    bool wake_pending = false;
    std::atomic<unsigned> idle_entries{0U};

    uint64_t monotonic_ns() const noexcept
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void signal_dispatcher_from_task() noexcept { wake(); }
    void signal_dispatcher_from_isr() noexcept  { wake(); }
    void enter_direct() noexcept {}
    void leave_direct() noexcept  {}

    void wait_dispatcher(uint32_t timeout_ms) noexcept
    {
        std::unique_lock<std::mutex> lock(mtx);
        if (0U == timeout_ms) {
            idle_entries.fetch_add(1U, std::memory_order_release);
            cv.wait(lock, [this]() { return wake_pending; });
        }
        else {
            cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [this]() { return wake_pending; });
        }
        wake_pending = false;
    }

private:
    void wake() noexcept
    {
        std::lock_guard<std::mutex> lock(mtx);
        wake_pending = true;
        cv.notify_one();
    }
};

/* Low-priority, non-direct-eligible AO: every submit is staged in the Low
   partition, so it can only be served after the Dispatcher wakes from its idle
   wait. Mirrors TraitsLow in test_integration.cpp. */
struct IdleLowTraits {
    static coact::LogicalPrio   logical_prio()   { return 30U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Low; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
using IdleLowAo = coact::Ao<Ctx, CtxHsm, IdleLowTraits>;

static std::atomic<int> g_idle_low_count{0};
static void idle_low_action(Ctx&, const coact::Event&)
{
    g_idle_low_count.fetch_add(1, std::memory_order_relaxed);
}
static const coact::TransitionDef<Ctx> kIdleLowTrans[] = {
    { 1, 1U, 1, coact::TransitionKind::Internal, c_ok, idle_low_action },
};

using IdleStageT = coact::Staging<coact::DefaultConfig, coact::BoundedMpscQueue>;

static IdleStageT make_idle_staging()
{
    return IdleStageT(coact::CriticalSection{nullptr,
        [](void*) -> coact::CriticalSection::Token { return 0U; },
        [](void*, coact::CriticalSection::Token) {}});
}

/* Bounded wait for the Dispatcher to have parked in the unbounded idle wait at
   least `target` times. */
static bool wait_for_idle_entries(BlockingPal& pal, unsigned target,
                                  std::chrono::milliseconds budget)
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while ((pal.idle_entries.load(std::memory_order_acquire) < target)
           && (std::chrono::steady_clock::now() < deadline)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pal.idle_entries.load(std::memory_order_acquire) >= target;
}

COACT_TEST(dispatcher_idle_wait_wakes_low_event_on_each_idle_entry)
{
    g_idle_low_count.store(0);

    coact::Event init_e{};
    init_e.signal = 0U;
    init_e.pool_id = 0U;
    init_e.ref_ctr = 0U;
    IdleLowAo ao(kStates, 2U, kIdleLowTrans, 1U, 1, 4U);
    ao.init(init_e);

    IdleStageT staging = make_idle_staging();
    coact::AoRegistry<coact::DefaultConfig> registry;
    coact::Monitor<coact::DefaultConfig> monitor;
    coact::DefaultConfig cfg{};
    coact::Breaker<coact::DefaultConfig> breaker(cfg);
    BlockingPal pal;
    coact::DispatchCoordinator<IdleStageT, BlockingPal,
                               coact::Breaker<coact::DefaultConfig>> coord(
        staging, registry, monitor, breaker, pal);
    coact::Dispatcher<IdleStageT, BlockingPal, coact::HostSmpProfile,
                      coact::Breaker<coact::DefaultConfig>> dispatcher(
        staging, registry, monitor, breaker, pal);
    REQUIRE(registry.bind(&ao, ao.logical_prio()));

    std::thread runner([&dispatcher]() { dispatcher.run(); });

    const auto budget = std::chrono::milliseconds(
        coact::DefaultConfig::kLowMaxWaitMs);
    const coact::EventQos qos{false, false};
    coact::Event e{};
    e.signal = 1U;
    e.pool_id = 0U;   /* static event: the reclaimer never touches it */
    e.ref_ctr = 0U;

    for (int round = 0; round < 2; ++round) {
        const unsigned idle_target = static_cast<unsigned>(round) + 1U;
        CHECK(wait_for_idle_entries(pal, idle_target, budget));

        const auto t0 = std::chrono::steady_clock::now();
        const coact::SubmitResult r =
            coord.submit_from_task(coact::TargetId(1U), &e, qos);
        CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
                 static_cast<int>(r.disposition));

        const int want = round + 1;
        while ((g_idle_low_count.load(std::memory_order_acquire) != want)
               && (std::chrono::steady_clock::now() < t0 + budget)) {
            std::this_thread::yield();
        }
        const auto served_after = std::chrono::steady_clock::now() - t0;
        CHECK_EQ(want, g_idle_low_count.load(std::memory_order_acquire));
        CHECK(served_after < budget);
    }

    dispatcher.request_stop();
    runner.join();
}

}  // namespace

COACT_TEST_MAIN()
