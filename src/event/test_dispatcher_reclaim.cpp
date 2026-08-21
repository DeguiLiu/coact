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
#include <chrono>
#include <cstdint>
#include <cstring>
#include <new>
#include <sys/wait.h>
#include <thread>
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
using BreakerBankT = coact::BreakerBank<Config>;
using CoordinatorT = coact::DispatchCoordinator<
    StagingT, coact::pal::Posix, BreakerBankT>;
using DispatcherT = coact::Dispatcher<
    StagingT, coact::pal::Posix, coact::HostSmpProfile, BreakerBankT>;

struct SlowBatchConfig : Config {
    enum : uint32_t {
        kBatchTimeoutMs = 1000U,
    };
};
using SlowStagingT = coact::Staging<SlowBatchConfig, coact::pal::Posix::QueueBackend>;

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

struct DirectTraits {
    static coact::LogicalPrio logical_prio() noexcept { return 31U; }
    static coact::PriorityClass priority_class() noexcept
    {
        return coact::PriorityClass::Normal;
    }
    static bool direct_eligible() noexcept { return true; }
    static bool isr_direct_safe() noexcept { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
using DirectTestAo = coact::Ao<Ctx, coact::Hsm<Ctx>, DirectTraits>;

struct ContendedCtx {
    std::atomic<int> direct_count{0};
    std::atomic<int> queued_count{0};
    std::atomic<bool> direct_entered{false};
    std::atomic<bool> release_direct{false};
};

static void on_contended_evt(ContendedCtx& context,
                             const coact::Event& event) noexcept
{
    if (1U == event.signal) {
        context.direct_entered.store(true, std::memory_order_release);
        while (!context.release_direct.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        context.direct_count.fetch_add(1, std::memory_order_relaxed);
    }
    else {
        context.queued_count.fetch_add(1, std::memory_order_relaxed);
    }
}

static const coact::StateDef<ContendedCtx> kContendedStates[] = {
    {-1, nullptr, nullptr, nullptr},
    {0, nullptr, nullptr, nullptr},
};
static const coact::TransitionDef<ContendedCtx> kContendedTrans[] = {
    {1, 1U, 1, coact::TransitionKind::Internal, nullptr, on_contended_evt},
    {1, 2U, 1, coact::TransitionKind::Internal, nullptr, on_contended_evt},
};
using ContendedAo = coact::Ao<ContendedCtx, coact::Hsm<ContendedCtx>, DirectTraits>;

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
    coact::BreakerBank<Config> breaker;
    CoordinatorT coordinator;
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

struct BlockingAdmissionPolicy {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
};

coact::PolicyResult block_after_admission(void* context, coact::TargetId,
                                          const coact::Event&, const coact::EventQos&,
                                          uint64_t) noexcept
{
    BlockingAdmissionPolicy& policy = *static_cast<BlockingAdmissionPolicy*>(context);
    policy.entered.store(true, std::memory_order_release);
    while (!policy.release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    return coact::PolicyResult{true, false, coact::kReasonOk};
}

const coact::PolicyOps kBlockingAdmissionOps = {
    &block_after_admission,
    nullptr,
};

class ObservedPosixPal {
public:
    coact::pal::CriticalToken irq_save() noexcept
    {
        return pal_.irq_save();
    }

    void irq_restore(coact::pal::CriticalToken token) noexcept
    {
        pal_.irq_restore(token);
    }

    static bool in_dispatcher_thread() noexcept
    {
        return coact::pal::Posix::in_dispatcher_thread();
    }

    uint64_t monotonic_ns() const noexcept
    {
        return pal_.monotonic_ns();
    }

    void wait_dispatcher(uint32_t timeout_ms) noexcept
    {
        wait_calls_.fetch_add(1U, std::memory_order_release);
        pal_.wait_dispatcher(timeout_ms);
    }

    void signal_dispatcher_from_task() noexcept
    {
        pal_.signal_dispatcher_from_task();
    }

    void signal_dispatcher_from_isr() noexcept
    {
        pal_.signal_dispatcher_from_isr();
    }

    void start_dispatcher(coact::pal::ThreadEntry entry, void* context) noexcept
    {
        pal_.start_dispatcher(entry, context);
    }

    void join_dispatcher() noexcept
    {
        pal_.join_dispatcher();
    }

    void enter_direct() noexcept
    {
        pal_.enter_direct();
    }

    void leave_direct() noexcept
    {
        pal_.leave_direct();
    }

    uint32_t wait_calls() const noexcept
    {
        return wait_calls_.load(std::memory_order_acquire);
    }

private:
    coact::pal::Posix pal_;
    std::atomic<uint32_t> wait_calls_{0U};
};

using SlowBreakerBankT = coact::BreakerBank<SlowBatchConfig>;
using ObservedSlowCoordinatorT = coact::DispatchCoordinator<
    SlowStagingT, ObservedPosixPal, SlowBreakerBankT>;
using ObservedSlowDispatcherT = coact::Dispatcher<
    SlowStagingT, ObservedPosixPal, coact::HostSmpProfile, SlowBreakerBankT>;

static bool wait_until(const std::atomic<bool>& value,
                       std::chrono::steady_clock::time_point deadline) noexcept
{
    while (!value.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return value.load(std::memory_order_acquire);
}

static bool run_direct_dispatcher_contention(bool stop_with_deferred) noexcept
{
    ObservedPosixPal pal;
    SlowBatchConfig config;
    SlowStagingT staging(coact::make_critical_section(pal));
    coact::AoRegistry<SlowBatchConfig> registry;
    coact::Monitor<SlowBatchConfig> monitor;
    coact::BreakerBank<SlowBatchConfig> breaker(config);
    ObservedSlowCoordinatorT coordinator(
        staging, registry, monitor, breaker, pal);
    ObservedSlowDispatcherT dispatcher(staging, registry, monitor, breaker, pal);

    ContendedAo ao(kContendedStates, 2U, kContendedTrans, 2U, 1, 4U);
    ao.init(coact::Event{0U, 0U, 0U});
    if (!registry.bind(&ao, DirectTraits::logical_prio())) {
        return false;
    }
    const coact::TargetId target = registry.target_of(&ao);

    g_pool.init(g_pool_storage, sizeof(g_pool_storage),
                coact::make_spin_critical_section(g_spin));
    coact::Event* direct_event = g_pool.alloc(1U);
    coact::Event* queued_event = g_pool.alloc(2U);
    if ((nullptr == direct_event) || (nullptr == queued_event)) {
        return false;
    }

    const coact::EventQos qos{false, false};
    coact::SubmitResult direct_result{coact::SubmitDisposition::RejectedState, 0U};
    std::thread producer([&]() {
        direct_result = coordinator.submit_from_task(target, direct_event, qos);
    });

    const auto enter_deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(250);
    bool passed = wait_until(ao.context().direct_entered, enter_deadline);
    const coact::SubmitResult queued_result =
        coordinator.submit_from_task(target, queued_event, qos);
    passed = passed
          && (coact::SubmitDisposition::Queued == queued_result.disposition)
          && (1U == ao.pending().load());

    pal.start_dispatcher(&trampoline<ObservedSlowDispatcherT>, &dispatcher);
    const auto wait_deadline = std::chrono::steady_clock::now()
                             + std::chrono::milliseconds(250);
    while ((pal.wait_calls() < 1U)
           && (std::chrono::steady_clock::now() < wait_deadline)) {
        std::this_thread::yield();
    }
    passed = passed && (pal.wait_calls() >= 1U);

    if (stop_with_deferred) {
        dispatcher.request_stop();
    }
    ao.context().release_direct.store(true, std::memory_order_release);
    producer.join();

    if (!stop_with_deferred) {
        const auto dispatch_deadline = std::chrono::steady_clock::now()
                                     + std::chrono::milliseconds(250);
        while ((0 == ao.context().queued_count.load(std::memory_order_acquire))
               && (std::chrono::steady_clock::now() < dispatch_deadline)) {
            std::this_thread::yield();
        }
        dispatcher.request_stop();
    }
    pal.join_dispatcher();

    passed = passed
          && (coact::SubmitDisposition::Direct == direct_result.disposition)
          && (1 == ao.context().direct_count.load(std::memory_order_acquire))
          && ((stop_with_deferred ? 0 : 1)
              == ao.context().queued_count.load(std::memory_order_acquire))
          && (0U == ao.pending().load())
          && (0U == g_pool.used());
    return passed;
}

static bool contention_child_succeeds(bool stop_with_deferred) noexcept
{
    const pid_t pid = fork();
    if (0 == pid) {
        std::_Exit(run_direct_dispatcher_contention(stop_with_deferred) ? 0 : 1);
    }
    if (pid < 0) {
        return false;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && (0 == WEXITSTATUS(status));
}

struct ReservedFrontStaging {
    using ConfigType = Config;

    void begin_batch() noexcept { batch_used_ = 0U; }
    void arm_dispatcher_wait() noexcept {}
    void close_admission() noexcept {}
    bool submissions_idle() const noexcept { return true; }
    bool dequeue_one(coact::StagingSlot& out, uint64_t) noexcept
    {
        if (!front_ready_ || consumed_) {
            return false;
        }
        out = coact::StagingSlot{coact::kInvalidTarget, &event_, 0U};
        consumed_ = true;
        ++batch_used_;
        return true;
    }
    uint8_t batch_used() const noexcept { return batch_used_; }
    bool any_buffered() const noexcept { return !consumed_; }
    bool any_ready() const noexcept { return front_ready_ && !consumed_; }
    void release_front() noexcept { front_ready_ = true; }

private:
    coact::Event event_{1U, 0U, 0U};
    bool front_ready_ = false;
    bool consumed_ = false;
    uint8_t batch_used_ = 0U;
};

struct ReservedFrontPal {
    explicit ReservedFrontPal(ReservedFrontStaging& staging) noexcept
        : staging_(staging) {}

    static bool in_dispatcher_thread() noexcept { return true; }
    uint64_t monotonic_ns() const noexcept { return 0U; }
    void wait_dispatcher(uint32_t) noexcept
    {
        ++wait_count;
        staging_.release_front();
    }
    void signal_dispatcher_from_task() noexcept {}
    void signal_dispatcher_from_isr() noexcept {}

    int wait_count = 0;

private:
    ReservedFrontStaging& staging_;
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

// Once shutdown begins, Coordinator must consume late submissions immediately.
// Otherwise a post-join event remains staged forever because the Dispatcher is
// already gone and no longer owns a final release path.
COACT_TEST(dispatcher_stop_rejects_late_submission_without_pool_leak)
{
    Fixture fx;
    TestAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    coact::Event init_e{0U, 0U, 0U};
    ao.init(init_e);
    REQUIRE(fx.registry.bind(&ao, Traits::logical_prio()));
    const coact::TargetId target = fx.registry.target_of(&ao);
    REQUIRE(target != coact::kInvalidTarget);

    g_pool.init(g_pool_storage, sizeof(g_pool_storage),
                coact::make_spin_critical_section(g_spin));

    fx.dispatcher.request_stop();
    fx.pal.start_dispatcher(&trampoline<DispatcherT>, &fx.dispatcher);
    fx.pal.join_dispatcher();

    coact::Event* late = g_pool.alloc(1U);
    REQUIRE(late != nullptr);
    const coact::EventQos qos{false, false};
    const coact::SubmitResult result =
        fx.coordinator.submit_from_task(target, late, qos);

    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::RejectedState),
             static_cast<int>(result.disposition));
    CHECK_EQ(g_pool.used(), 0U);
    CHECK_EQ(ao.pending().load(), 0U);
}

COACT_TEST(dispatcher_stop_drains_submission_admitted_before_close)
{
    coact::pal::Posix pal;
    Config config;
    StagingT staging(coact::make_critical_section(pal));
    coact::AoRegistry<Config> registry;
    coact::Monitor<Config> monitor;
    coact::BreakerBank<Config> breaker(config);
    BlockingAdmissionPolicy policy;
    CoordinatorT coordinator(
        staging, registry, monitor, breaker, pal, &kBlockingAdmissionOps, &policy);
    DispatcherT dispatcher(staging, registry, monitor, breaker, pal);

    TestAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    coact::Event init_e{0U, 0U, 0U};
    ao.init(init_e);
    REQUIRE(registry.bind(&ao, Traits::logical_prio()));
    const coact::TargetId target = registry.target_of(&ao);
    REQUIRE(target != coact::kInvalidTarget);

    g_pool.init(g_pool_storage, sizeof(g_pool_storage),
                coact::make_spin_critical_section(g_spin));
    coact::Event* event = g_pool.alloc(1U);
    REQUIRE(event != nullptr);
    const coact::EventQos qos{false, false};
    coact::SubmitResult result{coact::SubmitDisposition::RejectedState, 0U};
    std::thread producer([&]() {
        result = coordinator.submit_from_task(target, event, qos);
    });
    while (!policy.entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    dispatcher.request_stop();
    pal.start_dispatcher(&trampoline<DispatcherT>, &dispatcher);
    std::atomic<bool> joined{false};
    std::thread joiner([&]() {
        pal.join_dispatcher();
        joined.store(true, std::memory_order_release);
    });

    usleep(20000);
    CHECK(!joined.load(std::memory_order_acquire));

    policy.release.store(true, std::memory_order_release);
    producer.join();
    joiner.join();

    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(result.disposition));
    CHECK_EQ(g_pool.used(), 0U);
    CHECK_EQ(ao.pending().load(), 0U);
}

// The final admitted submission must wake the stop drain even if it completes
// through direct dispatch, which has no staging enqueue wakeup. The stop wake
// is deliberately consumed before the policy is released; timely join below
// therefore depends on SubmissionLease's destructor signal.
COACT_TEST(dispatcher_stop_last_admission_lease_wakes_direct_submit)
{
    ObservedPosixPal pal;
    SlowBatchConfig config;
    SlowStagingT staging(coact::make_critical_section(pal));
    coact::AoRegistry<SlowBatchConfig> registry;
    coact::Monitor<SlowBatchConfig> monitor;
    coact::BreakerBank<SlowBatchConfig> breaker(config);
    BlockingAdmissionPolicy policy;
    ObservedSlowCoordinatorT coordinator(
        staging, registry, monitor, breaker, pal, &kBlockingAdmissionOps, &policy);
    ObservedSlowDispatcherT dispatcher(staging, registry, monitor, breaker, pal);

    DirectTestAo ao(kStates, 2U, kTrans, 1U, 1, 4U);
    coact::Event init_e{0U, 0U, 0U};
    ao.init(init_e);
    REQUIRE(registry.bind(&ao, DirectTraits::logical_prio()));
    const coact::TargetId target = registry.target_of(&ao);
    REQUIRE(target != coact::kInvalidTarget);

    g_pool.init(g_pool_storage, sizeof(g_pool_storage),
                coact::make_spin_critical_section(g_spin));
    coact::Event* event = g_pool.alloc(1U);
    REQUIRE(event != nullptr);
    const coact::EventQos qos{false, false};
    coact::SubmitResult result{coact::SubmitDisposition::RejectedState, 0U};
    std::thread producer([&]() {
        result = coordinator.submit_from_task(target, event, qos);
    });
    while (!policy.entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    dispatcher.request_stop();
    pal.start_dispatcher(&trampoline<ObservedSlowDispatcherT>, &dispatcher);
    std::atomic<bool> joined{false};
    std::thread joiner([&]() {
        pal.join_dispatcher();
        joined.store(true, std::memory_order_release);
    });

    const auto wait_deadline = std::chrono::steady_clock::now()
                             + std::chrono::milliseconds(250);
    // The second entry can occur only after the first wait returned and
    // consumed request_stop()'s latched wake token. It is now safe to release
    // the final admission lease: no pre-stop wake remains for the drain.
    while (pal.wait_calls() < 2U && std::chrono::steady_clock::now() < wait_deadline) {
        std::this_thread::yield();
    }
    CHECK(pal.wait_calls() >= 2U);
    CHECK(!joined.load(std::memory_order_acquire));

    const auto release_time = std::chrono::steady_clock::now();
    policy.release.store(true, std::memory_order_release);
    producer.join();
    joiner.join();
    const auto wake_latency = std::chrono::steady_clock::now() - release_time;

    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Direct),
             static_cast<int>(result.disposition));
    CHECK(wake_latency < std::chrono::milliseconds(250));
    CHECK_EQ(ao.context().count.load(), 1);
    CHECK_EQ(g_pool.used(), 0U);
    CHECK_EQ(ao.pending().load(), 0U);
}

COACT_TEST(dispatcher_stop_waits_for_reserved_front_slot)
{
    ReservedFrontStaging staging;
    ReservedFrontPal pal(staging);
    Config config;
    coact::AoRegistry<Config> registry;
    coact::Monitor<Config> monitor;
    coact::BreakerBank<Config> breaker(config);
    coact::Dispatcher<ReservedFrontStaging, ReservedFrontPal,
                      coact::HostSmpProfile, BreakerBankT> dispatcher(
        staging, registry, monitor, breaker, pal);

    dispatcher.request_stop();
    dispatcher.run();

    CHECK_EQ(pal.wait_count, 1);
    CHECK(!staging.any_buffered());
}

COACT_TEST(dispatcher_defers_queued_event_while_direct_holds_lease)
{
    CHECK(contention_child_succeeds(false));
}

COACT_TEST(dispatcher_stop_releases_deferred_event_exactly_once)
{
    CHECK(contention_child_succeeds(true));
}

COACT_TEST_MAIN()
