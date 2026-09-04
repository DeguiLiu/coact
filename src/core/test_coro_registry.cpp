// coact::coro task registry test: fixed-slot lifecycle, TaskId generation
// guard, handle moves, complete/cancel/double-complete rejections, waiter
// registration, result take semantics and slot recycle.
//
// Execution model: the coact Dispatcher thread is the ONLY thread the rig
// starts (the event plane). Completions direct-dispatch when possible and
// stage to the Dispatcher otherwise; drains are event-count based (no fixed
// sleeps). The test thread itself creates no additional threads.
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <array>
#include <cstdint>
#include <type_traits>
#include <unistd.h>
#include <utility>

#include "coact/ao.hpp"
#include "coact/coro/coro.hpp"
#include "coact/coro/config.hpp"
#include "coact/coro/detail/fixed_storage.hpp"
#include "coact/coro/error.hpp"
#include "coact/coro/task_id.hpp"
#include "coact/coordinator.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/monitor.hpp"
#include "coact/pal_posix.hpp"
#include "coact/pool.hpp"
#include "coact/runtime.hpp"
#include "coact/staging.hpp"

namespace {

/* ---------------------------------------------------------------------- */
/* Compile-time contract tests (plan Task 2 Step 1).                       */
/* ---------------------------------------------------------------------- */
using coact::coro::AsyncConfig;
using coact::coro::TaskId;

static_assert(std::is_standard_layout<TaskId>::value,
              "TaskId must be standard layout");
static_assert(std::is_trivially_copyable<TaskId>::value,
              "TaskId must be trivially copyable");
static_assert(sizeof(TaskId) == sizeof(uint16_t),
              "TaskId must be zero-overhead (2 bytes)");
static_assert(coact::coro::is_coro_result_v<uint32_t>,
              "uint32_t must be a valid coro result");
static_assert(coact::coro::is_coro_result_v<coact::coro::GroupStatus>,
              "GroupStatus must be a valid coro result");
static_assert(!coact::coro::is_coro_result_v<std::string>,
              "std::string must be rejected (heap)");
static_assert(AsyncConfig::kDefaultGroupCapacity <= 32U,
              "group bitmap bound");

static_assert(coact::coro::detail::FixedStorageContract<uint64_t>::value,
              "FixedStorage accepts uint64_t");
static_assert(
    !coact::coro::detail::FixedStorageContract<std::vector<int>>::value,
    "FixedStorage must reject non-trivial types");

static_assert(std::is_standard_layout<coact::coro::CompletionPayload>::value,
              "completion payload must be standard layout");

/* TaskId packing checks. */
static_assert(coact::coro::slot_of(coact::coro::make_task_id(
                  coact::coro::TaskSlotId(3U), 5U)).value == 3U,
              "TaskId low bits are the slot index");
static_assert(coact::coro::generation_of(coact::coro::make_task_id(
                  coact::coro::TaskSlotId(3U), 5U)) == 5U,
              "TaskId high bits are the generation");
static_assert(!coact::coro::kInvalidTaskId, "kInvalidTaskId is falsy");

/* ---------------------------------------------------------------------- */
/* Test rig: real coact plumbing, single-threaded. The runtime is bound and
   initialized but NEVER started (no dispatcher thread): the AO is
   direct-eligible, so coordinator submissions direct-dispatch in the
   calling thread and the completion event reaches the HSM synchronously. */
/* ---------------------------------------------------------------------- */
struct TrivialCtx {
    unsigned completed_events = 0U;
    uint16_t last_payload_task_id = 0U;
    uint8_t last_payload_status = 0xFFU;
};

static void t_noop_entry(TrivialCtx&) {}
static void t_noop_exit(TrivialCtx&) {}
static bool t_always(const TrivialCtx&, const coact::Event&) { return true; }

static void t_count(TrivialCtx& c, const coact::Event& e)
{
    c.completed_events++;
    /* The Event& handed to the action is a CompletionBlock (Event at
       offset 0): decode through the awaitable bridge. */
    const coact::coro::CompletionEventPayload p =
        coact::coro::decode_completion(e);
    c.last_payload_task_id = p.id.value();
    c.last_payload_status = static_cast<uint8_t>(p.status);
}

static const coact::StateDef<TrivialCtx> kStates[] = {
    /* 0: root */ { -1, nullptr, nullptr, nullptr, 1 },
    /* 1: S1   */ {  0, t_noop_entry, t_noop_exit },
};
static const coact::TransitionDef<TrivialCtx> kTrans[] = {
    { 1, 42U, 1, coact::TransitionKind::Internal, t_always, t_count },
};

struct AoTraits {
    static coact::LogicalPrio logical_prio() { return 30U; }
    static coact::PriorityClass priority_class()
    {
        return coact::PriorityClass::Normal;
    }
    static bool direct_eligible() { return true; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using WorkerAo = coact::Ao<TrivialCtx, coact::Hsm<TrivialCtx>, AoTraits>;

using PoolT = coact::EventPool<32U, 64U>;
using Rt = coact::Runtime<coact::DefaultConfig, coact::pal::Posix>;
using RegU32 = coact::coro::TaskRegistry<uint32_t, PoolT,
                                          Rt::CoordinatorType, 2U>;

alignas(16) static unsigned char g_storage[32U * 64U + 32U];

struct Rig {
    WorkerAo ao;
    PoolT pool;
    coact::pal::Posix pal;
    Rt rt;
    RegU32 reg;

    Rig()
        : ao(kStates, 2U, kTrans, 1U, 1, 2U),
          pool(),
          pal(),
          rt(pal),
          reg(pool, rt.coordinator())
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

    /* Event-count-based drain (no fixed sleeps): wait until the AO observed
       the expected number of completion events. */
    void drain(unsigned expected_events)
    {
        for (int spin = 0; spin < 20000; ++spin) {
            if (ao.context().completed_events >= expected_events) {
                break;
            }
            usleep(200U);
        }
    }
};

}  // namespace

/* ---------------------------------------------------------------------- */
/* Lifecycle: create -> complete -> take_result -> release; used() == 0.    */
/* ---------------------------------------------------------------------- */
COACT_TEST(task_basic_complete_and_release)
{
    Rig rig;
    auto created = rig.reg.create(coact::TargetId(1U), 42U,
                                  coact::EventQos{false, false});
    REQUIRE(static_cast<bool>(created));
    CHECK_EQ(1U, rig.reg.used());

    auto pair = std::move(created.value());
    auto promise = std::move(pair.promise);
    auto task = std::move(pair.task);
    REQUIRE(task.is_valid());
    CHECK(static_cast<bool>(task.id()));

    CHECK(!task.is_ready());
    const auto done = promise.complete(7U);
    REQUIRE(static_cast<bool>(done));
    CHECK(task.is_ready());
    CHECK(!task.has_error());

    /* The completion event reached the AO (direct or via the Dispatcher). */
    rig.drain(1U);
    CHECK_EQ(1U, rig.ao.context().completed_events);
    CHECK_EQ(task.id().value(), rig.ao.context().last_payload_task_id);

    auto res = rig.reg.take_result(task.id());
    REQUIRE(static_cast<bool>(res));
    CHECK_EQ(7U, res.value());

    /* Second take is an explicit rejection. */
    const auto again = rig.reg.take_result(task.id());
    CHECK(!static_cast<bool>(again));
    CHECK(coact::coro::TaskError::kResultTaken == again.error());

    const auto rel = task.release();
    REQUIRE(static_cast<bool>(rel));
    CHECK(!task.is_valid());
    CHECK_EQ(0U, rig.reg.used());
    CHECK_EQ(0U, rig.pool.used());
}

/* Capacity 2: the third create fails with kSlotsFull (plan Task 3 Step 1). */
COACT_TEST(task_slots_full_rejection)
{
    Rig rig;
    auto a_exp = rig.reg.create(coact::TargetId(1U), 42U, coact::EventQos{false, false});
    REQUIRE(static_cast<bool>(a_exp));
    auto a = std::move(a_exp.value());
    auto b_exp = rig.reg.create(coact::TargetId(1U), 42U, coact::EventQos{false, false});
    REQUIRE(static_cast<bool>(b_exp));
    auto b = std::move(b_exp.value());
    (void)b;
    CHECK_EQ(2U, rig.reg.used());

    auto third = rig.reg.create(coact::TargetId(1U), 42U,
                                coact::EventQos{false, false});
    CHECK(!static_cast<bool>(third));
    CHECK(coact::coro::TaskError::kSlotsFull == third.error());

    /* Releasing one slot makes room again. */
    const auto rel = a.task.release();
    CHECK(static_cast<bool>(rel));
    auto again = rig.reg.create(coact::TargetId(1U), 42U,
                                coact::EventQos{false, false});
    CHECK(static_cast<bool>(again));
    CHECK_EQ(0U, rig.reg.submit_failures());
}

/* Handle move semantics: source handle invalid, target keeps the id. */
COACT_TEST(task_handle_move_semantics)
{
    Rig rig;
    auto pair_exp = rig.reg.create(coact::kInvalidTarget, 0U, coact::EventQos{false, false});
    REQUIRE(static_cast<bool>(pair_exp));
    auto pair = std::move(pair_exp.value());
    const coact::coro::TaskId id = pair.task.id();

    auto moved = std::move(pair.task);
    CHECK(!pair.task.is_valid());
    CHECK(moved.is_valid());
    CHECK(id == moved.id());

    /* The moved-from registry_ is nullptr: operations fail safely. */
    const auto bad = pair.task.set_awaiter(coact::TargetId(1U), 1U);
    CHECK(!static_cast<bool>(bad));
    CHECK(coact::coro::TaskError::kInvalidId == bad.error());

    CHECK(moved.is_valid());
    (void)moved.release();
    CHECK_EQ(0U, rig.reg.used());
}

/* Cancel a waiting task, then attempt completion: kCancelled rejection,
   waiter receives the cancellation event, rejection counter incremented. */
COACT_TEST(task_cancel_and_post_cancel_complete)
{
    Rig rig;
    auto pair_exp = rig.reg.create(coact::TargetId(1U), 42U, coact::EventQos{false, false});
    REQUIRE(static_cast<bool>(pair_exp));
    auto pair = std::move(pair_exp.value());
    auto promise = std::move(pair.promise);
    auto task = std::move(pair.task);

    const auto set = task.set_awaiter(coact::TargetId(1U), 42U);
    CHECK(static_cast<bool>(set));

    const auto cancelled = promise.cancel();
    REQUIRE(static_cast<bool>(cancelled));
    CHECK(task.is_ready());
    CHECK(task.has_error());
    CHECK_EQ(coact::coro::TaskError::kCancelled,
             rig.reg.take_error(task.id()));
    /* Cancellation event delivered (kCancelled status). */
    rig.drain(1U);
    rig.drain(1U);
    CHECK_EQ(1U, rig.ao.context().completed_events);
    CHECK_EQ(2U, rig.ao.context().last_payload_status);

    /* Completing after cancel: counted rejection, state untouched. */
    const auto late = promise.complete(9U);
    CHECK(!static_cast<bool>(late));
    CHECK_EQ(coact::coro::TaskError::kCancelled, late.error());
    CHECK_EQ(1U, rig.reg.rejected_count());
    CHECK_EQ(1U, rig.ao.context().completed_events);

    (void)task.release();
    CHECK_EQ(0U, rig.reg.used());
}

/* Double completion is a counted rejection; the stored result survives. */
COACT_TEST(task_double_complete_rejection)
{
    Rig rig;
    auto pair_exp = rig.reg.create(coact::kInvalidTarget, 0U, coact::EventQos{false, false});
    REQUIRE(static_cast<bool>(pair_exp));
    auto pair = std::move(pair_exp.value());
    auto promise = std::move(pair.promise);
    auto task = std::move(pair.task);

    REQUIRE(static_cast<bool>(promise.complete(11U)));
    const auto twice = promise.complete(12U);
    CHECK(!static_cast<bool>(twice));
    CHECK(coact::coro::TaskError::kAlreadyCompleted == twice.error());
    CHECK_EQ(1U, rig.reg.rejected_count());

    /* First result preserved. */
    auto res = rig.reg.take_result(task.id());
    REQUIRE(static_cast<bool>(res));
    CHECK_EQ(11U, res.value());

    (void)task.release();
}

/* set_awaiter: second registration rejected (kAwaiterBusy), completion
   after waiting delivers exactly one event. */
COACT_TEST(task_set_awaiter_single_registration)
{
    Rig rig;
    auto pair_exp = rig.reg.create(coact::kInvalidTarget, 0U, coact::EventQos{false, false});
    REQUIRE(static_cast<bool>(pair_exp));
    auto pair = std::move(pair_exp.value());
    auto promise = std::move(pair.promise);
    auto task = std::move(pair.task);

    REQUIRE(static_cast<bool>(task.set_awaiter(coact::TargetId(1U), 42U)));
    const auto dup = task.set_awaiter(coact::TargetId(1U), 42U);
    CHECK(!static_cast<bool>(dup));
    CHECK(coact::coro::TaskError::kAwaiterBusy == dup.error());
    CHECK_EQ(1U, rig.reg.rejected_count());

    REQUIRE(static_cast<bool>(promise.complete(21U)));
    /* set_awaiter on the completed task is rejected (no late binding). */
    const auto late = task.set_awaiter(coact::TargetId(1U), 42U);
    CHECK(!static_cast<bool>(late));
    CHECK(coact::coro::TaskError::kAlreadyCompleted == late.error());

    CHECK_EQ(1U, rig.ao.context().completed_events);
    CHECK(static_cast<uint8_t>(coact::coro::CompletionStatus::kSucceeded) == 0U);

    (void)task.release();
    CHECK_EQ(0U, rig.pool.used());
}

/* Generation guard: a stale handle (released slot) cannot alias the
   recycled incarnation. */
COACT_TEST(task_generation_guard_stale_handle)
{
    Rig rig;
    auto first_exp = rig.reg.create(coact::kInvalidTarget, 0U, coact::EventQos{false, false});
    REQUIRE(static_cast<bool>(first_exp));
    auto first = std::move(first_exp.value());
    const coact::coro::TaskId stale_id = first.task.id();

    /* Keep a promise copy of the id by failing through the promise before
       release, then release and re-create in the same slot. */
    REQUIRE(static_cast<bool>(first.promise.complete(1U)));
    REQUIRE(static_cast<bool>(first.task.release()));

    auto second_exp = rig.reg.create(coact::kInvalidTarget, 0U, coact::EventQos{false, false});
    REQUIRE(static_cast<bool>(second_exp));
    auto second = std::move(second_exp.value());
    CHECK_EQ(coact::coro::slot_of(stale_id).value,
             coact::coro::slot_of(second.task.id()).value);
    CHECK(coact::coro::generation_of(stale_id) !=
          coact::coro::generation_of(second.task.id()));

    /* The fresh incarnation is live and independent. */
    REQUIRE(static_cast<bool>(second.promise.complete(2U)));

    /* Stale id operations fail with kInvalidId. */
    CHECK(!rig.reg.is_ready(stale_id));
    const auto bad_complete = rig.reg.complete(stale_id, 5U);
    CHECK(!static_cast<bool>(bad_complete));
    CHECK(coact::coro::TaskError::kInvalidId == bad_complete.error());
    const auto bad_release = rig.reg.release_task(stale_id);
    CHECK(!static_cast<bool>(bad_release));

    /* The live incarnation holds its own result. */
    CHECK(rig.reg.is_ready(second.task.id()));
    auto res = rig.reg.take_result(second.task.id());
    REQUIRE(static_cast<bool>(res));
    CHECK_EQ(2U, res.value());

    (void)second.task.release();
    CHECK_EQ(0U, rig.reg.used());
}

/* fail(): error stored, has_error true, take_result returns the error,
   waiter event carries the failure status. */
COACT_TEST(task_fail_path_error_propagation)
{
    Rig rig;
    auto pair_exp = rig.reg.create(coact::TargetId(1U), 42U, coact::EventQos{false, false});
    REQUIRE(static_cast<bool>(pair_exp));
    auto pair = std::move(pair_exp.value());
    auto promise = std::move(pair.promise);
    auto task = std::move(pair.task);

    REQUIRE(static_cast<bool>(
        promise.fail(coact::coro::TaskError::kTargetRejected)));
    CHECK(task.is_ready());
    CHECK(task.has_error());
    CHECK_EQ(coact::coro::TaskError::kTargetRejected,
             rig.reg.take_error(task.id()));

    auto res = rig.reg.take_result(task.id());
    CHECK(!static_cast<bool>(res));
    CHECK(coact::coro::TaskError::kTargetRejected == res.error());

    /* Failure event delivered (kFailed status). */
    rig.drain(1U);
    CHECK_EQ(1U, rig.ao.context().completed_events);
    CHECK_EQ(1U, rig.ao.context().last_payload_status);

    (void)task.release();
    CHECK_EQ(0U, rig.pool.used());
}

/* Slot recycling: release and re-create repeatedly, generations advance,
   no state bleeds between incarnations. */
COACT_TEST(task_slot_recycle_clean_state)
{
    Rig rig;
    for (int i = 0; i < 4; ++i) {
    auto pair_exp = rig.reg.create(coact::kInvalidTarget, 0U, coact::EventQos{false, false});
    REQUIRE(static_cast<bool>(pair_exp));
    auto pair = std::move(pair_exp.value());
        REQUIRE(static_cast<bool>(pair.promise.complete(
            static_cast<uint32_t>(i))));
        auto res = rig.reg.take_result(pair.task.id());
        REQUIRE(static_cast<bool>(res));
        CHECK_EQ(static_cast<uint32_t>(i), res.value());
        REQUIRE(static_cast<bool>(pair.task.release()));
    }
    CHECK_EQ(0U, rig.reg.used());
    CHECK_EQ(0U, rig.reg.rejected_count());
    CHECK_EQ(0U, rig.pool.used());
}

/* Invalid-id edge: kInvalidTaskId operations never touch slots. */
COACT_TEST(task_invalid_id_edges)
{
    Rig rig;
    const auto bad = rig.reg.complete(coact::coro::kInvalidTaskId, 1U);
    CHECK(!static_cast<bool>(bad));
    CHECK(coact::coro::TaskError::kInvalidId == bad.error());
    CHECK(!rig.reg.is_ready(coact::coro::kInvalidTaskId));
    CHECK_EQ(0U, rig.reg.used());
    CHECK_EQ(0U, rig.reg.rejected_count());
}

COACT_TEST_MAIN()
