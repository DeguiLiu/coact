// coact ao module host tests: ExecutionLease, PendingCounter, AoRegistry and
// the Ao<Context, Hsm, Traits> RTC dispatch incl. illegal-reentry negative.
// See contract 4.6 and design 5. COACT_ASSERT cases run in a forked child
// so the harness can confirm the abort signal without killing the suite.
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <cstdlib>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"

#include "test/test_harness.hpp"

namespace {

// ---------------------------------------------------------------------------
// Fork helper: run fn in a child and report whether it terminated via abort.
// The inherited harness stats are discarded (copy-on-write), so the check is
// purely "the child was killed by SIGABRT".
// ---------------------------------------------------------------------------
template <typename Fn>
static bool expect_abort(Fn&& fn)
{
    const pid_t pid = fork();
    if (0 == pid) {
        fn();
        std::_Exit(0);  // reached only if fn returned without aborting
    }
    if (pid < 0) {
        return false;   // fork failed: nothing to observe
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        return (SIGABRT == WTERMSIG(status));
    }
    return false;
}

// ---------------------------------------------------------------------------
// ExecutionLease suites.
// ---------------------------------------------------------------------------

COACT_TEST(lease_starts_idle_and_acquire_release_roundtrip)
{
    coact::ExecutionLease lease;
    CHECK(coact::AoRunState::Idle == lease.state());

    CHECK(lease.try_acquire(coact::AoRunState::RunningDirect));
    CHECK(coact::AoRunState::RunningDirect == lease.state());

    lease.release(coact::AoRunState::RunningDirect);
    CHECK(coact::AoRunState::Idle == lease.state());

    CHECK(lease.try_acquire(coact::AoRunState::RunningDispatcher));
    CHECK(coact::AoRunState::RunningDispatcher == lease.state());
    lease.release(coact::AoRunState::RunningDispatcher);
    CHECK(coact::AoRunState::Idle == lease.state());
}

COACT_TEST(lease_competition_second_acquire_fails)
{
    coact::ExecutionLease lease;
    CHECK(lease.try_acquire(coact::AoRunState::RunningDirect));

    // A competing acquirer (Dispatcher path) must fail, not spin.
    CHECK(!lease.try_acquire(coact::AoRunState::RunningDispatcher));
    CHECK(coact::AoRunState::RunningDirect == lease.state());

    // The holder still sees its own running state and can release.
    lease.release(coact::AoRunState::RunningDirect);
    CHECK(lease.try_acquire(coact::AoRunState::RunningDispatcher));
}

COACT_TEST(lease_acquire_into_idle_rejected)
{
    coact::ExecutionLease lease;
    CHECK(!lease.try_acquire(coact::AoRunState::Idle));
    CHECK(coact::AoRunState::Idle == lease.state());
}

COACT_TEST(lease_wrong_release_budget_is_abort)
{
    // Releasing with a state that does not match the running state is a
    // protocol violation: the child must terminate via SIGABRT.
    const bool aborted = expect_abort([]() {
        coact::ExecutionLease lease;
        lease.try_acquire(coact::AoRunState::RunningDispatcher);
        lease.release(coact::AoRunState::RunningDirect);  // WRONG expected
    });
    CHECK(aborted);
}

COACT_TEST(lease_release_while_idle_is_abort)
{
    const bool aborted = expect_abort([]() {
        coact::ExecutionLease lease;  // still Idle
        lease.release(coact::AoRunState::RunningDirect);  // WRONG: already idle
    });
    CHECK(aborted);
}

// ---------------------------------------------------------------------------
// PendingCounter suites.
// ---------------------------------------------------------------------------

COACT_TEST(pending_increment_decrement_roundtrip)
{
    coact::PendingCounter pending;
    CHECK_EQ(pending.load(), 0U);
    pending.increment();
    pending.increment();
    pending.increment();
    CHECK_EQ(pending.load(), 3U);
    pending.decrement();
    CHECK_EQ(pending.load(), 2U);
    pending.decrement();
    pending.decrement();
    CHECK_EQ(pending.load(), 0U);
}

COACT_TEST(pending_decrement_below_zero_is_abort)
{
    const bool aborted = expect_abort([]() {
        coact::PendingCounter pending;  // count == 0
        pending.decrement();            // underflow is a fault
    });
    CHECK(aborted);
}

// ---------------------------------------------------------------------------
// Minimal HSM shared by the Ao suites.
// ---------------------------------------------------------------------------
struct AoCtx {
    int ticks;
    int rec_calls;
};

enum : uint16_t {
    SIG_TICK = 1U,
    SIG_REC = 2U,
    SIG_NOMATCH = 99U
};

// Re-entrancy negative drives a second dispatch from an action. The AO base
// address is stashed globally so the (host) HSM action can re-enter it.
coact::AoBase* g_reentry_target = nullptr;

static void on_tick(AoCtx& ctx, const coact::Event&) noexcept
{
    ++ctx.ticks;
}

static void on_rec(AoCtx& ctx, const coact::Event&) noexcept
{
    ++ctx.rec_calls;
    // Re-enter dispatch for the same AO while its lease is held: illegal.
    if (g_reentry_target != nullptr) {
        g_reentry_target->dispatch(coact::Event{SIG_NOMATCH, 0U, 0U});
    }
}

static const coact::StateDef<AoCtx> ao_states[] = {
    /* 0 root */ {-1, nullptr, nullptr},
    /* 1 leaf */ {0, nullptr, nullptr}
};

static const coact::TransitionDef<AoCtx> ao_trans[] = {
    {1, SIG_TICK, 0, coact::TransitionKind::Internal, nullptr, on_tick},
    {1, SIG_REC, 0, coact::TransitionKind::Internal, nullptr, on_rec}
};

constexpr uint16_t kAoNumStates = sizeof(ao_states) / sizeof(ao_states[0]);
constexpr uint16_t kAoNumTransitions = sizeof(ao_trans) / sizeof(ao_trans[0]);
constexpr int8_t kAoInitialState = 1;
constexpr uint8_t kAoMaxDepth = 4U;

// ---------------------------------------------------------------------------
// Traits inject the AO compile-time properties.
// ---------------------------------------------------------------------------
struct TraitsA {
    static coact::LogicalPrio logical_prio() noexcept { return 10U; }
    static coact::PriorityClass priority_class() noexcept
    {
        return coact::PriorityClass::Normal;
    }
    static bool direct_eligible() noexcept { return true; }
    static bool isr_direct_safe() noexcept { return false; }
    static constexpr uint64_t kRtcBudgetNs = coact::DefaultConfig::kRtcBudgetNs;
};

struct TraitsB {
    static coact::LogicalPrio logical_prio() noexcept { return 20U; }
    static coact::PriorityClass priority_class() noexcept
    {
        return coact::PriorityClass::High;
    }
    static bool direct_eligible() noexcept { return false; }
    static bool isr_direct_safe() noexcept { return true; }
    static constexpr uint64_t kRtcBudgetNs = 5000ULL;
};

typedef coact::Ao<AoCtx, coact::Hsm<AoCtx>, TraitsA> AoA;
typedef coact::Ao<AoCtx, coact::Hsm<AoCtx>, TraitsB> AoB;

COACT_TEST(ao_rtc_dispatch_runs_hsm_normal)
{
    AoA ao(ao_states, kAoNumStates, ao_trans, kAoNumTransitions,
           kAoInitialState, kAoMaxDepth);
    ao.init(coact::Event{SIG_TICK, 0U, 0U});
    REQUIRE(coact::AoRunState::Idle == ao.lease().state());  // before dispatch

    ao.dispatch(coact::Event{SIG_TICK, 0U, 0U});
    // The synchronous RTC acquires and releases within one call, so the AO
    // is idle again as soon as dispatch returns.
    CHECK(coact::AoRunState::Idle == ao.lease().state());
}

COACT_TEST(ao_rtc_unhandled_event_tolerated)
{
    AoA ao(ao_states, kAoNumStates, ao_trans, kAoNumTransitions,
           kAoInitialState, kAoMaxDepth);
    ao.init(coact::Event{SIG_TICK, 0U, 0U});
    ao.dispatch(coact::Event{SIG_TICK, 0U, 0U});  // handled
    ao.dispatch(coact::Event{SIG_NOMATCH, 0U, 0U});  // unhandled is a no-op
    CHECK(coact::AoRunState::Idle == ao.lease().state());
}

COACT_TEST(ao_illegal_reentry_is_abort)
{
    AoA ao(ao_states, kAoNumStates, ao_trans, kAoNumTransitions,
           kAoInitialState, kAoMaxDepth);
    ao.init(coact::Event{SIG_TICK, 0U, 0U});
    g_reentry_target = &ao;

    // SIG_REC drives a re-entrant dispatch -> COACT_ASSERT in the child.
    const bool aborted = expect_abort([&ao]() {
        ao.dispatch(coact::Event{SIG_REC, 0U, 0U});
    });
    CHECK(aborted);
    g_reentry_target = nullptr;
}

COACT_TEST(ao_traits_injected_through_interface)
{
    AoA aoa(ao_states, kAoNumStates, ao_trans, kAoNumTransitions,
            kAoInitialState, kAoMaxDepth);
    AoB aob(ao_states, kAoNumStates, ao_trans, kAoNumTransitions,
            kAoInitialState, kAoMaxDepth);

    coact::AoBase& base_a = aoa;
    coact::AoBase& base_b = aob;

    CHECK_EQ(base_a.logical_prio(), 10U);
    CHECK(coact::PriorityClass::Normal == base_a.priority_class());
    CHECK(base_a.direct_eligible());
    CHECK(!base_a.isr_direct_safe());

    CHECK_EQ(base_b.logical_prio(), 20U);
    CHECK(coact::PriorityClass::High == base_b.priority_class());
    CHECK(!base_b.direct_eligible());
    CHECK(base_b.isr_direct_safe());

    CHECK_EQ(AoA::rtc_budget_ns(), coact::DefaultConfig::kRtcBudgetNs);
    CHECK_EQ(AoB::rtc_budget_ns(), 5000ULL);

    // lease()/pending() accessors are reachable through the erased base.
    CHECK(coact::AoRunState::Idle == base_a.lease().state());
    CHECK_EQ(base_a.pending().load(), 0U);
}

// ---------------------------------------------------------------------------
// AoRegistry suites.
// ---------------------------------------------------------------------------

COACT_TEST(registry_bind_lookup_roundtrip)
{
    coact::AoRegistry reg;
    AoA aoa(ao_states, kAoNumStates, ao_trans, kAoNumTransitions,
            kAoInitialState, kAoMaxDepth);
    AoB aob(ao_states, kAoNumStates, ao_trans, kAoNumTransitions,
            kAoInitialState, kAoMaxDepth);

    CHECK(reg.bind(&aoa, 10U));
    CHECK(reg.bind(&aob, 20U));

    const coact::TargetId id_a = reg.target_of(&aoa);
    const coact::TargetId id_b = reg.target_of(&aob);
    CHECK(coact::kInvalidTarget != id_a);
    CHECK(coact::kInvalidTarget != id_b);
    CHECK(id_a != id_b);

    CHECK(reg.lookup(id_a) == &aoa);
    CHECK(reg.lookup(id_b) == &aob);
    CHECK(coact::kInvalidTarget == reg.target_of(nullptr));
}

COACT_TEST(registry_lookup_unknown_or_invalid_target_is_null)
{
    coact::AoRegistry reg;
    AoA aoa(ao_states, kAoNumStates, ao_trans, kAoNumTransitions,
            kAoInitialState, kAoMaxDepth);
    reg.bind(&aoa, 10U);

    CHECK(nullptr == reg.lookup(coact::kInvalidTarget));
    CHECK(nullptr == reg.lookup(static_cast<coact::TargetId>(
                     coact::AoRegistry::kCapacity + 1U)));  // out of range
    const coact::TargetId id_a = reg.target_of(&aoa);
    CHECK(nullptr == reg.lookup(static_cast<coact::TargetId>(id_a + 1U)));
}

COACT_TEST(registry_duplicate_priority_rejected)
{
    coact::AoRegistry reg;
    AoA aoa(ao_states, kAoNumStates, ao_trans, kAoNumTransitions,
            kAoInitialState, kAoMaxDepth);
    AoA aoa2(ao_states, kAoNumStates, ao_trans, kAoNumTransitions,
             kAoInitialState, kAoMaxDepth);

    CHECK(reg.bind(&aoa, 10U));
    // A second AO claiming the same logical priority must be refused.
    CHECK(!reg.bind(&aoa2, 10U));
}

COACT_TEST(registry_null_and_invalid_prio_rejected)
{
    coact::AoRegistry reg;
    AoA aoa(ao_states, kAoNumStates, ao_trans, kAoNumTransitions,
            kAoInitialState, kAoMaxDepth);

    CHECK(!reg.bind(nullptr, 10U));                        // null AO
    CHECK(!reg.bind(&aoa, coact::kInvalidPrio));           // dummy priority
    CHECK(reg.bind(&aoa, 10U));                            // then valid
    CHECK(!reg.bind(&aoa, 10U));                           // same AO re-bind
}

}  // namespace

COACT_TEST_MAIN()
