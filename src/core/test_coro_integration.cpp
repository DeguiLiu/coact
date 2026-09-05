// coact::coro Runtime + AO/HSM integration (plan Task 7): success, fail,
// cancel and an explicit reject arc. Drain is pending()/event-count based.
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <cstdint>
#include <unistd.h>
#include <utility>

#include "coact/ao.hpp"
#include "coact/coro/coro.hpp"
#include "coact/hsm.hpp"
#include "coact/pal_posix.hpp"
#include "coact/pool.hpp"
#include "coact/runtime.hpp"

namespace {

enum : uint16_t {
    kKick = 1U,
    kDone = 2U,
    kBogus = 99U
};

enum : int8_t { kRoot = 0, kIdle = 1, kBusy = 2 };

struct Ctx {
    unsigned kicks = 0U;
    unsigned success = 0U;
    unsigned failed = 0U;
    unsigned cancelled = 0U;
    unsigned rejected = 0U;
};

static void noop(Ctx&) {}
static bool always(const Ctx&, const coact::Event&) { return true; }

static void on_kick(Ctx& c, const coact::Event&) { c.kicks++; }
static void on_done(Ctx& c, const coact::Event& e)
{
    const coact::coro::CompletionEventPayload p =
        coact::coro::decode_completion(e);
    if (coact::coro::CompletionStatus::kSucceeded == p.status) {
        c.success++;
    } else if (coact::coro::CompletionStatus::kCancelled == p.status) {
        c.cancelled++;
    } else {
        c.failed++;
    }
}
static void on_reject(Ctx& c, const coact::Event&) { c.rejected++; }

static const coact::StateDef<Ctx> kStates[] = {
    { -1, nullptr, nullptr, "Root", kIdle },
    { kRoot, noop, noop, "Idle" },
    { kRoot, noop, noop, "Busy" },
};
static const coact::TransitionDef<Ctx> kTrans[] = {
    { kIdle, kKick, kBusy, coact::TransitionKind::External, always, on_kick },
    { kBusy, kDone, kIdle, coact::TransitionKind::External, always, on_done },
    { kIdle, kBogus, kIdle, coact::TransitionKind::Internal, always, on_reject },
    { kBusy, kBogus, kBusy, coact::TransitionKind::Internal, always, on_reject },
};

struct Traits {
    static coact::LogicalPrio logical_prio() { return 28U; }
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

alignas(16) static unsigned char g_storage[32U * 64U + 32U];

struct Rig {
    WorkerAo ao;
    PoolT pool;
    coact::pal::Posix pal;
    Rt rt;
    Reg reg;

    Rig()
        : ao(kStates, 3U, kTrans, 4U, kIdle, 2U),
          pool(),
          pal(),
          rt(pal),
          reg(pool, rt.coordinator())
    {
        pool.init(g_storage, sizeof(g_storage), coact::detail::noop_cs());
        coact::Event init_e{};
        init_e.signal = 0U;
        ao.init(init_e);
        (void)rt.bind(&ao);
        (void)rt.initialize();
        (void)rt.start();
    }

    ~Rig() { rt.stop(); }

    void submit(uint16_t sig)
    {
        coact::Event* e = pool.alloc(sig);
        REQUIRE(nullptr != e);
        (void)rt.coordinator().submit_from_task(
            coact::TargetId(1U), e, coact::EventQos{false, false});
    }

    void drain_pending()
    {
        for (int32_t w = 0; w < 2000; ++w) {
            if (0U == ao.pending().load()) {
                break;
            }
            usleep(200);
        }
    }
};

}  // namespace

COACT_TEST(integration_success_fail_cancel_and_reject)
{
    Rig rig;
    coact::EventQos qos{false, false};

    auto ok = rig.reg.create(coact::TargetId(1U), kDone, qos);
    REQUIRE(static_cast<bool>(ok));
    auto ok_pair = std::move(ok.value());
    rig.submit(kKick);
    REQUIRE(static_cast<bool>(ok_pair.promise.complete(7U)));
    CHECK_EQ(1U, rig.ao.context().kicks);
    CHECK_EQ(1U, rig.ao.context().success);

    auto bad = rig.reg.create(coact::TargetId(1U), kDone, qos);
    REQUIRE(static_cast<bool>(bad));
    auto bad_pair = std::move(bad.value());
    rig.submit(kKick);
    REQUIRE(static_cast<bool>(
        bad_pair.promise.fail(coact::coro::TaskError::kTargetRejected)));
    CHECK_EQ(1U, rig.ao.context().failed);

    auto can = rig.reg.create(coact::TargetId(1U), kDone, qos);
    REQUIRE(static_cast<bool>(can));
    auto can_pair = std::move(can.value());
    rig.submit(kKick);
    REQUIRE(static_cast<bool>(can_pair.promise.cancel()));
    CHECK_EQ(1U, rig.ao.context().cancelled);

    rig.submit(kBogus);
    CHECK_EQ(1U, rig.ao.context().rejected);

    REQUIRE(static_cast<bool>(ok_pair.task.release()));
    REQUIRE(static_cast<bool>(bad_pair.task.release()));
    REQUIRE(static_cast<bool>(can_pair.task.release()));
    rig.drain_pending();
    CHECK_EQ(0U, rig.reg.used());
    CHECK_EQ(0U, rig.pool.used());
}

COACT_TEST_MAIN()
