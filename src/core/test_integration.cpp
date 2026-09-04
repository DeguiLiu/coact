// coact end-to-end integration test: event pool -> submit -> Dispatcher ->
// AO action -> event gc. Multi-AO, multi-event.
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <atomic>
#include <cstring>
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
static std::atomic<bool> g_trace_dispatch_captured{false};

/* B-handler-run probe for the nested-serialization test (see below). */
static std::atomic<int> g_nested_b_ran{0};
template <typename AoT>
static bool g_counter_probe(AoT*)
{
    return g_nested_b_ran.load() != 0;
}

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

using IntHsm = coact::Hsm<IntCtx>;
using AoA    = coact::Ao<IntCtx, IntHsm, TraitsA>;
using AoB    = coact::Ao<IntCtx, IntHsm, TraitsB>;

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
    pool.init(g_pool_storage, sizeof(g_pool_storage), coact::detail::noop_cs()); // single-threaded test

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
 * Nested-dispatch guard integration: AO-A's handler (running on the real
 * Dispatcher thread) submits to a direct-eligible, Idle AO-B. Without the
 * in_dispatcher_thread() guard the coordinator would run B's handler inline
 * on the same stack, so B's handler would observe a_running == true. With the
 * guard the event goes to staging and B runs only after A's handler returns.
 * (The demo's own AOs are all staged-only, so this regression surface is
 * exercised only by the test traits below.)
 * ========================================================================= */
static std::atomic<bool> g_a_running{false};
static std::atomic<bool> g_b_ran_while_a_running{false};
static coact::Runtime<coact::DefaultConfig, coact::pal::Posix>* g_rt = nullptr;
static IntPool* g_nested_pool = nullptr;

struct NestedCtx {};

static void n_noop_entry(NestedCtx&) {}
static void n_noop_exit(NestedCtx&)  {}
static bool n_ok(const NestedCtx&, const coact::Event&) { return true; }

static void n_action_a(NestedCtx&, const coact::Event&)
{
    g_a_running.store(true, std::memory_order_release);
    coact::Event* eb = g_nested_pool->alloc(2U);
    if (eb != nullptr) {
        g_rt->coordinator().submit_from_task(coact::TargetId(2U), eb,
                                             coact::EventQos{false, false});
    }
    /* Hold the handler briefly so an inline (unguarded) direct dispatch of B
       would deterministically observe a_running == true. */
    usleep(20000);
    g_a_running.store(false, std::memory_order_release);
}

static void n_action_b(NestedCtx&, const coact::Event&)
{
    g_nested_b_ran.fetch_add(1, std::memory_order_release);
    if (g_a_running.load(std::memory_order_acquire)) {
        g_b_ran_while_a_running.store(true, std::memory_order_release);
    }
}

static const coact::StateDef<NestedCtx> kNStates[] = {
    { -1, nullptr, nullptr },
    {  0, n_noop_entry, n_noop_exit },
};
static const coact::TransitionDef<NestedCtx> kNTransA[] = {
    { 1, 1U, 1, coact::TransitionKind::Internal, n_ok, n_action_a },
};
static const coact::TransitionDef<NestedCtx> kNTransB[] = {
    { 1, 2U, 1, coact::TransitionKind::Internal, n_ok, n_action_b },
};

struct NestedTraitsA {
    static coact::LogicalPrio   logical_prio()   { return 20U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 100000000ULL;
};
struct NestedTraitsB {
    static coact::LogicalPrio   logical_prio()   { return 21U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return true; }   /* the whole point */
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using NAoA = coact::Ao<NestedCtx, coact::Hsm<NestedCtx>, NestedTraitsA>;
using NAoB = coact::Ao<NestedCtx, coact::Hsm<NestedCtx>, NestedTraitsB>;

COACT_TEST(nested_submit_from_handler_serializes)
{
    g_a_running.store(false);
    g_b_ran_while_a_running.store(false);
    g_nested_b_ran.store(0);

    NAoA ao_a(kNStates, 2U, kNTransA, 1U, 1, 4U);
    NAoB ao_b(kNStates, 2U, kNTransB, 1U, 1, 4U);

    coact::Event init_e;
    init_e.signal  = 0U;
    init_e.pool_id = 0U;
    init_e.ref_ctr = 0U;
    ao_a.init(init_e);
    ao_b.init(init_e);

    IntPool pool;
    pool.init(g_pool_storage, sizeof(g_pool_storage), coact::detail::noop_cs());
    g_nested_pool = &pool;

    coact::pal::Posix pal;
    coact::Runtime<coact::DefaultConfig, coact::pal::Posix> rt(pal);

    CHECK(rt.bind(&ao_a));
    CHECK(rt.bind(&ao_b));
    CHECK(rt.initialize());
    g_rt = &rt;
    rt.start();

    coact::Event* ea = pool.alloc(1U);
    REQUIRE(ea != nullptr);
    const coact::SubmitResult r =
        rt.coordinator().submit_from_task(coact::TargetId(1U), ea,
                                          coact::EventQos{false, false});
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(r.disposition));

    /* Wait until B's handler ran (or 1 s deadline). */
    for (int w = 0; w < 200; ++w) {
        if (g_counter_probe(&ao_b)) {
            break;
        }
        usleep(5000);
    }
    g_rt = nullptr;
    g_nested_pool = nullptr;
    rt.stop();

    /* B must never have run while A's handler was still on the stack. */
    CHECK_EQ(false, g_b_ran_while_a_running.load());
    CHECK_EQ(1, g_nested_b_ran.load());
    CHECK_EQ(0U, pool.used());
}

/* =========================================================================
 * Dispatcher hang detection (RTC layer-3 hole): the RTC budget only fires
 * after a handler RETURNS; a handler that blocks forever freezes the
 * Dispatcher inside try_dispatch_queued with no breaker trip. The heartbeat
 * added to the Dispatcher loop tops this: while the handler spins, progress
 * stops advancing and an external thread probing dispatcher_alive_within()
 * observes the hang.
 * ========================================================================= */
static std::atomic<bool> g_hang_release{false};
static std::atomic<bool> g_hang_entered{false};

static void h_noop_entry(NestedCtx&) {}
static void h_noop_exit(NestedCtx&)  {}
static bool h_ok(const NestedCtx&, const coact::Event&) { return true; }

static void h_action_hang(NestedCtx&, const coact::Event&)
{
    g_hang_entered.store(true, std::memory_order_release);
    /* Controllable hang: spin in 1 ms slices so the RAII release flag is
       observed promptly. Never blocks the CI beyond the test body. */
    while (!g_hang_release.load(std::memory_order_acquire)) {
        usleep(1000);
    }
}

static const coact::StateDef<NestedCtx> kHStates[] = {
    { -1, nullptr, nullptr },
    {  0, h_noop_entry, h_noop_exit },
};
static const coact::TransitionDef<NestedCtx> kHTrans[] = {
    { 1, 1U, 1, coact::TransitionKind::Internal, h_ok, h_action_hang },
};

struct HangTraits {
    static coact::LogicalPrio   logical_prio()   { return 30U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
using HangAo = coact::Ao<NestedCtx, coact::Hsm<NestedCtx>, HangTraits>;

/* RAII: releases the hang on ANY scope exit (including CHECK failure) so
   rt.stop() can always join and the CI never deadlocks. */
struct HangReleaseGuard {
    ~HangReleaseGuard() { g_hang_release.store(true, std::memory_order_release); }
};

COACT_TEST(dispatcher_hang_is_externally_detectable)
{
    g_hang_release.store(false);
    g_hang_entered.store(false);

    HangAo ao(kHStates, 2U, kHTrans, 1U, 1, 4U);
    coact::Event init_e;
    init_e.signal  = 0U;
    init_e.pool_id = 0U;
    init_e.ref_ctr = 0U;
    ao.init(init_e);

    IntPool pool;
    pool.init(g_pool_storage, sizeof(g_pool_storage), coact::detail::noop_cs());

    coact::pal::Posix pal;
    coact::Runtime<coact::DefaultConfig, coact::pal::Posix> rt(pal);

    CHECK(rt.bind(&ao));
    CHECK(rt.initialize());
    rt.start();

    HangReleaseGuard guard;

    coact::Event* e = pool.alloc(1U);
    REQUIRE(e != nullptr);
    rt.coordinator().submit_from_task(coact::TargetId(1U), e,
                                      coact::EventQos{false, false});

    /* Wait until the handler is inside its spin (1 s deadline). */
    for (int w = 0; w < 200; ++w) {
        if (g_hang_entered.load(std::memory_order_acquire)) {
            break;
        }
        usleep(5000);
    }
    REQUIRE(g_hang_entered.load());

    /* External watchdog view: while the handler holds the Dispatcher, the
       heartbeat is stale. Sample, wait past a 50 ms window, assert dead. */
    const uint64_t progress_during_hang =
        pal.dispatcher_progress_ns();
    usleep(100000);
    CHECK(!pal.dispatcher_alive_within(50U));
    CHECK_EQ(progress_during_hang, pal.dispatcher_progress_ns());

    /* Release the hang: the loop resumes beating. */
    g_hang_release.store(true, std::memory_order_release);
    usleep(50000);
    CHECK(pal.dispatcher_alive_within(200U));

    rt.stop();
    CHECK_EQ(0U, pool.used());
}

/* =========================================================================
 * TraceOps dispatcher instrumentation: a queued dispatch that actually runs
 * the handler must record exactly one on_dispatch with path=1 and timeout
 * matching the RTC budget comparison (design trace §3.2).
 * ========================================================================= */
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
};

static void capture_dispatch(void* ctx, coact::TargetId target,
                             uint64_t elapsed_ns, uint8_t path,
                             uint8_t timeout) noexcept
{
    CapturedTrace* cap = static_cast<CapturedTrace*>(ctx);
    cap->last_target_raw = target.raw();
    cap->last_elapsed = elapsed_ns;
    cap->last_path = path;
    cap->last_timeout = timeout;
    g_trace_dispatch_captured.store(true, std::memory_order_release);
}

COACT_TEST(dispatcher_trace_dispatch_records)
{
    g_counter_a.store(0);
    g_trace_dispatch_captured.store(false);

    AoA ao_a(kStates, 2U, kTransA, 1U, 1, 4U);
    coact::Event init_e;
    init_e.signal  = 0U;
    init_e.pool_id = 0U;
    init_e.ref_ctr = 0U;
    ao_a.init(init_e);

    IntPool pool;
    pool.init(g_pool_storage, sizeof(g_pool_storage), coact::detail::noop_cs());

    coact::pal::Posix pal;
    coact::Runtime<coact::DefaultConfig, coact::pal::Posix> rt(pal);

    CapturedTrace captured{};
    coact::TraceOps ops{};
    ops.on_dispatch = &capture_dispatch;
    ops.ctx = &captured;
    rt.monitor().bind_trace(ops);

    CHECK(rt.bind(&ao_a));
    CHECK(rt.initialize());
    rt.start();

    coact::Event* ea = pool.alloc(1U);
    REQUIRE(ea != nullptr);
    const coact::SubmitResult r =
        rt.coordinator().submit_from_task(coact::TargetId(1U), ea,
                                          coact::EventQos{false, false});
    CHECK_EQ(static_cast<int>(coact::SubmitDisposition::Queued),
             static_cast<int>(r.disposition));

    /* Wait until the Dispatcher has recorded the trace (or 1 s deadline). */
    for (int w = 0; w < 200; ++w) {
        if (g_trace_dispatch_captured.load(std::memory_order_acquire)) {
            break;
        }
        usleep(5000);
    }
    rt.stop();

    CHECK(g_trace_dispatch_captured.load(std::memory_order_acquire));
    CHECK_EQ(captured.last_target_raw, 1U);
    CHECK_EQ(captured.last_path, 1U);
    CHECK_EQ(captured.last_timeout, 0U);
    CHECK(coact_test::relaxed(
              rt.monitor().ao(coact::TargetId(1U)).dispatcher_duration_ns)
              > 0ULL);
    CHECK_EQ(0U, pool.used());
}

}  // namespace

COACT_TEST_MAIN()
