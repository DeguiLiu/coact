// coact::coro combinator test: when_all / when_any / when_some boundary
// conditions, out-of-order completion, first-error settlement and
// idempotent re-evaluation. Single-threaded (the dispatcher thread is the
// event plane; group evaluation itself is pure function calls).
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <array>
#include <cstdint>
#include <type_traits>
#include <unistd.h>
#include <utility>

#include "coact/ao.hpp"
#include "coact/coro/coro.hpp"
#include "coact/pal_posix.hpp"
#include "coact/pool.hpp"
#include "coact/runtime.hpp"

namespace {

struct CountCtx {
    unsigned group_events = 0U;
};

static void c_noop_entry(CountCtx&) {}
static void c_noop_exit(CountCtx&) {}
static bool c_always(const CountCtx&, const coact::Event&) { return true; }
static void c_count(CountCtx& c, const coact::Event&) { c.group_events++; }

static const coact::StateDef<CountCtx> kCStates[] = {
    /* 0: root */ { -1, nullptr, nullptr, nullptr, 1 },
    /* 1: S1   */ {  0, c_noop_entry, c_noop_exit },
};
static const coact::TransitionDef<CountCtx> kCTrans[] = {
    { 1, 77U, 1, coact::TransitionKind::Internal, c_always, c_count },
    { 1, 78U, 1, coact::TransitionKind::Internal, c_always, c_count },
};

struct CountTraits {
    static coact::LogicalPrio logical_prio() { return 31U; }
    static coact::PriorityClass priority_class()
    {
        return coact::PriorityClass::Normal;
    }
    static bool direct_eligible() { return true; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using CountAo = coact::Ao<CountCtx, coact::Hsm<CountCtx>, CountTraits>;

using PoolT = coact::EventPool<32U, 64U>;
using Rt = coact::Runtime<coact::DefaultConfig, coact::pal::Posix>;

/* Two registries share the runtime coordinator: one for the group
   aggregate tasks (GroupStatus result), one for the input tasks
   (uint32_t result). */
using GroupReg = coact::coro::GroupRegistry<PoolT, Rt::CoordinatorType, 8U>;
using InputReg = coact::coro::TaskRegistry<uint32_t, PoolT,
                                            Rt::CoordinatorType, 8U>;

using Cap = std::integral_constant<uint16_t, 3U>;
using G3 = coact::coro::TaskGroup<coact::coro::GroupStatus, GroupReg, 3U>;

alignas(16) static unsigned char g_storage[32U * 64U + 32U];

struct Rig {
    CountAo ao;
    PoolT pool;
    coact::pal::Posix pal;
    Rt rt;
    GroupReg groups;
    InputReg inputs;

    Rig()
        : ao(kCStates, 2U, kCTrans, 2U, 1, 2U),
          pool(),
          pal(),
          rt(pal),
          groups(pool, rt.coordinator()),
          inputs(pool, rt.coordinator())
    {
        pool.init(g_storage, sizeof(g_storage), coact::detail::noop_cs());
        coact::Event init_e;
        init_e.signal = 0U;
        init_e.pool_id = 0U;
        init_e.ref_ctr = 0U;
        ao.init(init_e);
        (void)rt.bind(&ao);
        (void)rt.initialize();
        (void)rt.start();
    }

    ~Rig() { rt.stop(); }

    /* Event-count-based drain (no fixed sleeps). */
    void drain(unsigned expected_events)
    {
        for (int32_t spin = 0; spin < 20000; ++spin) {
            if (ao.context().group_events >= expected_events) {
                break;
            }
            usleep(200U);
        }
    }
};

}  // namespace

/* when_all: three inputs, out-of-order completion - the group settles only
   after the last one (plan Task 5 Step 1). */
COACT_TEST(when_all_waits_for_every_input)
{
    Rig rig;
    std::array<coact::coro::TaskId, 3U> ids{};
    std::array<coact::coro::Promise<uint32_t, InputReg>, 3U> promises{};

    for (uint16_t i = 0U; i < 3U; ++i) {
        auto exp = rig.inputs.create(coact::kInvalidTarget, 0U,
                                     coact::EventQos{false, false});
        REQUIRE(static_cast<bool>(exp));
        auto pair = std::move(exp.value());
        ids[i] = pair.task.id();
        promises[i] = std::move(pair.promise);
    }

    auto g = coact::coro::when_all<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, ids, 3U, coact::TargetId(1U), 77U);
    REQUIRE(static_cast<bool>(g));

    /* Complete input 2 (out of order): not settled. */
    REQUIRE(static_cast<bool>(promises[2].complete(22U)));
    auto r1 = coact::coro::evaluate_all<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, g.value());
    REQUIRE(static_cast<bool>(r1));
    CHECK(!r1.value());

    /* Complete input 0: still not settled. */
    REQUIRE(static_cast<bool>(promises[0].complete(10U)));
    auto r2 = coact::coro::evaluate_all<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, g.value());
    REQUIRE(static_cast<bool>(r2));
    CHECK(!r2.value());

    /* Complete input 1: settles with succeeded == 3. */
    REQUIRE(static_cast<bool>(promises[1].complete(11U)));
    auto r3 = coact::coro::evaluate_all<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, g.value());
    REQUIRE(static_cast<bool>(r3));
    CHECK(r3.value());

    /* Aggregate result: all three succeeded, no error. */
    auto res = rig.groups.take_result(g.value().group_id());
    REQUIRE(static_cast<bool>(res));
    CHECK_EQ(3U, res.value().succeeded);
    CHECK_EQ(0U, res.value().failed);

    /* Exactly one aggregate event reached the waiter AO. */
    CHECK_EQ(1U, rig.ao.context().group_events);

    /* Idempotent: a further evaluate reports settled without a second
       completion event. */
    auto r4 = coact::coro::evaluate_all<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, g.value());
    REQUIRE(static_cast<bool>(r4));
    CHECK(r4.value());
    CHECK_EQ(1U, rig.ao.context().group_events);
}

/* when_all: a failure settles the group as failed with the first error. */
COACT_TEST(when_all_first_error_settles_failed)
{
    Rig rig;
    std::array<coact::coro::TaskId, 3U> ids{};
    std::array<coact::coro::Promise<uint32_t, InputReg>, 3U> promises{};

    for (uint16_t i = 0U; i < 3U; ++i) {
        auto exp = rig.inputs.create(coact::kInvalidTarget, 0U,
                                     coact::EventQos{false, false});
        REQUIRE(static_cast<bool>(exp));
        auto pair = std::move(exp.value());
        ids[i] = pair.task.id();
        promises[i] = std::move(pair.promise);
    }

    auto g = coact::coro::when_all<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, ids, 3U, coact::TargetId(1U), 77U);
    REQUIRE(static_cast<bool>(g));

    /* Input 1 fails first. */
    REQUIRE(static_cast<bool>(
        promises[1].fail(coact::coro::TaskError::kTargetRejected)));
    auto r = coact::coro::evaluate_all<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, g.value());
    REQUIRE(static_cast<bool>(r));
    CHECK(!r.value());   /* when_all waits for ALL completions, error or not:
                            not settled yet with 2 inputs pending. */

    REQUIRE(static_cast<bool>(promises[0].complete(1U)));
    auto r2 = coact::coro::evaluate_all<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, g.value());
    REQUIRE(static_cast<bool>(r2));
    CHECK(!r2.value());

    REQUIRE(static_cast<bool>(promises[2].complete(2U)));
    auto r3 = coact::coro::evaluate_all<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, g.value());
    REQUIRE(static_cast<bool>(r3));
    CHECK(r3.value());

    auto res = rig.groups.take_result(g.value().group_id());
    REQUIRE(static_cast<bool>(res));
    CHECK_EQ(2U, res.value().succeeded);
    CHECK_EQ(1U, res.value().failed);
    CHECK(coact::coro::TaskError::kTargetRejected == res.value().first_error);
    CHECK_EQ(1U, rig.ao.context().group_events);
}

/* when_any: settles on the first input completion. */
COACT_TEST(when_any_settles_on_first_completion)
{
    Rig rig;
    std::array<coact::coro::TaskId, 3U> ids{};
    std::array<coact::coro::Promise<uint32_t, InputReg>, 3U> promises{};

    for (uint16_t i = 0U; i < 3U; ++i) {
        auto exp = rig.inputs.create(coact::kInvalidTarget, 0U,
                                     coact::EventQos{false, false});
        REQUIRE(static_cast<bool>(exp));
        auto pair = std::move(exp.value());
        ids[i] = pair.task.id();
        promises[i] = std::move(pair.promise);
    }

    auto g = coact::coro::when_any<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, ids, 3U, coact::TargetId(1U), 78U);
    REQUIRE(static_cast<bool>(g));

    auto r0 = coact::coro::evaluate_any<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, g.value());
    REQUIRE(static_cast<bool>(r0));
    CHECK(!r0.value());

    /* First completion (input 2, out of order) settles immediately. */
    REQUIRE(static_cast<bool>(promises[2].complete(22U)));
    auto r1 = coact::coro::evaluate_any<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, g.value());
    REQUIRE(static_cast<bool>(r1));
    CHECK(r1.value());

    auto res = rig.groups.take_result(g.value().group_id());
    REQUIRE(static_cast<bool>(res));
    CHECK_EQ(1U, res.value().succeeded);
    rig.drain(1U);
    CHECK_EQ(1U, rig.ao.context().group_events);
}

/* when_some: settles when the threshold is reached (2 of 3). */
COACT_TEST(when_some_threshold_two_of_three)
{
    Rig rig;
    std::array<coact::coro::TaskId, 3U> ids{};
    std::array<coact::coro::Promise<uint32_t, InputReg>, 3U> promises{};

    for (uint16_t i = 0U; i < 3U; ++i) {
        auto exp = rig.inputs.create(coact::kInvalidTarget, 0U,
                                     coact::EventQos{false, false});
        REQUIRE(static_cast<bool>(exp));
        auto pair = std::move(exp.value());
        ids[i] = pair.task.id();
        promises[i] = std::move(pair.promise);
    }

    auto g = coact::coro::when_some<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, ids, 3U, 2U, coact::TargetId(1U), 78U);
    REQUIRE(static_cast<bool>(g));

    auto r0 = coact::coro::evaluate_some<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, g.value(), 2U);
    REQUIRE(static_cast<bool>(r0));
    CHECK(!r0.value());

    REQUIRE(static_cast<bool>(promises[0].complete(1U)));
    auto r1 = coact::coro::evaluate_some<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, g.value(), 2U);
    REQUIRE(static_cast<bool>(r1));
    CHECK(!r1.value());   /* 1 of 2: below threshold. */

    REQUIRE(static_cast<bool>(promises[1].complete(2U)));
    auto r2 = coact::coro::evaluate_some<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, g.value(), 2U);
    REQUIRE(static_cast<bool>(r2));
    CHECK(r2.value());    /* 2 of 2: settled. */

    auto res = rig.groups.take_result(g.value().group_id());
    REQUIRE(static_cast<bool>(res));
    CHECK_EQ(2U, res.value().succeeded);
    rig.drain(1U);
    CHECK_EQ(1U, rig.ao.context().group_events);
}

/* Builder error paths: zero count, count over capacity, invalid task id. */
COACT_TEST(combinator_builder_error_paths)
{
    Rig rig;
    std::array<coact::coro::TaskId, 3U> ids{};

    auto zero = coact::coro::when_all<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, ids, 0U, coact::TargetId(1U), 77U);
    CHECK(!static_cast<bool>(zero));
    CHECK(coact::coro::GroupError::kZeroCount == zero.error());

    auto over = coact::coro::when_all<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, ids, 4U, coact::TargetId(1U), 77U);
    CHECK(!static_cast<bool>(over));
    CHECK(coact::coro::GroupError::kTooManyInputs == over.error());

    ids[0] = coact::coro::kInvalidTaskId;
    ids[1] = coact::coro::kInvalidTaskId;
    ids[2] = coact::coro::kInvalidTaskId;
    auto invalid = coact::coro::when_all<PoolT, Rt::CoordinatorType, 3U>(
        rig.groups, rig.inputs, ids, 3U, coact::TargetId(1U), 77U);
    CHECK(!static_cast<bool>(invalid));
    CHECK(coact::coro::GroupError::kInvalidTask == invalid.error());
}

COACT_TEST_MAIN()
