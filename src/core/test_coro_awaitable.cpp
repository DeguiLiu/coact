// coact::coro AwaitableRef + completion-event bridge tests (plan Task 4).
// Immediate vs delayed completion, error take, duplicate awaiter rejection,
// CompletionSourceTraits glue. Single-threaded: the AO is direct-eligible so
// completion events dispatch on the calling thread (no fixed sleeps).
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <cstdint>
#include <utility>

#include "coact/ao.hpp"
#include "coact/coro/awaitable.hpp"
#include "coact/coro/coro.hpp"
#include "coact/hsm.hpp"
#include "coact/pal_posix.hpp"
#include "coact/pool.hpp"
#include "coact/runtime.hpp"

namespace {

enum : uint16_t { kDone = 42U };

struct Ctx {
    unsigned events = 0U;
    uint8_t last_status = 0xFFU;
    coact::coro::TaskError last_error = coact::coro::TaskError::kOk;
};

static void noop_entry(Ctx&) {}
static void noop_exit(Ctx&) {}
static bool always(const Ctx&, const coact::Event&) { return true; }

static void on_done(Ctx& c, const coact::Event& e)
{
    const coact::coro::CompletionEventPayload p =
        coact::coro::decode_completion(e);
    c.events++;
    c.last_status = static_cast<uint8_t>(p.status);
    c.last_error = p.error;
}

static const coact::StateDef<Ctx> kStates[] = {
    { -1, nullptr, nullptr, nullptr, 1 },
    {  0, noop_entry, noop_exit, "Active" },
};
static const coact::TransitionDef<Ctx> kTrans[] = {
    { 1, kDone, 1, coact::TransitionKind::Internal, always, on_done },
};

struct Traits {
    static coact::LogicalPrio logical_prio() { return 30U; }
    static coact::PriorityClass priority_class()
    {
        return coact::PriorityClass::Normal;
    }
    static bool direct_eligible() { return true; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using WorkerAo = coact::Ao<Ctx, coact::Hsm<Ctx>, Traits>;
using PoolT = coact::EventPool<32U, 64U>;
using Rt = coact::Runtime<coact::DefaultConfig, coact::pal::Posix>;
using Reg = coact::coro::TaskRegistry<uint32_t, PoolT, Rt::CoordinatorType, 4U>;
using Await = coact::coro::AwaitableRef<uint32_t, Reg>;

alignas(16) static unsigned char g_storage[32U * 64U + 32U];

struct Rig {
    WorkerAo ao;
    PoolT pool;
    coact::pal::Posix pal;
    Rt rt;
    Reg reg;

    Rig()
        : ao(kStates, 2U, kTrans, 1U, 1, 2U),
          pool(),
          pal(),
          rt(pal),
          reg(pool, rt.coordinator())
    {
        pool.init(g_storage, sizeof(g_storage));
        coact::Event init_e{};
        init_e.signal = 0U;
        ao.init(init_e);
        (void)rt.bind(&ao);
        (void)rt.initialize();
        (void)rt.start();
    }

    ~Rig() { rt.stop(); }
};

}  // namespace

COACT_TEST(awaitable_immediate_complete_then_observe)
{
    Rig rig;
    auto created = rig.reg.create(coact::kInvalidTarget, 0U,
                                  coact::EventQos{false, false});
    REQUIRE(static_cast<bool>(created));
    auto pair = std::move(created.value());

    REQUIRE(static_cast<bool>(pair.promise.complete(9U)));

    Await obs(pair.task);
    CHECK(obs.is_valid());
    CHECK(obs.is_ready());
    CHECK(!obs.has_error());
    CHECK_EQ(coact::coro::TaskError::kOk, obs.take_error());

    auto value = obs.take_result();
    REQUIRE(static_cast<bool>(value));
    CHECK_EQ(9U, value.value());

    auto again = obs.take_result();
    CHECK(!static_cast<bool>(again));
    CHECK(coact::coro::TaskError::kResultTaken == again.error());

    CHECK_EQ(0U, rig.ao.context().events);
    REQUIRE(static_cast<bool>(obs.release()));
    CHECK_EQ(0U, rig.reg.used());
    CHECK_EQ(0U, rig.pool.used());
}

COACT_TEST(awaitable_delayed_complete_posts_one_event)
{
    Rig rig;
    auto created = rig.reg.create(coact::kInvalidTarget, 0U,
                                  coact::EventQos{false, false});
    REQUIRE(static_cast<bool>(created));
    auto pair = std::move(created.value());

    Await obs(pair.task);
    CHECK(!obs.is_ready());

    using TraitsT = coact::coro::CompletionSourceTraits<Reg>;
    REQUIRE(static_cast<bool>(
        TraitsT::bind_waiter(rig.reg, obs.id(), coact::TargetId(1U), kDone)));

    const auto dup = obs.set_awaiter(coact::TargetId(1U), kDone);
    CHECK(!static_cast<bool>(dup));
    CHECK(coact::coro::TaskError::kAwaiterBusy == dup.error());

    REQUIRE(static_cast<bool>(pair.promise.complete(21U)));
    CHECK(obs.is_ready());
    CHECK_EQ(1U, rig.ao.context().events);
    CHECK_EQ(0U, rig.ao.context().last_status);

    auto value = obs.take_result();
    REQUIRE(static_cast<bool>(value));
    CHECK_EQ(21U, value.value());
    REQUIRE(static_cast<bool>(obs.release()));
    CHECK_EQ(0U, rig.pool.used());
}

COACT_TEST(awaitable_error_propagation_and_decode)
{
    Rig rig;
    auto created = rig.reg.create(coact::TargetId(1U), kDone,
                                  coact::EventQos{false, false});
    REQUIRE(static_cast<bool>(created));
    auto pair = std::move(created.value());
    Await obs(pair.task);

    REQUIRE(static_cast<bool>(
        pair.promise.fail(coact::coro::TaskError::kTargetRejected)));
    CHECK(obs.is_ready());
    CHECK(obs.has_error());
    CHECK_EQ(coact::coro::TaskError::kTargetRejected, obs.take_error());
    CHECK_EQ(1U, rig.ao.context().events);
    CHECK_EQ(1U, rig.ao.context().last_status);
    CHECK_EQ(coact::coro::TaskError::kTargetRejected,
             rig.ao.context().last_error);

    auto value = obs.take_result();
    CHECK(!static_cast<bool>(value));
    CHECK(coact::coro::TaskError::kTargetRejected == value.error());
    REQUIRE(static_cast<bool>(obs.release()));
    CHECK_EQ(0U, rig.pool.used());
}

COACT_TEST(awaitable_moved_from_is_invalid)
{
    Rig rig;
    auto created = rig.reg.create(coact::kInvalidTarget, 0U,
                                  coact::EventQos{false, false});
    REQUIRE(static_cast<bool>(created));
    auto pair = std::move(created.value());
    Await obs(pair.task);
    Await moved = std::move(obs);
    CHECK(!obs.is_valid());
    CHECK(moved.is_valid());
    CHECK(coact::coro::TaskError::kInvalidId == obs.take_error());
    auto bad = obs.take_result();
    CHECK(!static_cast<bool>(bad));
    CHECK(coact::coro::TaskError::kInvalidId == bad.error());
    REQUIRE(static_cast<bool>(pair.promise.complete(1U)));
    REQUIRE(static_cast<bool>(moved.release()));
    CHECK_EQ(0U, rig.reg.used());
}

COACT_TEST_MAIN()
