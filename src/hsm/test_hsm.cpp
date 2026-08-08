// coact HSM differential tests: entry/exit/action ordering, bubbling,
// guard fallback, LCA external, self, internal, init, and negative cases.
// SPDX-License-Identifier: MIT

#include "coact/hsm.hpp"

#include "test/test_harness.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Test context and global trace. Entry/exit/action callbacks push a label
// into a shared order array; assertions then verify the exact sequence.
// Host tests may use the heap; the hsm core itself never allocates.
// ---------------------------------------------------------------------------
struct TestCtx {
    std::vector<std::string>* trace;
};

static void record(TestCtx& ctx, const char* label) {
    ctx.trace->push_back(label);
}

// Signal ids (contract 4.1 Event::signal is uint16_t).
enum : uint16_t {
    SIG_LEAF = 1U,    // leaf internal: action only
    SIG_UP = 2U,      // parent (s1) captures after one bubble step
    SIG_INT = 3U,     // dedicated internal transition
    SIG_GUARD = 4U,   // guard-failure fallback on the same source
    SIG_GUARD2 = 5U,  // all candidates on s2 fail -> parent s1 search
    SIG_EXT = 6U,     // external s1 -> s4 (LCA s0)
    SIG_SELF = 7U,    // self transition on s2
    SIG_ROOT = 8U,    // handled only at root (depth-bound test)
    SIG_NOMATCH = 100U
};

// --- state entry/exit callbacks ------------------------------------------
static void s0_entry(TestCtx& ctx) noexcept { record(ctx, "E0"); }
static void s0_exit(TestCtx& ctx) noexcept { record(ctx, "X0"); }
static void s1_entry(TestCtx& ctx) noexcept { record(ctx, "E1"); }
static void s1_exit(TestCtx& ctx) noexcept { record(ctx, "X1"); }
static void s2_entry(TestCtx& ctx) noexcept { record(ctx, "E2"); }
static void s2_exit(TestCtx& ctx) noexcept { record(ctx, "X2"); }
static void s3_entry(TestCtx& ctx) noexcept { record(ctx, "E3"); }
static void s3_exit(TestCtx& ctx) noexcept { record(ctx, "X3"); }
static void s4_entry(TestCtx& ctx) noexcept { record(ctx, "E4"); }
static void s4_exit(TestCtx& ctx) noexcept { record(ctx, "X4"); }

// --- guards ----------------------------------------------------------------
static bool guard_false(const TestCtx&, const coact::Event&) noexcept {
    return false;
}

static bool guard_true(const TestCtx&, const coact::Event&) noexcept {
    return true;
}

// --- transition actions -----------------------------------------------------
static void a_leaf(TestCtx& ctx, const coact::Event&) noexcept { record(ctx, "A_LEAF"); }
static void a_up(TestCtx& ctx, const coact::Event&) noexcept { record(ctx, "A_UP"); }
static void a_int(TestCtx& ctx, const coact::Event&) noexcept { record(ctx, "A_INT"); }
static void a_g1(TestCtx& ctx, const coact::Event&) noexcept { record(ctx, "A_G1"); }
static void a_g2(TestCtx& ctx, const coact::Event&) noexcept { record(ctx, "A_G2"); }
static void a_g3(TestCtx& ctx, const coact::Event&) noexcept { record(ctx, "A_G3"); }
static void a_g4(TestCtx& ctx, const coact::Event&) noexcept { record(ctx, "A_G4"); }
static void a_g5(TestCtx& ctx, const coact::Event&) noexcept { record(ctx, "A_G5"); }
static void a_ext(TestCtx& ctx, const coact::Event&) noexcept { record(ctx, "A_EXT"); }
static void a_self(TestCtx& ctx, const coact::Event&) noexcept { record(ctx, "A_SELF"); }
static void a_root(TestCtx& ctx, const coact::Event&) noexcept { record(ctx, "A_ROOT"); }

// ---------------------------------------------------------------------------
// State hierarchy (three nested levels in each branch):
//   s0 (root, parent -1) -> s1 (mid)  -> s2 (leaf, initial)
//                         -> s3 (mid)  -> s4 (leaf2)
// ---------------------------------------------------------------------------
static const coact::StateDef<TestCtx> states[] = {
    /* 0 */ {-1, s0_entry, s0_exit},
    /* 1 */ {0, s1_entry, s1_exit},
    /* 2 */ {1, s2_entry, s2_exit},
    /* 3 */ {0, s3_entry, s3_exit},
    /* 4 */ {3, s4_entry, s4_exit}
};

static const coact::TransitionDef<TestCtx> transitions[] = {
    // [0] leaf internal: action only, no exit/entry
    {2, SIG_LEAF, 0, coact::TransitionKind::Internal, nullptr, a_leaf},
    // [1] parent capture: bubbles s2 -> s1, internal semantics
    {1, SIG_UP, 0, coact::TransitionKind::Internal, nullptr, a_up},
    // [2] dedicated internal transition on the leaf
    {2, SIG_INT, 0, coact::TransitionKind::Internal, nullptr, a_int},
    // [3][4] guard-failure fallback on the same (s2, SIG_GUARD)
    {2, SIG_GUARD, 0, coact::TransitionKind::Internal, guard_false, a_g1},
    {2, SIG_GUARD, 0, coact::TransitionKind::Internal, guard_true, a_g2},
    // [5][6][7] all s2 candidates fail guard -> continue parent search at s1
    {2, SIG_GUARD2, 0, coact::TransitionKind::Internal, guard_false, a_g3},
    {2, SIG_GUARD2, 0, coact::TransitionKind::Internal, guard_false, a_g4},
    {1, SIG_GUARD2, 0, coact::TransitionKind::Internal, nullptr, a_g5},
    // [8] external s1 -> s4: LCA is s0, exit s2,s1 then enter s3,s4
    {1, SIG_EXT, 4, coact::TransitionKind::External, nullptr, a_ext},
    // [9] self transition on the leaf s2
    {2, SIG_SELF, 2, coact::TransitionKind::Self, nullptr, a_self},
    // [10] root-level transition (only reachable when max_depth allows)
    {0, SIG_ROOT, 0, coact::TransitionKind::Internal, nullptr, a_root}
};

constexpr uint16_t kNumStates = sizeof(states) / sizeof(states[0]);
constexpr uint16_t kNumTransitions = sizeof(transitions) / sizeof(transitions[0]);
constexpr int8_t kInitialState = 2;

static coact::Hsm<TestCtx> make_hsm(uint8_t max_depth = 4) {
    return coact::Hsm<TestCtx>(
        states, kNumStates, transitions, kNumTransitions, kInitialState, max_depth);
}

static void init_machine(coact::Hsm<TestCtx>& hsm, TestCtx& ctx,
                         std::vector<std::string>& trace) {
    trace.clear();
    hsm.init(ctx, coact::Event{SIG_LEAF, 0, 0});
    trace.clear();  // drop the init entry sequence; each test asserts its own
}

}  // namespace

// ---------------------------------------------------------------------------
// init: entry sequence runs from the root down to the initial leaf.
// ---------------------------------------------------------------------------
COACT_TEST(init_enters_root_to_leaf) {
    std::vector<std::string> trace;
    TestCtx ctx{&trace};
    coact::Hsm<TestCtx> hsm = make_hsm();
    hsm.init(ctx, coact::Event{SIG_LEAF, 0, 0});
    REQUIRE(trace == std::vector<std::string>({"E0", "E1", "E2"}));
    CHECK_EQ(hsm.current_state(), kInitialState);
}

// ---------------------------------------------------------------------------
// leaf event: internal transition runs only the action.
// ---------------------------------------------------------------------------
COACT_TEST(leaf_event_hits_action) {
    std::vector<std::string> trace;
    TestCtx ctx{&trace};
    coact::Hsm<TestCtx> hsm = make_hsm();
    init_machine(hsm, ctx, trace);
    CHECK(hsm.dispatch(ctx, coact::Event{SIG_LEAF, 0, 0}));
    REQUIRE(trace == std::vector<std::string>({"A_LEAF"}));
    CHECK_EQ(hsm.current_state(), kInitialState);
}

// ---------------------------------------------------------------------------
// parent capture: the event bubbles up one level to s1; the internal
// transition runs its action and leaves the state untouched.
// ---------------------------------------------------------------------------
COACT_TEST(parent_captures_event) {
    std::vector<std::string> trace;
    TestCtx ctx{&trace};
    coact::Hsm<TestCtx> hsm = make_hsm();
    init_machine(hsm, ctx, trace);
    CHECK(hsm.dispatch(ctx, coact::Event{SIG_UP, 0, 0}));
    REQUIRE(trace == std::vector<std::string>({"A_UP"}));
    CHECK_EQ(hsm.current_state(), kInitialState);
}

// ---------------------------------------------------------------------------
// guard failure: the first (s2, SIG_GUARD) candidate fails its guard, so the
// next candidate with the same source/signal is tried and its action runs.
// ---------------------------------------------------------------------------
COACT_TEST(guard_fallback_next_candidate) {
    std::vector<std::string> trace;
    TestCtx ctx{&trace};
    coact::Hsm<TestCtx> hsm = make_hsm();
    init_machine(hsm, ctx, trace);
    CHECK(hsm.dispatch(ctx, coact::Event{SIG_GUARD, 0, 0}));
    REQUIRE(trace == std::vector<std::string>({"A_G2"}));  // A_G1 must not run
    CHECK_EQ(hsm.current_state(), kInitialState);
}

// ---------------------------------------------------------------------------
// guard fallback across sources: both s2 candidates fail their guards, so the
// lookup continues with the parent s1, whose candidate passes.
// ---------------------------------------------------------------------------
COACT_TEST(guard_all_fail_then_parent_search) {
    std::vector<std::string> trace;
    TestCtx ctx{&trace};
    coact::Hsm<TestCtx> hsm = make_hsm();
    init_machine(hsm, ctx, trace);
    CHECK(hsm.dispatch(ctx, coact::Event{SIG_GUARD2, 0, 0}));
    REQUIRE(trace == std::vector<std::string>({"A_G5"}));
    CHECK_EQ(hsm.current_state(), kInitialState);
}

// ---------------------------------------------------------------------------
// internal transition: no exit/entry fires at all.
// ---------------------------------------------------------------------------
COACT_TEST(internal_no_exit_entry) {
    std::vector<std::string> trace;
    TestCtx ctx{&trace};
    coact::Hsm<TestCtx> hsm = make_hsm();
    init_machine(hsm, ctx, trace);
    CHECK(hsm.dispatch(ctx, coact::Event{SIG_INT, 0, 0}));
    REQUIRE(trace == std::vector<std::string>({"A_INT"}));
    CHECK_EQ(hsm.current_state(), kInitialState);
}

// ---------------------------------------------------------------------------
// external transition found at s1: the active leaf s2 exits first, then s1,
// the action runs, then entry proceeds s3 -> s4.
// ---------------------------------------------------------------------------
COACT_TEST(external_lca_exit_entry_order) {
    std::vector<std::string> trace;
    TestCtx ctx{&trace};
    coact::Hsm<TestCtx> hsm = make_hsm();
    init_machine(hsm, ctx, trace);
    CHECK(hsm.dispatch(ctx, coact::Event{SIG_EXT, 0, 0}));
    REQUIRE(trace ==
            std::vector<std::string>({"X2", "X1", "A_EXT", "E3", "E4"}));
    CHECK_EQ(hsm.current_state(), int8_t(4));
}

// ---------------------------------------------------------------------------
// self transition: exit the leaf, run the action, re-enter the leaf.
// ---------------------------------------------------------------------------
COACT_TEST(self_exits_action_reeneters) {
    std::vector<std::string> trace;
    TestCtx ctx{&trace};
    coact::Hsm<TestCtx> hsm = make_hsm();
    init_machine(hsm, ctx, trace);
    CHECK(hsm.dispatch(ctx, coact::Event{SIG_SELF, 0, 0}));
    REQUIRE(trace == std::vector<std::string>({"X2", "A_SELF", "E2"}));
    CHECK_EQ(hsm.current_state(), kInitialState);
}

// ---------------------------------------------------------------------------
// negative: an event with no matching transition anywhere is unhandled and
// leaves the machine untouched; dispatching before init is also safe.
// ---------------------------------------------------------------------------
COACT_TEST(unhandled_event_returns_false) {
    std::vector<std::string> trace;
    TestCtx ctx{&trace};
    coact::Hsm<TestCtx> hsm = make_hsm();
    CHECK(!hsm.dispatch(ctx, coact::Event{SIG_LEAF, 0, 0}));  // before init
    init_machine(hsm, ctx, trace);
    CHECK(!hsm.dispatch(ctx, coact::Event{SIG_NOMATCH, 0, 0}));
    REQUIRE(trace.empty());
    CHECK_EQ(hsm.current_state(), kInitialState);
}

// ---------------------------------------------------------------------------
// negative: with max_depth=1 the lookup cannot ascend far enough to reach the
// root transition (two parent hops are required), so it stops safely without
// touching the root; the leaf-level transition still resolves.
// ---------------------------------------------------------------------------
COACT_TEST(max_depth_bound_safe_stop) {
    std::vector<std::string> trace;
    TestCtx ctx{&trace};
    coact::Hsm<TestCtx> hsm = make_hsm(1);  // at most one parent hop
    init_machine(hsm, ctx, trace);
    CHECK(!hsm.dispatch(ctx, coact::Event{SIG_ROOT, 0, 0}));  // needs 2 hops
    REQUIRE(trace.empty());
    CHECK_EQ(hsm.current_state(), kInitialState);

    // Within the bound the leaf handler still works.
    CHECK(hsm.dispatch(ctx, coact::Event{SIG_LEAF, 0, 0}));
    REQUIRE(trace == std::vector<std::string>({"A_LEAF"}));
}

// ---------------------------------------------------------------------------
// root-level event resolves with a generous depth bound.
// ---------------------------------------------------------------------------
COACT_TEST(root_level_event_resolves) {
    std::vector<std::string> trace;
    TestCtx ctx{&trace};
    coact::Hsm<TestCtx> hsm = make_hsm(4);
    init_machine(hsm, ctx, trace);
    CHECK(hsm.dispatch(ctx, coact::Event{SIG_ROOT, 0, 0}));
    REQUIRE(trace == std::vector<std::string>({"A_ROOT"}));
    CHECK_EQ(hsm.current_state(), kInitialState);
}

COACT_TEST_MAIN()
