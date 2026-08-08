// coact example: hierarchical protocol HSM.
// Adapted from newosp examples/hsm_protocol_demo.cpp (MIT, liudegui).
//
// Demonstrates coact's Hsm parent-state event inheritance running under the
// full event pipeline: pool alloc -> coordinator submit -> staging ->
// Dispatcher -> Ao::dispatch -> HSM. The Connected parent handles DISCONNECT
// for both the Idle and Active children, so the children define no DISCONNECT
// transition (dispatch walks the leaf->parent chain). SPDX-License-Identifier: MIT

#include <cstdint>
#include <cstdio>

#include <unistd.h>

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/pal_posix.hpp"
#include "coact/pool.hpp"
#include "coact/runtime.hpp"

namespace {

/* ---- Protocol events (coact Event.signal is uint16_t) ------------------ */
enum Signal : uint16_t {
    kConnect = 1U,
    kSynAck = 2U,
    kDisconnect = 3U,
    kFinAck = 4U,
    kTimeout = 5U,
    kDataReady = 6U,
    kDataSent = 7U,
    kError = 8U
};

/* ---- Protocol context -------------------------------------------------- */
struct ProtocolContext {
    int syn_count = 0;
    int ack_count = 0;
    int data_sent_count = 0;
    int error_count = 0;
    bool connected = false;
    char last_action[24] = {0};

    void set_action(const char* a)
    {
        std::snprintf(last_action, sizeof(last_action), "%s", a);
    }
};

/* ---- State table (indices are the state id, 0 is the root) ------------- */
constexpr int8_t kOperational = 0;
constexpr int8_t kDisconnected = 1;
constexpr int8_t kConnecting = 2;
constexpr int8_t kConnected = 3;
constexpr int8_t kIdle = 4;
constexpr int8_t kActive = 5;
constexpr int8_t kDisconnecting = 6;

void disconnected_entry(ProtocolContext& ctx)
{
    ctx.connected = false;
    ctx.set_action("disconnected");
    std::printf("  [Disconnected] entry: connection closed\n");
}

void connecting_entry(ProtocolContext& ctx)
{
    ++ctx.syn_count;
    ctx.set_action("connecting");
    std::printf("  [Connecting] entry: sending SYN...\n");
}

void connected_entry(ProtocolContext& ctx)
{
    ctx.connected = true;
    ctx.set_action("connected");
    std::printf("  [Connected] entry: connection established\n");
}

void connected_exit(ProtocolContext&)
{
    std::printf("  [Connected] exit: leaving connected state\n");
}

void idle_entry(ProtocolContext& ctx)
{
    ctx.set_action("idle");
    std::printf("  [Idle] entry: waiting for data\n");
}

void active_entry(ProtocolContext& ctx)
{
    ctx.set_action("active");
    std::printf("  [Active] entry: processing data\n");
}

void disconnecting_entry(ProtocolContext& ctx)
{
    ctx.set_action("disconnecting");
    std::printf("  [Disconnecting] entry: sending FIN...\n");
}

void ack_action(ProtocolContext& ctx, const coact::Event&)
{
    ++ctx.ack_count;
}

void sent_action(ProtocolContext& ctx, const coact::Event&)
{
    ++ctx.data_sent_count;
}

void error_action(ProtocolContext& ctx, const coact::Event&)
{
    ++ctx.error_count;
}

const coact::StateDef<ProtocolContext> kStates[] = {
    { -1, nullptr, nullptr },                        // Operational (root)
    { kOperational, disconnected_entry, nullptr },   // Disconnected
    { kOperational, connecting_entry, nullptr },     // Connecting
    { kOperational, connected_entry, connected_exit },  // Connected (parent)
    { kConnected, idle_entry, nullptr },              // Idle   (child of Connected)
    { kConnected, active_entry, nullptr },            // Active (child of Connected)
    { kOperational, disconnecting_entry, nullptr },   // Disconnecting
};

const coact::TransitionDef<ProtocolContext> kTransitions[] = {
    { kDisconnected, kConnect,   kConnecting,    coact::TransitionKind::External, nullptr, nullptr },
    { kConnecting,   kSynAck,    kIdle,          coact::TransitionKind::External, nullptr, ack_action },
    { kConnecting,   kTimeout,   kDisconnected,  coact::TransitionKind::External, nullptr, nullptr },
    /* Connected handles DISCONNECT for Idle/Active children via parent lookup. */
    { kConnected,    kDisconnect,kDisconnecting, coact::TransitionKind::External, nullptr, nullptr },
    { kIdle,         kDataReady, kActive,        coact::TransitionKind::External, nullptr, nullptr },
    { kActive,       kDataSent,  kIdle,          coact::TransitionKind::External, nullptr, sent_action },
    { kActive,       kError,     kIdle,          coact::TransitionKind::External, nullptr, error_action },
    { kDisconnecting,kFinAck,    kDisconnected,  coact::TransitionKind::External, nullptr, nullptr },
    { kDisconnecting,kTimeout,   kDisconnected,  coact::TransitionKind::External, nullptr, nullptr },
};

struct ProtocolTraits {
    static coact::LogicalPrio logical_prio() { return 20U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using ProtocolHsm = coact::Hsm<ProtocolContext>;
using ProtocolAo = coact::Ao<ProtocolContext, ProtocolHsm, ProtocolTraits>;

const char* signal_name(uint16_t s)
{
    switch (s) {
    case kConnect: return "CONNECT";
    case kSynAck: return "SYN_ACK";
    case kDisconnect: return "DISCONNECT";
    case kFinAck: return "FIN_ACK";
    case kTimeout: return "TIMEOUT";
    case kDataReady: return "DATA_READY";
    case kDataSent: return "DATA_SENT";
    case kError: return "ERROR";
    default: return "?";
    }
}

}  // namespace

int main()
{
    coact::pal::Posix pal;

    /* One pool big enough for the whole script (zero-copy: Event* travels
       through the pipeline, no payload copy). */
    constexpr uint16_t kBlk = 16U;
    constexpr uint16_t kCap = 32U;
    alignas(16) static unsigned char storage[kBlk * kCap + kBlk];
    coact::EventPool<kBlk, kCap> pool;
    pool.init(storage, sizeof(storage), coact::make_critical_section(pal));

    ProtocolAo ao(kStates, sizeof(kStates) / sizeof(kStates[0]),
                  kTransitions, sizeof(kTransitions) / sizeof(kTransitions[0]),
                  kDisconnected, /*max_depth=*/3U);

    coact::Event init_e;
    init_e.signal = 0U;
    init_e.pool_id = 0U;
    init_e.ref_ctr = 0U;
    ao.init(init_e);   /* enter Disconnected */

    coact::Runtime<coact::DefaultConfig, coact::pal::Posix> rt(pal);
    if (!rt.bind(&ao)) {
        std::printf("bind failed\n");
        return 1;
    }
    rt.initialize();
    rt.start();

    /* Scripted session (mirrors the newosp scenario). DISCONNECT in Idle has
       no local transition; the parent Connected handles it. */
    const uint16_t script[] = {
        kConnect, kSynAck,
        kDataReady, kDataSent,
        kDataReady, kDataSent,
        kDataReady, kDataSent,
        kDataReady, kError,
        kDataReady, kDataSent,
        kDisconnect, kFinAck,
    };

    std::printf("=== coact protocol HSM demo: submitting %d events ===\n",
                static_cast<int>(sizeof(script) / sizeof(script[0])));
    coact::EventQos qos{false, false};
    for (uint16_t sig : script) {
        std::printf(">> submit %s\n", signal_name(sig));
        coact::Event* e = pool.alloc(sig);
        if (nullptr == e) {
            std::printf("pool exhausted\n");
            break;
        }
        rt.coordinator().submit_from_task(1U, e, qos);
    }

    /* Drain: pending()==0 after all submits. The acquire-order counter load
       makes the Dispatcher's context writes visible to this thread. */
    for (int w = 0; w < 200; ++w) {
        if (0U == ao.pending().load()) {
            break;
        }
        usleep(5000);
    }
    rt.stop();

    std::printf("\n=== final context ===\n");
    std::printf("syn_count:       %d\n", ao.context().syn_count);
    std::printf("ack_count:       %d\n", ao.context().ack_count);
    std::printf("data_sent_count: %d\n", ao.context().data_sent_count);
    std::printf("error_count:     %d\n", ao.context().error_count);
    std::printf("connected:       %s\n", ao.context().connected ? "true" : "false");
    std::printf("last_action:     %s\n", ao.context().last_action);
    std::printf("drained:         %s\n",
                (0U == ao.pending().load()) ? "yes" : "NO");
    std::printf("pool.used:       %u\n", static_cast<unsigned>(pool.used()));
    return 0;
}
