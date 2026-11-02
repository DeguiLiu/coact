/*
 * coact on-target verification program (Renode, Cortex-M4, bare metal).
 *
 * Output: one PASS/FAIL line per assertion on USART2, then
 * "RESULT PASS n/m" or "RESULT FAIL m/n". The virtual clock is advanced only
 * by the scenarios; nothing here sleeps or depends on wall time.
 */
#include <cstddef>
#include <cstdint>

#include "coact/admission.hpp"
#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/coordinator.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/monitor.hpp"
#include "coact/policy.hpp"
#include "coact/pool.hpp"
#include "coact/staging.hpp"

#include "output.hpp"
#include "pal_baremetal.hpp"

extern "C" {
[[noreturn]] void heap_trap(void);
}

/* No heap, by construction: the freestanding runtime provides no malloc, so
 * any allocation on the submit path traps instead of failing to link. S9
 * asserts the static footprint; this asserts the dynamic one stays zero. */
void* operator new(std::size_t) noexcept
{
    heap_trap();
}

void* operator new[](std::size_t) noexcept
{
    heap_trap();
}

void operator delete(void*) noexcept {}
void operator delete[](void*) noexcept {}
void operator delete(void*, std::size_t) noexcept {}
void operator delete[](void*, std::size_t) noexcept {}

namespace {

// ---------------------------------------------------------------------------
// Assertion reporting
// ---------------------------------------------------------------------------
int g_pass = 0;
int g_fail = 0;

const char* disposition_name(coact::SubmitDisposition d)
{
    switch (d) {
    case coact::SubmitDisposition::Direct: return "Direct";
    case coact::SubmitDisposition::Queued: return "Queued";
    case coact::SubmitDisposition::Merged: return "Merged";
    case coact::SubmitDisposition::DroppedPolicy: return "DroppedPolicy";
    case coact::SubmitDisposition::DroppedRateLimit: return "DroppedRateLimit";
    case coact::SubmitDisposition::DroppedOverload: return "DroppedOverload";
    case coact::SubmitDisposition::RejectedFull: return "RejectedFull";
    case coact::SubmitDisposition::RejectedState: return "RejectedState";
    }
    return "?";
}

void record(bool ok, const char* name)
{
    harness::puts(ok ? "PASS " : "FAIL ");
    harness::puts(name);
    harness::newline();
    if (ok) {
        ++g_pass;
    }
    else {
        ++g_fail;
    }
}

void record_u64(const char* name, uint64_t got, uint64_t want)
{
    if (got == want) {
        record(true, name);
        return;
    }
    harness::puts("FAIL ");
    harness::puts(name);
    harness::puts(" got=");
    harness::put_u64(got);
    harness::puts(" want=");
    harness::put_u64(want);
    harness::newline();
    ++g_fail;
}

void record_disposition(const char* name, coact::SubmitDisposition got,
                        coact::SubmitDisposition want)
{
    if (got == want) {
        record(true, name);
        return;
    }
    harness::puts("FAIL ");
    harness::puts(name);
    harness::puts(" got=");
    harness::puts(disposition_name(got));
    harness::puts(" want=");
    harness::puts(disposition_name(want));
    harness::newline();
    ++g_fail;
}

// ---------------------------------------------------------------------------
// Minimal HSM used by the test AOs. Signal 1 runs one Internal action.
// ---------------------------------------------------------------------------
struct Ctx {
    int actions = 0;
};

void ctx_entry(Ctx&) {}
void ctx_exit(Ctx&) {}
bool ctx_guard(const Ctx&, const coact::Event&) { return true; }
void ctx_action(Ctx& ctx, const coact::Event&)
{
    ++ctx.actions;
}

const coact::StateDef<Ctx> kStates[] = {
    {-1, nullptr, nullptr},
    {0, ctx_entry, ctx_exit},
};

const coact::TransitionDef<Ctx> kTransitions[] = {
    {1, 1U, 1, coact::TransitionKind::Internal, ctx_guard, ctx_action},
};

struct DirectTraits {
    static coact::LogicalPrio logical_prio() { return 10U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return true; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

struct StagedTraits {
    static coact::LogicalPrio logical_prio() { return 11U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using CtxHsm = coact::Hsm<Ctx>;
using DirectAo = coact::Ao<Ctx, CtxHsm, DirectTraits>;
using StagedAo = coact::Ao<Ctx, CtxHsm, StagedTraits>;

struct Cfg {
    enum : uint8_t {
        kMaxAo = 2U,
        kMaxStateDepth = 6U,
        kMaxDirectDepth = 4U,
        kBatchSizeMax = 4U
    };
    enum : uint16_t {
        kHighCapacity = 4U,
        kNormalCapacity = 8U,
        kLowCapacity = 8U,
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
    /* S7 reads the rejection ring, so this Config opts into 8 slots. A Config
     * without the field gets an empty ring and no storage at all. */
    static constexpr uint8_t kAdmissionSnapshotSlots = 8U;
};

/* Same capacities as Cfg, no ring opt-in. S9 measures the two side by side so
 * the snapshot storage is a declared cost instead of a hidden one. */
struct NoSnapCfg {
    enum : uint8_t {
        kMaxAo = 2U,
        kMaxStateDepth = 6U,
        kMaxDirectDepth = 4U,
        kBatchSizeMax = 4U
    };
    enum : uint16_t {
        kHighCapacity = 4U,
        kNormalCapacity = 8U,
        kLowCapacity = 8U,
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

using StageT = coact::Staging<Cfg, coact::SingleCoreCriticalRing>;
using RegistryT = coact::AoRegistry<Cfg>;
using MonitorT = coact::Monitor<Cfg>;
using BreakerT = coact::Breaker<Cfg>;
using BareCoord = coact::DispatchCoordinator<StageT, harness::BarePal, BreakerT>;

coact::CriticalSection noop_cs() noexcept
{
    return coact::CriticalSection{
        nullptr,
        [](void*) -> coact::CriticalSection::Token { return 0U; },
        [](void*, coact::CriticalSection::Token) {}};
}

coact::Event make_event(uint16_t signal) noexcept
{
    coact::Event e{};
    e.signal = signal;
    e.pool_id = 0U;
    e.ref_ctr = 0U;
    return e;
}

// ---------------------------------------------------------------------------
// Baseline cases: exercise the submit pipeline that exists at 73fe452.
// ---------------------------------------------------------------------------
void case_baseline_direct()
{
    StageT staging(noop_cs());
    RegistryT registry;
    MonitorT monitor;
    Cfg cfg{};
    BreakerT breaker(cfg);
    harness::BarePal pal;
    BareCoord coord(staging, registry, monitor, breaker, pal);

    DirectAo ao(kStates, 2U, kTransitions, 1U, 1, 4U);
    ao.init(make_event(0U));
    record(registry.bind(&ao, ao.logical_prio()), "baseline.direct.bind");

    coact::Event e = make_event(1U);
    harness::set_time_ns(0U);
    const coact::EventQos qos{false, false};
    const coact::SubmitResult r =
        coord.submit_from_task(coact::TargetId(1U), &e, qos);

    record_disposition("baseline.direct.disposition", r.disposition,
                       coact::SubmitDisposition::Direct);
    record_u64("baseline.direct.action_count",
               static_cast<uint64_t>(ao.context().actions), 1U);
    record_u64("baseline.direct.pending",
               static_cast<uint64_t>(ao.pending().load()), 0U);
}

void case_baseline_staged()
{
    StageT staging(noop_cs());
    RegistryT registry;
    MonitorT monitor;
    Cfg cfg{};
    BreakerT breaker(cfg);
    harness::BarePal pal;
    BareCoord coord(staging, registry, monitor, breaker, pal);

    StagedAo ao(kStates, 2U, kTransitions, 1U, 1, 4U);
    ao.init(make_event(0U));
    record(registry.bind(&ao, ao.logical_prio()), "baseline.staged.bind");

    coact::Event e = make_event(1U);
    harness::set_time_ns(0U);
    const coact::EventQos qos{false, false};
    const coact::SubmitResult r =
        coord.submit_from_task(coact::TargetId(1U), &e, qos);

    record_disposition("baseline.staged.disposition", r.disposition,
                       coact::SubmitDisposition::Queued);
    record_u64("baseline.staged.pending",
               static_cast<uint64_t>(ao.pending().load()), 1U);

    coact::StagingSlot slot{};
    const bool got = staging.dequeue_one(slot, 0U);
    record(got, "baseline.staged.dequeue");
    record_u64("baseline.staged.slot_target",
               static_cast<uint64_t>(slot.target.raw()), 1U);
    record(slot.event == &e, "baseline.staged.slot_event");

    ao.dispatch(*slot.event);
    record_u64("baseline.staged.dispatch_action_count",
               static_cast<uint64_t>(ao.context().actions), 1U);
    coact::event_gc(slot.event);
}

coact::PolicyResult baseline_policy_eval(void* ctx, coact::TargetId,
                                         const coact::Event&,
                                         const coact::EventQos&, uint64_t now)
{
    auto* limiter = static_cast<coact::TokenBucketRateLimiter*>(ctx);
    if (limiter->acquire(now)) {
        return {true, false, coact::kReasonOk};
    }
    return {false, false, coact::kReasonRateLimit};
}

void case_baseline_policy()
{
    StageT staging(noop_cs());
    RegistryT registry;
    MonitorT monitor;
    Cfg cfg{};
    BreakerT breaker(cfg);
    harness::BarePal pal;

    coact::TokenBucketRateLimiter limiter;
    limiter.init({1U, 1U, 1000000000ULL, true}, 0U);
    const coact::PolicyOps ops{baseline_policy_eval, nullptr};
    BareCoord coord(staging, registry, monitor, breaker, pal, &ops, &limiter);

    StagedAo ao(kStates, 2U, kTransitions, 1U, 1, 4U);
    ao.init(make_event(0U));
    record(registry.bind(&ao, ao.logical_prio()), "baseline.policy.bind");

    coact::EventQos qos{false, false};
    coact::Event first = make_event(1U);
    coact::Event second = make_event(1U);
    harness::set_time_ns(0U);

    const coact::SubmitResult r1 =
        coord.submit_from_task(coact::TargetId(1U), &first, qos);
    const coact::SubmitResult r2 =
        coord.submit_from_task(coact::TargetId(1U), &second, qos);

    record_disposition("baseline.policy.first", r1.disposition,
                       coact::SubmitDisposition::Queued);
    record_disposition("baseline.policy.second", r2.disposition,
                       coact::SubmitDisposition::DroppedRateLimit);
    record_u64("baseline.policy.second_reason",
               static_cast<uint64_t>(r2.reason),
               static_cast<uint64_t>(coact::kReasonRateLimit));
}

#ifdef COACT_HAS_ADMISSION

// ---------------------------------------------------------------------------
// Compile-time rule sets (design 4.1/4.2). On target a rule set is a type, so
// the tables below are .rodata constants resolved by an O(1) compile-time
// index, and an invalid one cannot be written at all: StaticAdmission rejects
// it where it is declared. The five test_admission_*_neg ctest targets are the
// gate that proves the rejection.
// ---------------------------------------------------------------------------
struct IntervalRules {   /* target 1: at least 1000 ns between arrivals */
    static constexpr auto kRules = coact::make_rules(
        coact::TargetAdmission{coact::TargetId(1U), coact::MinArrivalNs{1000U},
                               coact::MaxBacklog{0U}});
};

struct CapRules {        /* target 1: at most two queued events */
    static constexpr auto kRules = coact::make_rules(
        coact::TargetAdmission{coact::TargetId(1U), coact::MinArrivalNs{0U},
                               coact::MaxBacklog{2U}});
};

struct IntervalAndCapRules {   /* both limits on one target at once */
    static constexpr auto kRules = coact::make_rules(
        coact::TargetAdmission{coact::TargetId(1U), coact::MinArrivalNs{1000U},
                               coact::MaxBacklog{1U}});
};

struct TwoTargetRules {  /* target 1 limited, target 2 present but unlimited */
    static constexpr auto kRules = coact::make_rules(
        coact::TargetAdmission{coact::TargetId(1U), coact::MinArrivalNs{1000U},
                               coact::MaxBacklog{1U}},
        coact::TargetAdmission{coact::TargetId(2U), coact::MinArrivalNs{0U},
                               coact::MaxBacklog{0U}});
};

/* S11: two targets with the same interval and no backlog cap, so a burst that
   is spread over targets must be admitted per target instead of being charged
   to one shared window. */
struct BurstRules {
    static constexpr auto kRules = coact::make_rules(
        coact::TargetAdmission{coact::TargetId(1U), coact::MinArrivalNs{1000U},
                               coact::MaxBacklog{0U}},
        coact::TargetAdmission{coact::TargetId(2U), coact::MinArrivalNs{1000U},
                               coact::MaxBacklog{0U}});
};

struct EmptyRules {      /* policy enabled, no rules at all */
    static constexpr auto kRules = coact::make_rules();
};

struct ZeroRules {       /* policy enabled, one row that limits nothing */
    static constexpr auto kRules = coact::make_rules(
        coact::TargetAdmission{coact::TargetId(1U), coact::MinArrivalNs{0U},
                               coact::MaxBacklog{0U}});
};

/* S6 needs a second AO with its own logical priority: the registry refuses a
 * duplicate priority because two AOs would claim one scheduler slot. */
struct SecondStagedTraits {
    static coact::LogicalPrio logical_prio() { return 12U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
using SecondStagedAo = coact::Ao<Ctx, CtxHsm, SecondStagedTraits>;

/* S12/S13 drive the overload guard through the coordinator's own budget check:
 * `elapsed` is measured with the PAL clock, and the fixture advances that clock
 * inside the action, so one dispatch is enough to overrun Cfg::kRtcBudgetNs
 * without depending on Renode's instruction timing. */
constexpr uint64_t kSlowActionNs = 2000000ULL;   /* > kRtcBudgetNs = 1 ms */

struct SlowCtx {
    int actions = 0;
};

void slow_entry(SlowCtx&) {}
void slow_exit(SlowCtx&) {}
bool slow_guard(const SlowCtx&, const coact::Event&) { return true; }
void slow_action(SlowCtx& ctx, const coact::Event&)
{
    ++ctx.actions;
    harness::advance_time_ns(kSlowActionNs);
}

const coact::StateDef<SlowCtx> kSlowStates[] = {
    {-1, nullptr, nullptr},
    {0, slow_entry, slow_exit},
};

const coact::TransitionDef<SlowCtx> kSlowTransitions[] = {
    {1, 1U, 1, coact::TransitionKind::Internal, slow_guard, slow_action},
};

using SlowHsm = coact::Hsm<SlowCtx>;

struct SlowDirectTraits {          /* overruns its budget, direct eligible */
    static coact::LogicalPrio logical_prio() { return 10U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return true; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

struct FastDirectTraits {          /* stays inside its budget */
    static coact::LogicalPrio logical_prio() { return 13U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return true; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using SlowDirectAo = coact::Ao<SlowCtx, SlowHsm, SlowDirectTraits>;
using FastDirectAo = coact::Ao<Ctx, CtxHsm, FastDirectTraits>;

template <typename Rules>
using AdmCoord = coact::DispatchCoordinator<
    StageT, harness::BarePal, BreakerT, coact::StaticAdmission<Rules, Cfg, coact::AdmissionIrqSync>>;

/* Per-AO overload guard: the product deployment routes breaker inputs by
 * TargetId, so escalation and recovery are per AO rather than global. */
using BankT = coact::BreakerBank<Cfg>;
using BankCoord = coact::DispatchCoordinator<StageT, harness::BarePal, BankT>;
using BankAdmCoord = coact::DispatchCoordinator<
    StageT, harness::BarePal, BankT, coact::StaticAdmission<BurstRules, Cfg, coact::AdmissionIrqSync>>;

/* S8 keeps every admitted event queued (no Dispatcher runs in the fixture),
 * so it needs a deeper Normal lane than Cfg provides. */
struct IsrCfg {
    enum : uint8_t {
        kMaxAo = 2U,
        kMaxStateDepth = 6U,
        kMaxDirectDepth = 4U,
        kBatchSizeMax = 4U
    };
    enum : uint16_t {
        kHighCapacity = 4U,
        kNormalCapacity = 32U,
        kLowCapacity = 8U,
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
    static constexpr uint8_t kAdmissionSnapshotSlots = 8U;
};
using IsrStageT = coact::Staging<IsrCfg, coact::SingleCoreCriticalRing>;
using IsrRegistryT = coact::AoRegistry<IsrCfg>;
using IsrMonitorT = coact::Monitor<IsrCfg>;
using IsrBreakerT = coact::Breaker<IsrCfg>;
using IsrCoord = coact::DispatchCoordinator<
    IsrStageT, harness::BarePal, IsrBreakerT,
    coact::StaticAdmission<IntervalRules, IsrCfg, coact::AdmissionIrqSync>>;

// ---------------------------------------------------------------------------
// Real single-core critical section: PRIMASK save/restore. The scenarios that
// run without interrupts keep using noop_cs(); S8 needs the real thing because
// an interrupt may land inside a staging enqueue.
// ---------------------------------------------------------------------------
coact::CriticalSection irq_cs() noexcept
{
    return coact::CriticalSection{
        nullptr,
        [](void*) -> coact::CriticalSection::Token {
            uint32_t primask = 0U;
            __asm__ volatile("mrs %0, primask\n\tcpsid i"
                             : "=r"(primask) : : "memory");
            return static_cast<coact::CriticalSection::Token>(primask);
        },
        [](void*, coact::CriticalSection::Token token) {
            __asm__ volatile("msr primask, %0"
                             : : "r"(static_cast<uint32_t>(token)) : "memory");
        }};
}

// ---------------------------------------------------------------------------
// Timer2 / NVIC access. stm32f4.repl wires timer2 (10 MHz, Timers.STM32_Timer)
// to nvic@28, i.e. external IRQ 28 and vector 16 + 28 = 44. This model does not
// implement SysTick (writes to 0xE000E010 are logged as unhandled), so timer2
// is the asynchronous edge S8 uses.
// ---------------------------------------------------------------------------
namespace hw {

constexpr uintptr_t kTimer2Base = 0x40000000UL;
constexpr uint32_t kTimer2Irq = 28U;
constexpr uint32_t kTimer2Vector = 16U + kTimer2Irq;
constexpr uint32_t kCr1 = 0x00U / 4U;
constexpr uint32_t kDier = 0x0CU / 4U;
constexpr uint32_t kSr = 0x10U / 4U;
constexpr uint32_t kPsc = 0x28U / 4U;
constexpr uint32_t kArr = 0x2CU / 4U;
constexpr uintptr_t kNvicIser = 0xE000E100UL;
constexpr uintptr_t kNvicIcer = 0xE000E180UL;

inline volatile uint32_t* timer2(uint32_t index) noexcept
{
    return reinterpret_cast<volatile uint32_t*>(kTimer2Base) + index;
}

inline void timer2_start(uint32_t period_counts) noexcept
{
    *timer2(kArr) = period_counts;
    *timer2(kPsc) = 0U;
    *timer2(kSr) = 0U;
    *timer2(kDier) = 1U;   /* UIE */
    *timer2(kCr1) = 1U;    /* CEN */
}

inline void timer2_stop() noexcept
{
    *timer2(kCr1) = 0U;
    *timer2(kDier) = 0U;
    *timer2(kSr) = 0U;
}

inline void nvic_enable(uint32_t irq) noexcept
{
    volatile uint32_t* iser =
        reinterpret_cast<volatile uint32_t*>(kNvicIser) + (irq / 32U);
    *iser = 1U << (irq % 32U);
}

inline void nvic_disable(uint32_t irq) noexcept
{
    volatile uint32_t* icer =
        reinterpret_cast<volatile uint32_t*>(kNvicIcer) + (irq / 32U);
    *icer = 1U << (irq % 32U);
}

/* 0 in thread mode, 16 + IRQ number in handler mode. */
inline uint32_t ipsr() noexcept
{
    uint32_t value = 0U;
    __asm__ volatile("mrs %0, ipsr" : "=r"(value));
    return value;
}

}  // namespace hw

// ---------------------------------------------------------------------------
// S8 handler state. The handler never writes to the UART: it only counts, and
// every assertion is evaluated in thread mode after the timer is stopped.
// ---------------------------------------------------------------------------
using IsrSubmitFn = coact::SubmitResult (*)(void*, coact::TargetId,
                                            coact::Event*,
                                            const coact::EventQos&);

template <typename Coord>
coact::SubmitResult isr_submit_thunk(void* coord, coact::TargetId target,
                                     coact::Event* e,
                                     const coact::EventQos& qos)
{
    return static_cast<Coord*>(coord)->try_submit_from_isr(target, e, qos);
}

struct IsrContext {
    IsrSubmitFn submit;
    void* coord;
    coact::Event* events;
    uint32_t event_capacity;
    uint64_t step_ns;
    bool armed;
    volatile uint32_t invocations;
    volatile uint32_t accepted;
    volatile uint32_t rejected_interval;
    volatile uint32_t rejected_other;
    volatile uint32_t handler_ipsr;

    void reset() noexcept
    {
        submit = nullptr;
        coord = nullptr;
        events = nullptr;
        event_capacity = 0U;
        step_ns = 0U;
        armed = false;
        invocations = 0U;
        accepted = 0U;
        rejected_interval = 0U;
        rejected_other = 0U;
        handler_ipsr = 0U;
    }
};

IsrContext g_isr_ctx;

extern "C" void Timer2_IRQHandler(void)
{
    *hw::timer2(hw::kSr) = 0U;   /* clear UIF before anything else */
    IsrContext& ctx = g_isr_ctx;
    if (!ctx.armed) {
        return;
    }
    ++ctx.invocations;
    ctx.handler_ipsr = hw::ipsr();
    /* One tick == exactly one minimum interval, so the admission bound below
       is arithmetic, not a timing guess. */
    harness::advance_time_ns(ctx.step_ns);
    const uint32_t index = ctx.invocations - 1U;
    if ((nullptr == ctx.submit) || (index >= ctx.event_capacity)) {
        ++ctx.rejected_other;
        return;
    }
    const coact::EventQos qos{false, false};
    const coact::SubmitResult r =
        ctx.submit(ctx.coord, coact::TargetId(1U), &ctx.events[index], qos);
    if (coact::SubmitDisposition::Queued == r.disposition) {
        ++ctx.accepted;
    }
    else if ((coact::SubmitDisposition::DroppedRateLimit == r.disposition)
             && (static_cast<uint16_t>(coact::kArrivalTooSoon) == r.reason)) {
        ++ctx.rejected_interval;
    }
    else {
        ++ctx.rejected_other;
    }
}

// ---------------------------------------------------------------------------
// S1: minimum arrival interval. min = 1000 ns.
//   t=0    admit
//   t=999  reject DroppedRateLimit / reason kArrivalTooSoon
//   t=1000 admit (the boundary is inclusive)
// ---------------------------------------------------------------------------
void case_s1_interval()
{
    StageT staging(noop_cs());
    RegistryT registry;
    MonitorT monitor;
    Cfg cfg{};
    BreakerT breaker(cfg);
    harness::BarePal pal;
    AdmCoord<IntervalRules> coord(staging, registry, monitor, breaker, pal);

    StagedAo ao(kStates, 2U, kTransitions, 1U, 1, 4U);
    ao.init(make_event(0U));
    record(registry.bind(&ao, ao.logical_prio()), "S1.bind");

    const coact::EventQos qos{false, false};
    coact::Event e0 = make_event(1U);
    coact::Event e1 = make_event(1U);
    coact::Event e2 = make_event(1U);

    harness::set_time_ns(0U);
    const coact::SubmitResult r0 =
        coord.submit_from_task(coact::TargetId(1U), &e0, qos);
    harness::set_time_ns(999U);
    const coact::SubmitResult r1 =
        coord.submit_from_task(coact::TargetId(1U), &e1, qos);
    harness::set_time_ns(1000U);
    const coact::SubmitResult r2 =
        coord.submit_from_task(coact::TargetId(1U), &e2, qos);

    record_disposition("S1.t0_admit", r0.disposition,
                       coact::SubmitDisposition::Queued);
    record_disposition("S1.t999_reject", r1.disposition,
                       coact::SubmitDisposition::DroppedRateLimit);
    record_u64("S1.t999_reason", static_cast<uint64_t>(r1.reason),
               static_cast<uint64_t>(coact::AdmissionReason::kArrivalTooSoon));
    record_disposition("S1.t1000_admit", r2.disposition,
                       coact::SubmitDisposition::Queued);
    record_u64("S1.pending", static_cast<uint64_t>(ao.pending().load()), 2U);
    record_u64("S1.rejection_counter",
               monitor.ao(coact::TargetId(1U))
                   .rejections[static_cast<size_t>(
                       coact::RejectReason::kC8ArrivalInterval)]
                   .load(),
               1U);
}

// ---------------------------------------------------------------------------
// S2: a rejection must not consume the arrival window. Asserted twice: on a
// bare ArrivalGate, and end-to-end through the coordinator's own gate, which
// is readable now that the policy is a member the coordinator exposes.
// ---------------------------------------------------------------------------
void case_s2_reject_keeps_window()
{
    /* The bare gate runs under the same PRIMASK critical section the policy
     * binds through AdmissionIrqSync, so this is the production
     * synchronization path rather than a mock. */
    const coact::CriticalSection cs = irq_cs();
    coact::ArrivalGate gate;
    const coact::MinArrivalNs min_ns{1000U};

    record(gate.try_reserve(cs, 0U, min_ns), "S2.gate.first_reserve");
    record(!gate.admitted_ever(cs), "S2.gate.reserve_is_not_admit");
    gate.commit(cs, 0U);
    record(gate.admitted_ever(cs), "S2.gate.admitted_ever");
    record_u64("S2.gate.last_after_first", gate.last_admit_ns(cs), 0U);
    record(!gate.try_reserve(cs, 999U, min_ns), "S2.gate.reject_at_999");
    record_u64("S2.gate.last_unchanged_on_reject", gate.last_admit_ns(cs), 0U);
    record(gate.try_reserve(cs, 1000U, min_ns), "S2.gate.admit_at_1000");
    gate.commit(cs, 1000U);
    record_u64("S2.gate.last_after_second", gate.last_admit_ns(cs), 1000U);

    /* A submission that fails after reserving leaves the committed window
     * alone: no stale timestamp, no leftover hold. */
    record(gate.try_reserve(cs, 2000U, min_ns), "S2.gate.reserve_at_2000");
    record_u64("S2.gate.holds_while_reserved", gate.holds(cs), 1U);
    gate.rollback(cs);
    record_u64("S2.gate.last_after_rollback", gate.last_admit_ns(cs), 1000U);
    record_u64("S2.gate.holds_after_rollback", gate.holds(cs), 0U);

    StageT staging(noop_cs());
    RegistryT registry;
    MonitorT monitor;
    Cfg cfg{};
    BreakerT breaker(cfg);
    harness::BarePal pal;
    AdmCoord<IntervalRules> coord(staging, registry, monitor, breaker, pal);

    StagedAo ao(kStates, 2U, kTransitions, 1U, 1, 4U);
    ao.init(make_event(0U));
    record(registry.bind(&ao, ao.logical_prio()), "S2.bind");

    const coact::TargetId t1(1U);
    const coact::EventQos qos{false, false};
    coact::Event e0 = make_event(1U);
    coact::Event e1 = make_event(1U);
    coact::Event e2 = make_event(1U);

    harness::set_time_ns(0U);
    const coact::SubmitResult r0 = coord.submit_from_task(t1, &e0, qos);
    record_u64("S2.coord.window_after_admit",
               coord.admission_policy().last_admit_ns(t1), 0U);

    harness::set_time_ns(999U);
    const coact::SubmitResult r1 = coord.submit_from_task(t1, &e1, qos);
    /* The rejection left the coordinator's own gate untouched. */
    record_u64("S2.coord.window_unchanged_after_reject",
               coord.admission_policy().last_admit_ns(t1), 0U);

    harness::set_time_ns(1000U);
    const coact::SubmitResult r2 = coord.submit_from_task(t1, &e2, qos);

    record_disposition("S2.t0_admit", r0.disposition,
                       coact::SubmitDisposition::Queued);
    record_disposition("S2.t999_reject", r1.disposition,
                       coact::SubmitDisposition::DroppedRateLimit);
    record_disposition("S2.t1000_admit", r2.disposition,
                       coact::SubmitDisposition::Queued);
    record_u64("S2.coord.window_after_second_admit",
               coord.admission_policy().last_admit_ns(t1), 1000U);
    record(coord.admission_policy().admitted_ever(t1),
           "S2.coord.admitted_ever");
}

// ---------------------------------------------------------------------------
// S3: enqueue failure rolls the arrival window back. The AO is not
// direct-eligible, so every submission goes through staging; the Normal ring
// (capacity 8) is saturated, then one slot is drained and the retry at the
// same timestamp must be admitted. Without the rollback the retry would see a
// zero-length interval and be rejected.
// ---------------------------------------------------------------------------
void case_s3_enqueue_rollback()
{
    StageT staging(noop_cs());
    RegistryT registry;
    MonitorT monitor;
    Cfg cfg{};
    BreakerT breaker(cfg);
    harness::BarePal pal;
    AdmCoord<IntervalRules> coord(staging, registry, monitor, breaker, pal);

    StagedAo ao(kStates, 2U, kTransitions, 1U, 1, 4U);
    ao.init(make_event(0U));
    record(registry.bind(&ao, ao.logical_prio()), "S3.bind");

    const coact::TargetId t1(1U);
    const coact::EventQos qos{false, false};
    coact::Event fill[8];
    bool all_queued = true;
    for (uint32_t i = 0U; i < 8U; ++i) {
        fill[i] = make_event(1U);
        harness::set_time_ns(static_cast<uint64_t>(i) * 1000U);
        const coact::SubmitResult r = coord.submit_from_task(t1, &fill[i], qos);
        if (coact::SubmitDisposition::Queued != r.disposition) {
            all_queued = false;
        }
    }
    record(all_queued, "S3.fill_all_queued");
    record_u64("S3.window_after_fill",
               coord.admission_policy().last_admit_ns(t1), 7000U);

    coact::Event overflow = make_event(1U);
    harness::set_time_ns(8000U);
    const coact::SubmitResult rfull = coord.submit_from_task(t1, &overflow, qos);
    record_disposition("S3.enqueue_failure", rfull.disposition,
                       coact::SubmitDisposition::RejectedFull);
    record_u64("S3.enqueue_failure_reason",
               static_cast<uint64_t>(rfull.reason), 0U);
    record_u64("S3.pending_after_failure",
               static_cast<uint64_t>(ao.pending().load()), 8U);
    /* The failed enqueue handed the window back: it is still at 7000 ns, not
       at the 8000 ns this submission arrived with. */
    record_u64("S3.window_rolled_back",
               coord.admission_policy().last_admit_ns(t1), 7000U);
    /* The overflow is recorded in the ring as a staging-lane rejection while
       SubmitResult::reason keeps its historical 0. */
    const auto overflow_rec = monitor.admission_snapshot(
        monitor.admission_snapshot_count());
    record(static_cast<bool>(overflow_rec), "S3.overflow_snapshot_present");
    if (overflow_rec) {
        record_u64("S3.overflow_snapshot_reason",
                   static_cast<uint64_t>(overflow_rec.value().reason),
                   static_cast<uint64_t>(coact::AdmissionReason::kStagingFull));
        record_u64("S3.overflow_snapshot_time",
                   overflow_rec.value().now_ns, 8000U);
    }

    coact::StagingSlot drained{};
    const bool drained_ok = staging.dequeue_one(drained, 8000U);
    record(drained_ok, "S3.drain_one");
    if (drained_ok) {
        coact::event_gc(drained.event);
    }

    coact::Event retry = make_event(1U);
    /* The failed enqueue tripped the breaker to L2 (on_overflow). A critical
       submission bypasses that guard, so this retry exercises only the arrival
       window: with the rollback in place the interval since 7000 ns is exactly
       1000 ns and the submission is admitted. */
    const coact::EventQos retry_qos{true, false};
    const coact::SubmitResult rretry = coord.submit_from_task(t1, &retry, retry_qos);
    record_disposition("S3.retry_same_ts_admitted", rretry.disposition,
                       coact::SubmitDisposition::Queued);
    record_u64("S3.window_after_retry",
               coord.admission_policy().last_admit_ns(t1), 8000U);
}

// ---------------------------------------------------------------------------
// S4: backlog cap. max_backlog = 2 and the Normal ring has room for 8, so the
// third rejection is the cap, not the queue. A critical event does not bypass.
// ---------------------------------------------------------------------------
void case_s4_backlog_cap()
{
    StageT staging(noop_cs());
    RegistryT registry;
    MonitorT monitor;
    Cfg cfg{};
    BreakerT breaker(cfg);
    harness::BarePal pal;
    AdmCoord<CapRules> coord(staging, registry, monitor, breaker, pal);

    StagedAo ao(kStates, 2U, kTransitions, 1U, 1, 4U);
    ao.init(make_event(0U));
    record(registry.bind(&ao, ao.logical_prio()), "S4.bind");

    const coact::EventQos ordinary{false, false};
    coact::Event e0 = make_event(1U);
    coact::Event e1 = make_event(1U);
    coact::Event e2 = make_event(1U);
    harness::set_time_ns(0U);

    const coact::SubmitResult r0 =
        coord.submit_from_task(coact::TargetId(1U), &e0, ordinary);
    const coact::SubmitResult r1 =
        coord.submit_from_task(coact::TargetId(1U), &e1, ordinary);
    const coact::SubmitResult r2 =
        coord.submit_from_task(coact::TargetId(1U), &e2, ordinary);

    record_disposition("S4.first_queued", r0.disposition,
                       coact::SubmitDisposition::Queued);
    record_disposition("S4.second_queued", r1.disposition,
                       coact::SubmitDisposition::Queued);
    record_disposition("S4.third_rejected", r2.disposition,
                       coact::SubmitDisposition::RejectedFull);
    record_u64("S4.third_reason", static_cast<uint64_t>(r2.reason),
               static_cast<uint64_t>(coact::AdmissionReason::kBacklogFull));
    record_u64("S4.pending_stays_two",
               static_cast<uint64_t>(ao.pending().load()), 2U);
    record_u64("S4.no_overflow_accounting",
               monitor.global().overflow.load(), 0U);
    record_u64("S4.breaker_untouched",
               static_cast<uint64_t>(static_cast<uint8_t>(breaker.level())),
               static_cast<uint64_t>(static_cast<uint8_t>(
                   coact::BreakerLevel::Normal)));

    const coact::EventQos critical{true, false};
    coact::Event ecrit = make_event(1U);
    const coact::SubmitResult rcrit =
        coord.submit_from_task(coact::TargetId(1U), &ecrit, critical);
    record_disposition("S4.critical_not_bypass", rcrit.disposition,
                       coact::SubmitDisposition::RejectedFull);
    record_u64("S4.critical_reason", static_cast<uint64_t>(rcrit.reason),
               static_cast<uint64_t>(coact::AdmissionReason::kBacklogFull));
    record_u64("S4.pending_after_critical",
               static_cast<uint64_t>(ao.pending().load()), 2U);
}

// ---------------------------------------------------------------------------
// S5: default-off invariance. Three different policies must produce the same
// eight rapid submissions: NoAdmission (the default template argument, the
// whole mechanism compiled out), an enabled policy with an empty rule set, and
// an enabled policy whose row sets both limits to zero.
// ---------------------------------------------------------------------------
template <typename Coord>
uint32_t run_rapid_eight(Coord& coord, StagedAo& ao, RegistryT& registry,
                         const char* bind_name, coact::SubmitDisposition* out)
{
    record(registry.bind(&ao, ao.logical_prio()), bind_name);
    const coact::EventQos qos{false, false};
    coact::Event events[8];
    uint32_t queued = 0U;
    harness::set_time_ns(0U);
    for (uint32_t i = 0U; i < 8U; ++i) {
        events[i] = make_event(1U);
        const coact::SubmitResult r =
            coord.submit_from_task(coact::TargetId(1U), &events[i], qos);
        out[i] = r.disposition;
        if (coact::SubmitDisposition::Queued == r.disposition) {
            ++queued;
        }
    }
    return queued;
}

void case_s5_default_off()
{
    coact::SubmitDisposition off[8]{};
    coact::SubmitDisposition empty[8]{};
    coact::SubmitDisposition zero[8]{};
    uint32_t queued = 0U;

    {
        StageT staging(noop_cs());
        RegistryT registry;
        MonitorT monitor;
        Cfg cfg{};
        BreakerT breaker(cfg);
        harness::BarePal pal;
        BareCoord coord(staging, registry, monitor, breaker, pal);
        StagedAo ao(kStates, 2U, kTransitions, 1U, 1, 4U);
        ao.init(make_event(0U));
        queued = run_rapid_eight(coord, ao, registry, "S5.bind_off", off);
        record_u64("S5.default_off_snapshot_count",
                   monitor.admission_snapshot_count(), 0U);
    }
    {
        StageT staging(noop_cs());
        RegistryT registry;
        MonitorT monitor;
        Cfg cfg{};
        BreakerT breaker(cfg);
        harness::BarePal pal;
        AdmCoord<EmptyRules> coord(staging, registry, monitor, breaker, pal);
        StagedAo ao(kStates, 2U, kTransitions, 1U, 1, 4U);
        ao.init(make_event(0U));
        record_u64("S5.empty_rules_queued",
                   run_rapid_eight(coord, ao, registry, "S5.bind_empty", empty), queued);
    }
    {
        StageT staging(noop_cs());
        RegistryT registry;
        MonitorT monitor;
        Cfg cfg{};
        BreakerT breaker(cfg);
        harness::BarePal pal;
        AdmCoord<ZeroRules> coord(staging, registry, monitor, breaker, pal);
        StagedAo ao(kStates, 2U, kTransitions, 1U, 1, 4U);
        ao.init(make_event(0U));
        record_u64("S5.zero_limits_queued",
                   run_rapid_eight(coord, ao, registry, "S5.bind_zero", zero), queued);
    }

    record_u64("S5.eight_rapid_queued", queued, 8U);
    bool identical = true;
    for (uint32_t i = 0U; i < 8U; ++i) {
        if ((off[i] != empty[i]) || (off[i] != zero[i])) {
            identical = false;
        }
    }
    record(identical, "S5.dispositions_identical");
    bool all_queued = true;
    for (uint32_t i = 0U; i < 8U; ++i) {
        if (coact::SubmitDisposition::Queued != off[i]) {
            all_queued = false;
        }
    }
    record(all_queued, "S5.all_queued");
}

// ---------------------------------------------------------------------------
// S6: cross-target isolation. Target 1 carries an interval and a cap, target 2
// is listed with both limits off: a rejection on target 1 must not delay or
// block target 2, and the compile-time index must resolve each of them.
// ---------------------------------------------------------------------------
void case_s6_cross_target()
{
    StageT staging(noop_cs());
    RegistryT registry;
    MonitorT monitor;
    Cfg cfg{};
    BreakerT breaker(cfg);
    harness::BarePal pal;
    AdmCoord<TwoTargetRules> coord(staging, registry, monitor, breaker, pal);

    StagedAo gated(kStates, 2U, kTransitions, 1U, 1, 4U);
    SecondStagedAo unlimited_ao(kStates, 2U, kTransitions, 1U, 1, 4U);
    gated.init(make_event(0U));
    unlimited_ao.init(make_event(0U));
    const coact::TargetId t1(1U);
    const coact::TargetId t2(2U);
    record(registry.bind_at(t1, gated, gated.logical_prio()), "S6.bind_t1");
    record(registry.bind_at(t2, unlimited_ao, unlimited_ao.logical_prio()), "S6.bind_t2");

    /* The limits each target actually resolves to. */
    record_u64("S6.t1_min_arrival",
               coord.admission_policy().limits_for(t1).min_arrival.value, 1000U);
    record_u64("S6.t1_max_backlog",
               coord.admission_policy().backlog_cap(t1), 1U);
    record_u64("S6.t2_min_arrival",
               coord.admission_policy().limits_for(t2).min_arrival.value, 0U);
    record_u64("S6.t2_max_backlog",
               coord.admission_policy().backlog_cap(t2), 0U);

    const coact::EventQos qos{false, false};
    harness::set_time_ns(0U);
    coact::Event g0 = make_event(1U);
    record_disposition("S6.t1_first_admit",
                       coord.submit_from_task(t1, &g0, qos).disposition,
                       coact::SubmitDisposition::Queued);

    /* Target 1 is now both interval-blocked and at its cap. Target 2 must not
       care about either. */
    coact::Event f0 = make_event(1U);
    coact::Event f1 = make_event(1U);
    coact::Event f2 = make_event(1U);
    harness::set_time_ns(10U);
    record_disposition("S6.t2_back_to_back_1",
                       coord.submit_from_task(t2, &f0, qos).disposition,
                       coact::SubmitDisposition::Queued);
    harness::set_time_ns(20U);
    record_disposition("S6.t2_back_to_back_2",
                       coord.submit_from_task(t2, &f1, qos).disposition,
                       coact::SubmitDisposition::Queued);
    harness::set_time_ns(30U);
    record_disposition("S6.t2_back_to_back_3",
                       coord.submit_from_task(t2, &f2, qos).disposition,
                       coact::SubmitDisposition::Queued);

    coact::Event g1 = make_event(1U);
    harness::set_time_ns(999U);
    const coact::SubmitResult r = coord.submit_from_task(t1, &g1, qos);
    record_disposition("S6.t1_still_gated", r.disposition,
                       coact::SubmitDisposition::DroppedRateLimit);
    record_u64("S6.t1_gate_window",
               coord.admission_policy().last_admit_ns(t1), 0U);
    record_u64("S6.t2_gate_untouched",
               coord.admission_policy().admitted_ever(t2) ? 1U : 0U,
               0U);
    record_u64("S6.t2_pending",
               static_cast<uint64_t>(unlimited_ao.pending().load()), 3U);
    record_u64("S6.t1_pending",
               static_cast<uint64_t>(gated.pending().load()), 1U);
}

// ---------------------------------------------------------------------------
// S7: the fixed snapshot ring on target. This is also the only place where the
// IRQ-masked 64-bit atomics in runtime.c carry the diagnostic fields, so the
// Expected read path is exercised on the ARM ABI rather than only on the host.
// ---------------------------------------------------------------------------
void case_s7_snapshot_ring()
{
    using SnapshotError = MonitorT::SnapshotError;

    MonitorT fresh;
    const auto empty = fresh.admission_snapshot(1U);
    record(!empty, "S7.empty_ring_fails");
    record_u64("S7.empty_ring_error",
               static_cast<uint64_t>(static_cast<uint8_t>(empty.error())),
               static_cast<uint64_t>(static_cast<uint8_t>(SnapshotError::kEmpty)));
    record_u64("S7.empty_ring_count", fresh.admission_snapshot_count(), 0U);

    StageT staging(noop_cs());
    RegistryT registry;
    MonitorT monitor;
    Cfg cfg{};
    BreakerT breaker(cfg);
    harness::BarePal pal;
    AdmCoord<IntervalRules> coord(staging, registry, monitor, breaker, pal);

    StagedAo ao(kStates, 2U, kTransitions, 1U, 1, 4U);
    ao.init(make_event(0U));
    record(registry.bind(&ao, ao.logical_prio()), "S7.bind");

    const coact::TargetId t1(1U);
    const coact::EventQos qos{false, false};
    coact::Event first = make_event(1U);
    harness::set_time_ns(0U);
    record_disposition("S7.first_admit",
                       coord.submit_from_task(t1, &first, qos).disposition,
                       coact::SubmitDisposition::Queued);
    record_u64("S7.count_after_admit", monitor.admission_snapshot_count(), 0U);

    /* Nine rejections: the ring holds eight, so seq 1 is evicted. */
    bool nine_rejections = true;
    for (uint64_t now = 1U; now <= 9U; ++now) {
        coact::Event e = make_event(1U);
        harness::set_time_ns(now);
        const coact::SubmitResult r = coord.submit_from_task(t1, &e, qos);
        if (coact::SubmitDisposition::DroppedRateLimit != r.disposition) {
            nine_rejections = false;
            break;
        }
    }
    record(nine_rejections, "S7.nine_rejections");
    if (!nine_rejections) {
        return;
    }
    record_u64("S7.count_after_nine", monitor.admission_snapshot_count(), 9U);

    const auto zero_seq = monitor.admission_snapshot(0U);
    record(!zero_seq, "S7.seq_zero_fails");
    record_u64("S7.seq_zero_error",
               static_cast<uint64_t>(static_cast<uint8_t>(zero_seq.error())),
               static_cast<uint64_t>(
                   static_cast<uint8_t>(SnapshotError::kOutOfRange)));
    const auto beyond = monitor.admission_snapshot(10U);
    record(!beyond, "S7.seq_beyond_fails");
    record_u64("S7.seq_beyond_error",
               static_cast<uint64_t>(static_cast<uint8_t>(beyond.error())),
               static_cast<uint64_t>(
                   static_cast<uint8_t>(SnapshotError::kOutOfRange)));
    const auto evicted = monitor.admission_snapshot(1U);
    record(!evicted && (SnapshotError::kEvicted == evicted.error()),
           "S7.evicted_fails");
    record_u64("S7.evicted_error",
               static_cast<uint64_t>(static_cast<uint8_t>(evicted.error())),
               static_cast<uint64_t>(
                   static_cast<uint8_t>(SnapshotError::kEvicted)));

    bool survivors_ok = true;
    for (uint32_t seq = 2U; seq <= 9U; ++seq) {
        const auto rec = monitor.admission_snapshot(seq);
        if (!rec) {
            survivors_ok = false;
            continue;
        }
        const auto& snap = rec.value();
        if ((snap.seq != seq) || (1U != snap.target)
            || (static_cast<uint8_t>(coact::SubmitDisposition::DroppedRateLimit)
                != snap.disposition)
            || (static_cast<uint16_t>(coact::AdmissionReason::kArrivalTooSoon)
                != snap.reason)
            || (snap.now_ns != seq) || (snap.budget_ns != 1000000U)) {
            survivors_ok = false;
        }
    }
    record(survivors_ok, "S7.eight_survivors_intact");
}

// ---------------------------------------------------------------------------
// S8: a real Cortex-M interrupt submitting through try_submit_from_isr while
// thread mode submits through submit_from_task on the same target. Timer2
// (10 MHz on this model, IRQ 28 -> vector 44) provides the asynchronous edge
// and the handler advances the virtual clock by exactly one minimum interval
// per interrupt, so the admission count is arithmetic rather than a timing
// guess: one admission per tick, plus the first thread submission at t=0.
//
// If the arrival gate were a load-then-store instead of one CAS, a thread
// submission could be admitted for the same tick as the handler and the total
// would exceed that bound. If a rejection advanced the window, a later tick
// would be refused and the total would fall short.
// ---------------------------------------------------------------------------
void case_s8_isr_preemption()
{
    constexpr uint32_t kTicks = 8U;
    /* 2000 counts at 10 MHz == 200 us of virtual time per interrupt. */
    constexpr uint32_t kPeriod = 2000U;
    constexpr uint64_t kStep = 1000U;
    constexpr uint32_t kSpinLimit = 400000U;

    IsrStageT staging(irq_cs());
    IsrRegistryT registry;
    IsrMonitorT monitor;
    IsrCfg cfg{};
    IsrBreakerT breaker(cfg);
    harness::BarePal pal;
    IsrCoord coord(staging, registry, monitor, breaker, pal);

    StagedAo ao(kStates, 2U, kTransitions, 1U, 1, 4U);
    ao.init(make_event(0U));
    record(registry.bind(&ao, ao.logical_prio()), "S8.bind");

    static coact::Event isr_events[kTicks];
    for (uint32_t i = 0U; i < kTicks; ++i) {
        isr_events[i] = make_event(1U);
    }
    IsrContext& ctx = g_isr_ctx;
    ctx.reset();
    ctx.submit = &isr_submit_thunk<IsrCoord>;
    ctx.coord = &coord;
    ctx.events = isr_events;
    ctx.event_capacity = kTicks;
    ctx.step_ns = kStep;

    const coact::TargetId t1(1U);
    const coact::EventQos qos{false, false};

    /* Thread-mode admission at t=0, before any interrupt: the first arrival is
       always admitted and becomes the window every handler must respect. */
    harness::set_time_ns(0U);
    coact::Event thread_first = make_event(1U);
    /* The wake latch starts set (no Dispatcher runs in this fixture), so arm it
       the way a Dispatcher would before an idle wait: the submission that
       follows then owns the PAL signal, which is what makes the task and ISR
       wake paths distinguishable below. */
    staging.arm_dispatcher_wait();
    const coact::SubmitResult r0 = coord.submit_from_task(t1, &thread_first, qos);
    record_disposition("S8.thread_first_admitted", r0.disposition,
                       coact::SubmitDisposition::Queued);
    const uint32_t thread_ipsr = hw::ipsr();

    coact::Event thread_pool[4];
    for (uint32_t i = 0U; i < 4U; ++i) {
        thread_pool[i] = make_event(1U);
    }
    uint32_t thread_accepted = 1U;   /* the t=0 submission above */
    uint32_t thread_rejected_interval = 0U;
    uint32_t thread_rejected_other = 0U;
    uint32_t spins = 0U;

    ctx.armed = true;
    hw::timer2_start(kPeriod);
    hw::nvic_enable(hw::kTimer2Irq);
    __asm__ volatile("dsb\n\tisb");

    while ((ctx.invocations < kTicks) && (spins < kSpinLimit)) {
        ++spins;
        /* Re-arm every spin so each admission signals once, exactly as a
           Dispatcher that consumed the previous wake would. */
        staging.arm_dispatcher_wait();
        if (thread_accepted < 4U) {
            const coact::SubmitResult r =
                coord.submit_from_task(t1, &thread_pool[thread_accepted], qos);
            if (coact::SubmitDisposition::Queued == r.disposition) {
                ++thread_accepted;
            }
            else if ((coact::SubmitDisposition::DroppedRateLimit == r.disposition)
                     && (static_cast<uint16_t>(coact::kArrivalTooSoon)
                         == r.reason)) {
                ++thread_rejected_interval;
            }
            else {
                ++thread_rejected_other;
            }
        }
    }

    hw::nvic_disable(hw::kTimer2Irq);
    hw::timer2_stop();
    __asm__ volatile("dsb\n\tisb");
    ctx.armed = false;

    const uint32_t ticks = ctx.invocations;
    const uint32_t isr_accepted = ctx.accepted;
    record_u64("S8.timer_ticks", ticks, kTicks);
    record_u64("S8.handler_mode_ipsr", ctx.handler_ipsr, hw::kTimer2Vector);
    record_u64("S8.thread_mode_ipsr", thread_ipsr, 0U);
    record_u64("S8.isr_accepted", isr_accepted, kTicks);
    record_u64("S8.isr_rejected_interval", ctx.rejected_interval, 0U);
    record_u64("S8.isr_rejected_other", ctx.rejected_other, 0U);
    record_u64("S8.thread_accepted_once", thread_accepted, 1U);
    record_u64("S8.thread_rejected_other", thread_rejected_other, 0U);
    record(thread_rejected_interval > 0U, "S8.thread_path_exercised");
    /* One admission per tick plus the first thread arrival, whichever context
       got there first: never more, never fewer. */
    record_u64("S8.total_admissions", isr_accepted + thread_accepted,
               kTicks + 1U);
    record_u64("S8.pending_matches_admissions",
               static_cast<uint64_t>(ao.pending().load()),
               static_cast<uint64_t>(kTicks + 1U));
    record_u64("S8.no_overflow", monitor.global().overflow.load(), 0U);
    /* The window sits exactly on the last tick: thousands of rejected thread
       submissions never advanced it, and every handler arrival committed. */
    record_u64("S8.final_window",
               coord.admission_policy().last_admit_ns(t1),
               static_cast<uint64_t>(kTicks) * kStep);
    /* Both contexts must reach their own wake path: a handler that took the
       task path instead of signal_dispatcher_from_isr is the classic bug this
       catches, and it is only observable from a real interrupt. */
    record(pal.isr_signals > 0, "S8.isr_wake_signalled");
    record(pal.task_signals > 0, "S8.task_wake_signalled");
    /* At most one signal per admission, and never more admissions than ticks. */
    record((pal.isr_signals > 0)
               && (static_cast<uint32_t>(pal.isr_signals) <= kTicks),
           "S8.isr_signals_bounded");

    /* Queue integrity after real preemption: the admitted events must come
       back in admission order, each exactly once. A ring corrupted by an
       interrupt landing inside an enqueue cannot pass this. */
    const coact::Event* expected[kTicks + 1U];
    expected[0] = &thread_first;
    for (uint32_t i = 0U; i < kTicks; ++i) {
        expected[i + 1U] = &isr_events[i];
    }
    /* Drain exactly the way a Dispatcher does: open a batch, take up to
       kBatchSizeMax slots, open the next one. batch_used_ only resets in
       begin_batch(), so a flat loop stops at the batch cap and would report a
       truncated queue as corruption. One batch covers kTicks+1 == 9 slots. */
    constexpr uint32_t kBatches = (kTicks + 1U) / IsrCfg::kBatchSizeMax + 1U;
    constexpr uint64_t kDrainStep = 2000000U;
    bool order_ok = true;
    uint32_t drained_count = 0U;
    coact::StagingSlot drained{};
    for (uint32_t b = 0U;
         (b < kBatches) && (drained_count < (kTicks + 1U)); ++b) {
        staging.begin_batch();
        while ((drained_count < (kTicks + 1U))
               && (staging.batch_used() < IsrCfg::kBatchSizeMax)) {
            const uint64_t now =
                9000U + (static_cast<uint64_t>(drained_count) * kDrainStep);
            if (!staging.dequeue_one(drained, now)) {
                break;
            }
            if (drained.event != expected[drained_count]) {
                order_ok = false;
            }
            ++drained_count;
        }
    }
    record(order_ok, "S8.queue_order_intact");
    record_u64("S8.queue_drained", drained_count, kTicks + 1U);
    staging.begin_batch();
    const uint64_t after =
        9000U + (static_cast<uint64_t>(kTicks + 2U) * kDrainStep);
    record(!staging.dequeue_one(drained, after), "S8.queue_empty_after_drain");
    /* Every drained slot released its admission claim, so the queue reports
       zero occupancy and the ring is reusable rather than leaked. */
    record_u64("S8.pending_after_drain",
               static_cast<uint64_t>(
                   staging.size(coact::Partition::High)
                   + staging.size(coact::Partition::Normal)
                   + staging.size(coact::Partition::Low)),
               0U);
    harness::puts("S8.info ticks=");
    harness::put_u64(ticks);
    harness::puts(" spins=");
    harness::put_u64(spins);
    harness::puts(" thread_rejected=");
    harness::put_u64(thread_rejected_interval);
    harness::puts(" isr_signals=");
    harness::put_u64(static_cast<uint64_t>(pal.isr_signals));
    harness::puts(" task_signals=");
    harness::put_u64(static_cast<uint64_t>(pal.task_signals));
    harness::newline();
}

// ---------------------------------------------------------------------------
// S9: footprint, measured on the ARM ABI rather than asserted in prose. The
// disabled policy must add no state at all; the enabled one costs exactly one
// gate word per target. run.sh prints the ELF section sizes for both builds.
// ---------------------------------------------------------------------------
void case_s9_footprint()
{
    /* Four plain fields in 24 bytes: two timestamps, a hold count, a flag.
     * No atomics, so no libatomic call on the submit path. */
    record_u64("S9.gate_size", sizeof(coact::ArrivalGate), 24U);
    record_u64("S9.sync_size", sizeof(coact::AdmissionIrqSync),
               sizeof(coact::CriticalSection));
    /* Gate pointer, critical-section pointer, timestamp: bounded, stack only. */
    record(sizeof(coact::ArrivalReservation) <= 24U,
           "S9.reservation_bounded");
    /* The critical-section hook triple is 12 bytes; the gate array is 8-byte
     * aligned, so the policy pays 4 bytes of padding between them. */
    constexpr std::size_t kSyncStride =
        (sizeof(coact::AdmissionIrqSync) + alignof(coact::ArrivalGate) - 1U)
        / alignof(coact::ArrivalGate) * alignof(coact::ArrivalGate);
    record_u64("S9.policy_size",
               sizeof(coact::StaticAdmission<IntervalRules, Cfg, coact::AdmissionIrqSync>),
               kSyncStride
                   + static_cast<uint64_t>(Cfg::kMaxAo + 1U)
                         * sizeof(coact::ArrivalGate));
    record_u64("S9.no_admission_size", sizeof(coact::NoAdmission), 1U);
    record_u64("S9.inert_reservation_size", sizeof(coact::InertReservation), 1U);
    record(sizeof(AdmCoord<IntervalRules>) >= sizeof(BareCoord),
           "S9.coord_not_smaller");
    /* Exact, not just bounded: the policy is the only member the enabled
       coordinator adds, so an alignment surprise must fail here rather than
       hide behind a <=. */
    record(sizeof(AdmCoord<IntervalRules>) - sizeof(BareCoord)
               == sizeof(coact::StaticAdmission<IntervalRules, Cfg, coact::AdmissionIrqSync>),
           "S9.coord_delta_is_policy");
    /* The rule table is a compile-time constant: no runtime copy exists. */
    record_u64("S9.rule_count",
               coact::StaticAdmission<IntervalRules, Cfg, coact::AdmissionIrqSync>::kRuleCount, 1U);
    record_u64("S9.snapshot_slot_count", MonitorT::kAdmissionSnapshotSlots, 8U);
    /* The ring is opt-in storage, not part of the "disabled costs nothing"
     * claim: this Config asks for 8 slots and pays for them. */
    record(sizeof(coact::Monitor<Cfg>) > sizeof(coact::Monitor<NoSnapCfg>),
           "S9.snapshot_ring_costs_storage");
    record_u64("S9.no_snapshot_slots",
               coact::Monitor<NoSnapCfg>::kAdmissionSnapshotSlots, 0U);

    harness::puts("S9.info plain_coord=");
    harness::put_u64(sizeof(BareCoord));
    harness::puts(" gated_coord=");
    harness::put_u64(sizeof(AdmCoord<IntervalRules>));
    harness::puts(" policy=");
    harness::put_u64(sizeof(coact::StaticAdmission<IntervalRules, Cfg, coact::AdmissionIrqSync>));
    harness::puts(" isr_coord=");
    harness::put_u64(sizeof(IsrCoord));
    harness::puts(" monitor_with_ring=");
    harness::put_u64(sizeof(coact::Monitor<Cfg>));
    harness::puts(" monitor_without_ring=");
    harness::put_u64(sizeof(coact::Monitor<NoSnapCfg>));
    harness::newline();
}

// ---------------------------------------------------------------------------
// S10: both limits on one target. The cap rejects a submission that already
// passed the interval gate, and the RAII reservation must hand the window back
// so the retry after a drain is admitted at the very same timestamp.
// ---------------------------------------------------------------------------
void case_s10_interval_and_cap()
{
    StageT staging(noop_cs());
    RegistryT registry;
    MonitorT monitor;
    Cfg cfg{};
    BreakerT breaker(cfg);
    harness::BarePal pal;
    AdmCoord<IntervalAndCapRules> coord(staging, registry, monitor, breaker, pal);

    StagedAo ao(kStates, 2U, kTransitions, 1U, 1, 4U);
    ao.init(make_event(0U));
    const coact::TargetId t1(1U);
    record(registry.bind(&ao, ao.logical_prio()), "S10.bind");

    const coact::EventQos qos{false, false};
    coact::Event first = make_event(1U);
    harness::set_time_ns(0U);
    record_disposition("S10.first_admit",
                       coord.submit_from_task(t1, &first, qos).disposition,
                       coact::SubmitDisposition::Queued);

    /* t=1000 satisfies the interval but the cap is already reached. */
    coact::Event capped = make_event(1U);
    harness::set_time_ns(1000U);
    const coact::SubmitResult rcap = coord.submit_from_task(t1, &capped, qos);
    record_disposition("S10.cap_rejects", rcap.disposition,
                       coact::SubmitDisposition::RejectedFull);
    record_u64("S10.cap_reason", static_cast<uint64_t>(rcap.reason),
               static_cast<uint64_t>(coact::AdmissionReason::kBacklogFull));
    record_u64("S10.pending_stays_one",
               static_cast<uint64_t>(ao.pending().load()), 1U);
    /* The interval reservation was rolled back: the window is still at 0. */
    record_u64("S10.window_rolled_back",
               coord.admission_policy().last_admit_ns(t1), 0U);

    coact::StagingSlot drained{};
    const bool drained_ok = staging.dequeue_one(drained, 1000U);
    record(drained_ok, "S10.drain_one");
    if (drained_ok) {
        ao.pending().decrement();
        coact::event_gc(drained.event);
    }

    /* Same timestamp as the rejected submission: only a rolled-back window can
       admit it now. */
    coact::Event retry = make_event(1U);
    const coact::SubmitResult rretry = coord.submit_from_task(t1, &retry, qos);
    record_disposition("S10.retry_same_ts_admitted", rretry.disposition,
                       coact::SubmitDisposition::Queued);
    record_u64("S10.window_after_retry",
               coord.admission_policy().last_admit_ns(t1), 1000U);

    /* And the interval still bites afterwards. */
    coact::Event too_soon = make_event(1U);
    harness::set_time_ns(1999U);
    const coact::SubmitResult rlate = coord.submit_from_task(t1, &too_soon, qos);
    record_disposition("S10.interval_still_enforced", rlate.disposition,
                       coact::SubmitDisposition::DroppedRateLimit);
    record_u64("S10.pending_after_all",
               static_cast<uint64_t>(ao.pending().load()), 1U);
}

// ---------------------------------------------------------------------------
// S11: a dispatch burst is not an arrival burst. Two producers submit at the
// same timestamp, the queue is drained in one batch at a much later virtual
// time, and the arrival window must stay keyed to the arrival timestamps. A
// gate that sampled the clock on the dispatch side would admit the next
// submission immediately; one shared window for both targets would reject it.
// ---------------------------------------------------------------------------
void case_s11_burst_not_misjudged()
{
    StageT staging(noop_cs());
    RegistryT registry;
    MonitorT monitor;
    Cfg cfg{};
    BankT breakers(cfg);
    harness::BarePal pal;
    BankAdmCoord coord(staging, registry, monitor, breakers, pal);

    StagedAo a1(kStates, 2U, kTransitions, 1U, 1, 4U);
    SecondStagedAo a2(kStates, 2U, kTransitions, 1U, 1, 4U);
    a1.init(make_event(0U));
    a2.init(make_event(0U));
    const coact::TargetId t1(1U);
    const coact::TargetId t2(2U);
    record(registry.bind_at(t1, a1, a1.logical_prio()), "S11.bind_t1");
    record(registry.bind_at(t2, a2, a2.logical_prio()), "S11.bind_t2");

    const coact::EventQos qos{false, false};
    coact::Event e0 = make_event(1U);
    coact::Event f0 = make_event(1U);
    coact::Event e1 = make_event(1U);
    coact::Event f1 = make_event(1U);

    /* One producer burst: both targets at t=0, both again at t=1000. */
    harness::set_time_ns(0U);
    record_disposition("S11.t1_at_0",
                       coord.submit_from_task(t1, &e0, qos).disposition,
                       coact::SubmitDisposition::Queued);
    record_disposition("S11.t2_at_0",
                       coord.submit_from_task(t2, &f0, qos).disposition,
                       coact::SubmitDisposition::Queued);

    coact::Event g0 = make_event(1U);
    coact::Event h0 = make_event(1U);
    harness::set_time_ns(999U);
    record_disposition("S11.t1_too_soon",
                       coord.submit_from_task(t1, &g0, qos).disposition,
                       coact::SubmitDisposition::DroppedRateLimit);
    record_disposition("S11.t2_too_soon",
                       coord.submit_from_task(t2, &h0, qos).disposition,
                       coact::SubmitDisposition::DroppedRateLimit);

    harness::set_time_ns(1000U);
    record_disposition("S11.t1_at_1000",
                       coord.submit_from_task(t1, &e1, qos).disposition,
                       coact::SubmitDisposition::Queued);
    record_disposition("S11.t2_at_1000",
                       coord.submit_from_task(t2, &f1, qos).disposition,
                       coact::SubmitDisposition::Queued);
    record_u64("S11.pending_before_drain",
               static_cast<uint64_t>(a1.pending().load())
                   + static_cast<uint64_t>(a2.pending().load()),
               4U);

    /* Drain in one batch, far in the future. batch_used_ only resets in
       begin_batch(), and four slots is exactly one batch of Cfg. */
    constexpr uint64_t kDrainNow = 500000U;
    const coact::Event* expected[4] = {&e0, &f0, &e1, &f1};
    staging.begin_batch();
    uint32_t drained = 0U;
    bool order_ok = true;
    coact::StagingSlot slot{};
    while ((drained < 4U) && (staging.batch_used() < Cfg::kBatchSizeMax)) {
        if (!staging.dequeue_one(slot, kDrainNow)) {
            break;
        }
        if (slot.event != expected[drained]) {
            order_ok = false;
        }
        ((coact::TargetId(1U) == slot.target) ? a1.pending() : a2.pending())
            .decrement();
        coact::event_gc(slot.event);
        ++drained;
    }
    record_u64("S11.batch_drained", drained, 4U);
    record(order_ok, "S11.batch_order");
    record_u64("S11.queue_empty_after_batch",
               static_cast<uint64_t>(staging.size(coact::Partition::High))
                   + static_cast<uint64_t>(staging.size(coact::Partition::Normal))
                   + static_cast<uint64_t>(staging.size(coact::Partition::Low)),
               0U);
    record_u64("S11.pending_after_drain",
               static_cast<uint64_t>(a1.pending().load())
                   + static_cast<uint64_t>(a2.pending().load()),
               0U);

    /* The window still reads 1000: the drain at t=500000 moved nothing. */
    record_u64("S11.t1_window_before_next",
               coord.admission_policy().last_admit_ns(t1), 1000U);
    record_u64("S11.t2_window_before_next",
               coord.admission_policy().last_admit_ns(t2), 1000U);

    coact::Event e2 = make_event(1U);
    coact::Event f2 = make_event(1U);
    harness::set_time_ns(2000U);
    record_disposition("S11.t1_after_drain",
                       coord.submit_from_task(t1, &e2, qos).disposition,
                       coact::SubmitDisposition::Queued);
    record_disposition("S11.t2_after_drain",
                       coord.submit_from_task(t2, &f2, qos).disposition,
                       coact::SubmitDisposition::Queued);
    record_u64("S11.t1_window_after_drain",
               coord.admission_policy().last_admit_ns(t1), 2000U);
    record_u64("S11.t2_window_after_drain",
               coord.admission_policy().last_admit_ns(t2), 2000U);

    /* And the interval still bites at that same timestamp. */
    coact::Event e3 = make_event(1U);
    record_disposition("S11.t1_same_ts_rejected",
                       coord.submit_from_task(t1, &e3, qos).disposition,
                       coact::SubmitDisposition::DroppedRateLimit);

    /* Leave the queue as the Dispatcher would: release what is still held. */
    staging.begin_batch();
    uint32_t tail = 0U;
    while (staging.batch_used() < Cfg::kBatchSizeMax) {
        if (!staging.dequeue_one(slot, 2000U)) {
            break;
        }
        ((coact::TargetId(1U) == slot.target) ? a1.pending() : a2.pending())
            .decrement();
        coact::event_gc(slot.event);
        ++tail;
    }
    record_u64("S11.tail_drained", tail, 2U);
    record_u64("S11.pending_at_end",
               static_cast<uint64_t>(a1.pending().load())
                   + static_cast<uint64_t>(a2.pending().load()),
               0U);
}

// ---------------------------------------------------------------------------
// S12: overload-guard escalation and recovery through the coordinator's own
// budget check. Three consecutive direct dispatches that overrun the RTC budget
// must take the AO to BrokenL1 - direct dispatch off, queued traffic still
// accepted - and the cooldown plus low-watermark samples plus healthy probe
// windows must bring it back to Normal. No Dispatcher runs in the fixture, so
// the recovery inputs are exactly the ones a Dispatcher feeds the bank.
// ---------------------------------------------------------------------------
void case_s12_breaker_escalation()
{
    StageT staging(noop_cs());
    RegistryT registry;
    MonitorT monitor;
    Cfg cfg{};
    BankT breakers(cfg);
    harness::BarePal pal;
    BankCoord coord(staging, registry, monitor, breakers, pal);

    SlowDirectAo ao(kSlowStates, 2U, kSlowTransitions, 1U, 1, 4U);
    ao.init(make_event(0U));
    const coact::TargetId t1(1U);
    record(registry.bind_at(t1, ao, ao.logical_prio()), "S12.bind");
    record(coact::BreakerLevel::Normal == breakers.level(t1),
           "S12.starts_normal");

    const coact::EventQos qos{false, false};
    harness::set_time_ns(0U);
    coact::Event d1 = make_event(1U);
    coact::Event d2 = make_event(1U);
    coact::Event d3 = make_event(1U);
    record_disposition("S12.overrun_1",
                       coord.submit_from_task(t1, &d1, qos).disposition,
                       coact::SubmitDisposition::Direct);
    record(coact::BreakerLevel::Normal == breakers.level(t1),
           "S12.normal_after_one_overrun");
    record_disposition("S12.overrun_2",
                       coord.submit_from_task(t1, &d2, qos).disposition,
                       coact::SubmitDisposition::Direct);
    record(coact::BreakerLevel::Normal == breakers.level(t1),
           "S12.normal_after_two_overruns");
    record_disposition("S12.overrun_3",
                       coord.submit_from_task(t1, &d3, qos).disposition,
                       coact::SubmitDisposition::Direct);
    record(coact::BreakerLevel::BrokenL1 == breakers.level(t1),
           "S12.l1_after_three_overruns");
    record_u64("S12.actions_ran", static_cast<uint64_t>(ao.context().actions),
               3U);
    /* L1 removes the direct path but must not drop traffic. */
    record(!breakers.direct_allowed(t1), "S12.direct_disabled_at_l1");
    record(!breakers.drop_non_critical(t1), "S12.l1_keeps_non_critical");
    coact::Event q1 = make_event(1U);
    record_disposition("S12.fourth_queued",
                       coord.submit_from_task(t1, &q1, qos).disposition,
                       coact::SubmitDisposition::Queued);

    /* Cooldown first, then the low-watermark samples that release it. */
    for (uint32_t i = 0U; i < Cfg::kCooldownCycles; ++i) {
        breakers.on_dispatch_cycle(t1);
    }
    record(coact::BreakerLevel::BrokenL1 == breakers.level(t1),
           "S12.still_l1_after_cooldown");
    for (uint32_t i = 0U; i < 3U; ++i) {
        breakers.broadcast_watermark(10U);
    }
    record(coact::BreakerLevel::Recovering == breakers.level(t1),
           "S12.recovering_after_low_watermark");
    record(!breakers.direct_allowed(t1),
           "S12.direct_still_off_while_recovering");

    for (uint32_t i = 0U; i < 3U; ++i) {
        breakers.on_probe_success(t1);
        breakers.on_dispatch_cycle(t1);
    }
    record(coact::BreakerLevel::Normal == breakers.level(t1),
           "S12.normal_after_healthy_windows");
    record(breakers.direct_allowed(t1), "S12.direct_restored");

    coact::Event d4 = make_event(1U);
    record_disposition("S12.direct_after_recovery",
                       coord.submit_from_task(t1, &d4, qos).disposition,
                       coact::SubmitDisposition::Direct);
    record_u64("S12.actions_after_recovery",
               static_cast<uint64_t>(ao.context().actions), 4U);
}

// ---------------------------------------------------------------------------
// S13: escalation is per AO. Filling the shared Normal lane through the real
// enqueue path trips target 1's breaker to L2 on overflow; target 2 must keep
// its Normal level and its direct path, and a critical event for target 1 must
// fail on lane capacity rather than on the overload guard.
// ---------------------------------------------------------------------------
void case_s13_breaker_isolation()
{
    StageT staging(noop_cs());
    RegistryT registry;
    MonitorT monitor;
    Cfg cfg{};
    BankT breakers(cfg);
    harness::BarePal pal;
    BankCoord coord(staging, registry, monitor, breakers, pal);

    StagedAo victim(kStates, 2U, kTransitions, 1U, 1, 4U);
    FastDirectAo healthy(kStates, 2U, kTransitions, 1U, 1, 4U);
    victim.init(make_event(0U));
    healthy.init(make_event(0U));
    const coact::TargetId t1(1U);
    const coact::TargetId t2(2U);
    record(registry.bind_at(t1, victim, victim.logical_prio()), "S13.bind_t1");
    record(registry.bind_at(t2, healthy, healthy.logical_prio()),
           "S13.bind_t2");

    const coact::EventQos qos{false, false};
    harness::set_time_ns(0U);
    static coact::Event fill[Cfg::kNormalCapacity];
    uint32_t queued = 0U;
    for (uint32_t i = 0U; i < Cfg::kNormalCapacity; ++i) {
        fill[i] = make_event(1U);
        if (coact::SubmitDisposition::Queued
            == coord.submit_from_task(t1, &fill[i], qos).disposition) {
            ++queued;
        }
    }
    record_u64("S13.lane_filled", queued, Cfg::kNormalCapacity);
    record(coact::BreakerLevel::Normal == breakers.level(t1),
           "S13.normal_while_lane_full");

    coact::Event spill = make_event(1U);
    record_disposition("S13.overflow_rejected",
                       coord.submit_from_task(t1, &spill, qos).disposition,
                       coact::SubmitDisposition::RejectedFull);
    record(coact::BreakerLevel::BrokenL2 == breakers.level(t1),
           "S13.t1_broken_l2");
    record(coact::BreakerLevel::Normal == breakers.level(t2),
           "S13.t2_still_normal");
    record(breakers.drop_non_critical(t1), "S13.t1_drops_non_critical");
    record(!breakers.drop_non_critical(t2), "S13.t2_keeps_non_critical");
    record(breakers.direct_allowed(t2), "S13.t2_direct_allowed");
    record(!breakers.direct_allowed(t1), "S13.t1_direct_disabled");
    record_u64("S13.overflow_counter",
               monitor.global().overflow.load(std::memory_order_relaxed), 1U);

    /* The healthy AO keeps running while its neighbour is broken. */
    coact::Event h1 = make_event(1U);
    record_disposition("S13.t2_direct_unaffected",
                       coord.submit_from_task(t2, &h1, qos).disposition,
                       coact::SubmitDisposition::Direct);
    record_u64("S13.t2_actions",
               static_cast<uint64_t>(healthy.context().actions), 1U);

    /* Critical traffic for the broken AO is not dropped by the guard: it fails
       on lane capacity, which is the distinction the guard promises. */
    const coact::EventQos crit{true, false};
    coact::Event c1 = make_event(1U);
    record_disposition("S13.t1_critical_not_overload",
                       coord.submit_from_task(t1, &c1, crit).disposition,
                       coact::SubmitDisposition::RejectedFull);
    coact::Event n1 = make_event(1U);
    record_disposition("S13.t1_non_critical_dropped",
                       coord.submit_from_task(t1, &n1, qos).disposition,
                       coact::SubmitDisposition::DroppedOverload);
    record_u64("S13.overload_counter",
               monitor.global().disposition_overload.load(
                   std::memory_order_relaxed),
               1U);
    record_u64("S13.t1_pending_unchanged",
               static_cast<uint64_t>(victim.pending().load()),
               Cfg::kNormalCapacity);
    record_u64("S13.t2_pending_zero",
               static_cast<uint64_t>(healthy.pending().load()), 0U);
}

// ---------------------------------------------------------------------------
// S14: event-pool reference accounting on the ARM ABI with the product's
// single-core profile. Every block the coordinator takes must come back exactly
// once - used() returns to zero after a drain, a multi-reference event survives
// until its last release, and an exhausted pool refuses a block instead of
// corrupting the free list. The pool's critical section is the real PRIMASK
// save/restore, not the no-op the queue-only scenarios use.
// ---------------------------------------------------------------------------
void case_s14_pool_accounting()
{
    using Pool = coact::EventPool<64U, 8U, coact::RttSingleCoreProfile>;
    static uint8_t storage[8U * 64U] __attribute__((aligned(16)));
    Pool pool;
    record(pool.init(storage, sizeof(storage), irq_cs()), "S14.pool_init");
    record_u64("S14.capacity", pool.capacity(), 8U);
    record_u64("S14.used_at_init", pool.used(), 0U);

    StageT staging(noop_cs());
    RegistryT registry;
    MonitorT monitor;
    Cfg cfg{};
    BreakerT breaker(cfg);
    harness::BarePal pal;
    BareCoord coord(staging, registry, monitor, breaker, pal);
    StagedAo ao(kStates, 2U, kTransitions, 1U, 1, 4U);
    ao.init(make_event(0U));
    const coact::TargetId t1(1U);
    record(registry.bind_at(t1, ao, ao.logical_prio()), "S14.bind");

    const coact::EventQos qos{false, false};
    harness::set_time_ns(0U);
    uint32_t submitted = 0U;
    for (uint32_t i = 0U; i < 3U; ++i) {
        coact::Event* e = pool.alloc(1U);
        if ((nullptr != e)
            && (coact::SubmitDisposition::Queued
                == coord.submit_from_task(t1, e, qos).disposition)) {
            ++submitted;
        }
    }
    record_u64("S14.submitted", submitted, 3U);
    record_u64("S14.used_while_queued", pool.used(), 3U);
    record_u64("S14.high_watermark", pool.high_watermark(), 3U);
    record_u64("S14.pending_while_queued",
               static_cast<uint64_t>(ao.pending().load()), 3U);

    /* Drain the way a Dispatcher does: one release per slot. */
    staging.begin_batch();
    uint32_t released = 0U;
    coact::StagingSlot slot{};
    while (staging.batch_used() < Cfg::kBatchSizeMax) {
        if (!staging.dequeue_one(slot, 1000U)) {
            break;
        }
        ao.pending().decrement();
        coact::event_gc(slot.event);
        ++released;
    }
    record_u64("S14.released", released, 3U);
    record_u64("S14.used_after_drain", pool.used(), 0U);
    record_u64("S14.pending_after_drain",
               static_cast<uint64_t>(ao.pending().load()), 0U);

    /* A block with extra owners survives until the last release. */
    coact::Event* shared = pool.alloc(1U);
    record(nullptr != shared, "S14.shared_alloc");
    if (nullptr != shared) {
        coact::event_ref_inc(shared);
        coact::event_ref_inc(shared);
        record_u64("S14.shared_ref_ctr", shared->ref_ctr, 3U);
        coact::event_gc(shared);
        coact::event_gc(shared);
        record_u64("S14.used_with_last_ref", pool.used(), 1U);
        coact::event_gc(shared);
        record_u64("S14.used_after_last_ref", pool.used(), 0U);
    }

    /* Exhaustion is a refusal, and every block still comes back. */
    coact::Event* held[8] = {nullptr};
    uint32_t got = 0U;
    for (uint32_t i = 0U; i < 8U; ++i) {
        held[i] = pool.alloc(1U);
        if (nullptr != held[i]) {
            ++got;
        }
    }
    record_u64("S14.alloc_until_full", got, 8U);
    record_u64("S14.used_at_full", pool.used(), 8U);
    record(nullptr == pool.alloc(1U), "S14.alloc_refused_when_full");
    for (uint32_t i = 0U; i < 8U; ++i) {
        coact::event_gc(held[i]);
    }
    record_u64("S14.used_after_release_all", pool.used(), 0U);

    /* Stop path: after shutdown the pool hands out nothing and the accounting
       stays at zero, so a late submit cannot resurrect a detached block. */
    pool.shutdown();
    record(nullptr == pool.alloc(1U), "S14.alloc_after_shutdown_refused");
    record_u64("S14.used_after_shutdown", pool.used(), 0U);
    harness::puts("S14.info watermark=");
    harness::put_u64(pool.high_watermark());
    harness::newline();
}

#endif  // COACT_HAS_ADMISSION

}  // namespace

// ---------------------------------------------------------------------------
// Trap reporting: any unexpected exception ends the run without RESULT PASS.
// ---------------------------------------------------------------------------
extern "C" {

[[noreturn]] void trap_line(const char* kind)
{
    harness::puts("FAIL trap.");
    harness::puts(kind);
    harness::newline();
    harness::puts("RESULT FAIL 0/0\r\n");
    for (;;) {
    }
}

}  // extern "C"

extern "C" void heap_trap(void) { trap_line("heap_alloc"); }

extern "C" void HardFault_Handler(void) { trap_line("hardfault"); }
extern "C" void MemManage_Handler(void) { trap_line("memmanage"); }
extern "C" void BusFault_Handler(void) { trap_line("busfault"); }
extern "C" void UsageFault_Handler(void) { trap_line("usagefault"); }
extern "C" void SVC_Handler(void) { trap_line("svc"); }
extern "C" void DebugMon_Handler(void) { trap_line("debugmon"); }
extern "C" void PendSV_Handler(void) { trap_line("pendsv"); }
extern "C" void SysTick_Handler(void) { trap_line("systick"); }

extern "C" void abort(void) noexcept
{
    trap_line("abort");
}

extern "C" int main(void)
{
    harness::uart_init();
    harness::puts("coact renode harness: baseline\r\n");

    case_baseline_direct();
    case_baseline_staged();
    case_baseline_policy();

#ifdef COACT_HAS_ADMISSION
    harness::puts("coact renode harness: admission scenarios\r\n");
    case_s1_interval();
    case_s2_reject_keeps_window();
    case_s3_enqueue_rollback();
    case_s4_backlog_cap();
    case_s5_default_off();
    case_s6_cross_target();
    case_s7_snapshot_ring();
    case_s8_isr_preemption();
    case_s9_footprint();
    case_s10_interval_and_cap();
    case_s11_burst_not_misjudged();
    case_s12_breaker_escalation();
    case_s13_breaker_isolation();
    case_s14_pool_accounting();
#endif

    harness::puts("RESULT ");
    if (0 == g_fail) {
        harness::puts("PASS ");
    }
    else {
        harness::puts("FAIL ");
    }
    harness::put_u64(static_cast<uint64_t>(g_pass));
    harness::putc('/');
    harness::put_u64(static_cast<uint64_t>(g_pass + g_fail));
    harness::newline();

    for (;;) {
    }
}
