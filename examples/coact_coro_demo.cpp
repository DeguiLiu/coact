// coact::coro single-thread AO/HSM demo (plan Task 7): create a task,
// wait on its completion event, then drain to pool.used()==0.
// SPDX-License-Identifier: MIT
#include <cstdint>
#include <cstdio>
#include <unistd.h>
#include <utility>

#include "coact/ao.hpp"
#include "coact/coro/coro.hpp"
#include "coact/hsm.hpp"
#include "coact/pal_posix.hpp"
#include "coact/pool.hpp"
#include "coact/runtime.hpp"

namespace {

enum : uint16_t { kStart = 1U, kDone = 2U, kReject = 99U };
enum : int8_t { kRoot = 0, kIdle = 1, kWait = 2, kDoneSt = 3 };

struct Ctx {
    unsigned started = 0U;
    unsigned completed = 0U;
    unsigned rejected = 0U;
    uint32_t result = 0U;
};

static void noop(Ctx&) {}
static bool always(const Ctx&, const coact::Event&) { return true; }
static void on_start(Ctx& c, const coact::Event&) { c.started++; }
static void on_done(Ctx& c, const coact::Event& e)
{
    const coact::coro::CompletionEventPayload p =
        coact::coro::decode_completion(e);
    if (coact::coro::CompletionStatus::kSucceeded == p.status) {
        c.completed++;
    }
}
static void on_reject(Ctx& c, const coact::Event&) { c.rejected++; }

static const coact::StateDef<Ctx> kStates[] = {
    { -1, nullptr, nullptr, "Root", kIdle },
    { kRoot, noop, noop, "Idle" },
    { kRoot, noop, noop, "Wait" },
    { kRoot, noop, noop, "Done" },
};
static const coact::TransitionDef<Ctx> kTrans[] = {
    { kIdle, kStart, kWait, coact::TransitionKind::External, always, on_start },
    { kWait, kDone, kDoneSt, coact::TransitionKind::External, always, on_done },
    { kIdle, kReject, kIdle, coact::TransitionKind::Internal, always, on_reject },
    { kWait, kReject, kWait, coact::TransitionKind::Internal, always, on_reject },
    { kDoneSt, kReject, kDoneSt, coact::TransitionKind::Internal, always, on_reject },
};

struct Traits {
    static coact::LogicalPrio logical_prio() { return 20U; }
    static coact::PriorityClass priority_class()
    {
        return coact::PriorityClass::Normal;
    }
    static bool direct_eligible() { return true; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using DemoAo = coact::Ao<Ctx, coact::Hsm<Ctx>, Traits>;
using PoolT = coact::EventPool<32U, 32U>;
using Rt = coact::Runtime<coact::DefaultConfig, coact::pal::Posix>;
using Reg = coact::coro::TaskRegistry<uint32_t, PoolT, Rt::CoordinatorType, 4U>;

}  // namespace

int main()
{
    coact::pal::Posix pal;
    alignas(16) static unsigned char storage[32U * 32U + 32U];
    PoolT pool;
    pool.init(storage, sizeof(storage));

    DemoAo ao(kStates, 4U, kTrans, 5U, kIdle, 2U);
    coact::Event init_e{};
    init_e.signal = 0U;
    ao.init(init_e);

    Rt rt(pal);
    if (!rt.bind(&ao)) {
        std::printf("bind failed\n");
        return 1;
    }
    (void)rt.initialize();
    (void)rt.start();

    Reg reg(pool, rt.coordinator());
    coact::EventQos qos{false, false};
    auto created = reg.create(coact::TargetId(1U), kDone, qos);
    if (!static_cast<bool>(created)) {
        std::printf("create failed\n");
        rt.stop();
        return 1;
    }
    auto pair = std::move(created.value());

    coact::Event* start = pool.alloc(kStart);
    if (nullptr == start) {
        rt.stop();
        return 1;
    }
    (void)rt.coordinator().submit_from_task(coact::TargetId(1U), start, qos);
    if (!static_cast<bool>(pair.promise.complete(42U))) {
        std::printf("complete failed\n");
        rt.stop();
        return 1;
    }

    coact::Event* bogus = pool.alloc(kReject);
    if (nullptr != bogus) {
        (void)rt.coordinator().submit_from_task(coact::TargetId(1U), bogus, qos);
    }

    auto taken = pair.task.is_ready()
        ? reg.take_result(pair.task.id())
        : coact::Expected<uint32_t, coact::coro::TaskError>::error(
              coact::coro::TaskError::kResultUnavailable);
    if (static_cast<bool>(taken)) {
        ao.context().result = taken.value();
    }
    (void)pair.task.release();

    for (int w = 0; w < 200; ++w) {
        if (0U == ao.pending().load()) {
            break;
        }
        usleep(1000);
    }
    rt.stop();

    const bool pass = (1U == ao.context().started) &&
                      (1U == ao.context().completed) &&
                      (1U == ao.context().rejected) &&
                      (42U == ao.context().result) &&
                      (0U == reg.used()) &&
                      (0U == pool.used());
    std::printf("coact_coro_demo: started=%u completed=%u rejected=%u result=%u "
                "reg.used=%u pool.used=%u -> %s\n",
                ao.context().started, ao.context().completed,
                ao.context().rejected, ao.context().result,
                static_cast<unsigned>(reg.used()),
                static_cast<unsigned>(pool.used()),
                pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
