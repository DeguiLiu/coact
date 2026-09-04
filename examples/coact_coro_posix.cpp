// coact::coro POSIX worker bridge (plan Task 7): a pal thread completes a
// promise; the AO receives the completion event on the dispatcher plane.
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

enum : uint16_t { kDone = 7U };

struct Ctx {
    unsigned events = 0U;
    uint8_t status = 0xFFU;
};

static void noop(Ctx&) {}
static bool always(const Ctx&, const coact::Event&) { return true; }
static void on_done(Ctx& c, const coact::Event& e)
{
    const coact::coro::CompletionEventPayload p =
        coact::coro::decode_completion(e);
    c.events++;
    c.status = static_cast<uint8_t>(p.status);
}

static const coact::StateDef<Ctx> kStates[] = {
    { -1, nullptr, nullptr, "Root", 1 },
    {  0, noop, noop, "Active" },
};
static const coact::TransitionDef<Ctx> kTrans[] = {
    { 1, kDone, 1, coact::TransitionKind::Internal, always, on_done },
};

struct Traits {
    static coact::LogicalPrio logical_prio() { return 22U; }
    static coact::PriorityClass priority_class()
    {
        return coact::PriorityClass::Normal;
    }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using DemoAo = coact::Ao<Ctx, coact::Hsm<Ctx>, Traits>;
using PoolT = coact::EventPool<32U, 32U>;
using Rt = coact::Runtime<coact::DefaultConfig, coact::pal::Posix>;
using Reg = coact::coro::TaskRegistry<uint32_t, PoolT, Rt::CoordinatorType, 2U>;
using PromiseT = Reg::PromiseT;

struct WorkerArg {
    PromiseT* promise;
    coact::pal::Posix* pal;
    uint32_t value;
};

static void worker_entry(void* raw)
{
    WorkerArg* arg = static_cast<WorkerArg*>(raw);
    arg->pal->sleep_us(2000U);
    (void)arg->promise->complete(arg->value);
}

}  // namespace

int main()
{
    coact::pal::Posix pal;
    alignas(16) static unsigned char storage[32U * 32U + 32U];
    PoolT pool;
    pool.init(storage, sizeof(storage), coact::detail::noop_cs());

    DemoAo ao(kStates, 2U, kTrans, 1U, 1, 2U);
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
    auto created = reg.create(coact::TargetId(1U), kDone,
                              coact::EventQos{false, false});
    if (!static_cast<bool>(created)) {
        rt.stop();
        return 1;
    }
    auto pair = std::move(created.value());
    WorkerArg arg{&pair.promise, &pal, 77U};
    coact::pal::Posix::ThreadHandle th{};
    if (!pal.thread_create(th, &worker_entry, &arg)) {
        std::printf("thread_create failed\n");
        rt.stop();
        return 1;
    }

    for (int w = 0; w < 2000; ++w) {
        if (1U == ao.context().events) {
            break;
        }
        usleep(1000);
    }
    pal.thread_join(th);

    auto taken = reg.take_result(pair.task.id());
    const uint32_t value = static_cast<bool>(taken) ? taken.value() : 0U;
    (void)pair.task.release();

    for (int w = 0; w < 200; ++w) {
        if (0U == ao.pending().load()) {
            break;
        }
        usleep(1000);
    }
    rt.stop();

    const bool pass = (1U == ao.context().events) &&
                      (0U == ao.context().status) &&
                      (77U == value) &&
                      (0U == reg.used()) &&
                      (0U == pool.used());
    std::printf("coact_coro_posix: events=%u status=%u value=%u "
                "reg.used=%u pool.used=%u -> %s\n",
                ao.context().events, ao.context().status, value,
                static_cast<unsigned>(reg.used()),
                static_cast<unsigned>(pool.used()),
                pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
