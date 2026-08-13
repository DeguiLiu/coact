// coact example: node manager with heartbeat-driven HSM per node.
// Adapted from newosp examples/node_manager_hsm_demo.cpp (MIT, liudegui).
//
// Demonstrates several independent active objects under one Runtime: four node
// AOs, each owning a three-state HSM (Connected -> Suspect -> Disconnected),
// driven by heartbeat events routed by TargetId through the coordinator.
// Conditional transitions (missed >= threshold) use table-ordered guards.
// SPDX-License-Identifier: MIT

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

/* ---- Node events ------------------------------------------------------- */
enum Signal : uint16_t {
    kHeartbeatOk = 1U,
    kHeartbeatMiss = 2U,
    kDisconnect = 3U,
    kReconnect = 4U
};

/* ---- Node context (one per AO) ----------------------------------------- */
struct NodeContext {
    uint16_t node_id = 0U;
    uint32_t missed_heartbeats = 0U;
    uint32_t total_heartbeats = 0U;
    bool connected = false;
};

/* ---- State indices ----------------------------------------------------- */
constexpr int8_t kRoot = 0;
constexpr int8_t kNodeConnected = 1;
constexpr int8_t kNodeSuspect = 2;
constexpr int8_t kNodeDisconnected = 3;

void connected_entry(NodeContext& ctx)
{
    ctx.connected = true;
    ctx.missed_heartbeats = 0U;
    std::printf("[Node %u] -> Connected\n", ctx.node_id);
}

void suspect_entry(NodeContext& ctx)
{
    std::printf("[Node %u] -> Suspect (missed=%u)\n",
                ctx.node_id, ctx.missed_heartbeats);
}

void disconnected_entry(NodeContext& ctx)
{
    ctx.connected = false;
    std::printf("[Node %u] -> Disconnected\n", ctx.node_id);
}

/* Guards run before the transition action (context is pre-action). */
bool already_suspect(const NodeContext& ctx, const coact::Event&)
{
    return (ctx.missed_heartbeats >= 1U);
}
bool first_miss(const NodeContext& ctx, const coact::Event&)
{
    return (0U == ctx.missed_heartbeats);
}
bool already_disconnect(const NodeContext& ctx, const coact::Event&)
{
    return (ctx.missed_heartbeats >= 4U);
}
bool below_disconnect(const NodeContext& ctx, const coact::Event&)
{
    return (ctx.missed_heartbeats < 4U);
}

void miss_count(NodeContext& ctx, const coact::Event&)
{
    ++ctx.missed_heartbeats;
    std::printf("[Node %u] heartbeat MISS (count=%u)\n",
                ctx.node_id, ctx.missed_heartbeats);
}

void ok_count(NodeContext& ctx, const coact::Event&)
{
    ++ctx.total_heartbeats;
    std::printf("[Node %u] heartbeat OK (total=%u)\n",
                ctx.node_id, ctx.total_heartbeats);
}

void ok_recover(NodeContext& ctx, const coact::Event&)
{
    ++ctx.total_heartbeats;
    ctx.missed_heartbeats = 0U;
    std::printf("[Node %u] recovered to Connected\n", ctx.node_id);
}

void reconnect_action(NodeContext& ctx, const coact::Event&)
{
    ctx.missed_heartbeats = 0U;
    std::printf("[Node %u] reconnect\n", ctx.node_id);
}

void ignore_heartbeat(NodeContext& ctx, const coact::Event&)
{
    std::printf("[Node %u] ignoring heartbeat while Disconnected\n", ctx.node_id);
}

const coact::StateDef<NodeContext> kStates[] = {
    { -1, nullptr, nullptr, "Root" },
    { kRoot, connected_entry, nullptr, "Connected" },
    { kRoot, suspect_entry, nullptr, "Suspect" },
    { kRoot, disconnected_entry, nullptr, "Disconnected" },
};

const coact::TransitionDef<NodeContext> kTransitions[] = {
    /* Connected */
    { kNodeConnected, kHeartbeatOk,   kNodeConnected,    coact::TransitionKind::Internal, nullptr, ok_count },
    { kNodeConnected, kHeartbeatMiss, kNodeSuspect,      coact::TransitionKind::External, already_suspect, miss_count },
    { kNodeConnected, kHeartbeatMiss, kNodeConnected,    coact::TransitionKind::Internal, first_miss, miss_count },
    { kNodeConnected, kDisconnect,    kNodeDisconnected, coact::TransitionKind::External, nullptr, nullptr },
    /* Suspect */
    { kNodeSuspect,   kHeartbeatOk,   kNodeConnected,    coact::TransitionKind::External, nullptr, ok_recover },
    { kNodeSuspect,   kHeartbeatMiss, kNodeDisconnected, coact::TransitionKind::External, already_disconnect, miss_count },
    { kNodeSuspect,   kHeartbeatMiss, kNodeSuspect,      coact::TransitionKind::Internal, below_disconnect, miss_count },
    { kNodeSuspect,   kDisconnect,    kNodeDisconnected, coact::TransitionKind::External, nullptr, nullptr },
    /* Disconnected */
    { kNodeDisconnected, kReconnect,  kNodeConnected,    coact::TransitionKind::External, nullptr, reconnect_action },
    { kNodeDisconnected, kHeartbeatOk,   kNodeDisconnected, coact::TransitionKind::Internal, nullptr, ignore_heartbeat },
    { kNodeDisconnected, kHeartbeatMiss, kNodeDisconnected, coact::TransitionKind::Internal, nullptr, ignore_heartbeat },
};

/* coact's AoRegistry requires a unique logical_prio per AO (design 5.6: two
   AOs must not claim the same scheduler priority). The Traits are therefore
   parameterized by the node's priority; the four nodes use distinct values. */
template <uint8_t Prio>
struct NodeTraits {
    static coact::LogicalPrio logical_prio() { return Prio; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using NodeHsm = coact::Hsm<NodeContext>;
using NodeAo30 = coact::Ao<NodeContext, NodeHsm, NodeTraits<30>>;
using NodeAo25 = coact::Ao<NodeContext, NodeHsm, NodeTraits<25>>;
using NodeAo20 = coact::Ao<NodeContext, NodeHsm, NodeTraits<20>>;
using NodeAo15 = coact::Ao<NodeContext, NodeHsm, NodeTraits<15>>;

const char* signal_name(uint16_t s)
{
    switch (s) {
    case kHeartbeatOk: return "OK";
    case kHeartbeatMiss: return "MISS";
    case kDisconnect: return "DISCONNECT";
    case kReconnect: return "RECONNECT";
    default: return "?";
    }
}

}  // namespace

int main()
{
    coact::pal::Posix pal;

    constexpr uint16_t kBlk = 16U;
    constexpr uint16_t kCap = 64U;
    alignas(16) static unsigned char storage[kBlk * kCap + kBlk];
    coact::EventPool<kBlk, kCap> pool;
    pool.init(storage, sizeof(storage), coact::make_critical_section(pal));

    /* Four independent node AOs, each with a distinct logical priority;
       each owns its own context/HSM instance. */
    NodeAo30 node1(kStates, static_cast<uint16_t>(sizeof(kStates) / sizeof(kStates[0])),
                   kTransitions, static_cast<uint16_t>(sizeof(kTransitions) / sizeof(kTransitions[0])),
                   kNodeConnected, /*max_depth=*/2U);
    NodeAo25 node2(kStates, static_cast<uint16_t>(sizeof(kStates) / sizeof(kStates[0])),
                   kTransitions, static_cast<uint16_t>(sizeof(kTransitions) / sizeof(kTransitions[0])),
                   kNodeConnected, /*max_depth=*/2U);
    NodeAo20 node3(kStates, static_cast<uint16_t>(sizeof(kStates) / sizeof(kStates[0])),
                   kTransitions, static_cast<uint16_t>(sizeof(kTransitions) / sizeof(kTransitions[0])),
                   kNodeConnected, /*max_depth=*/2U);
    NodeAo15 node4(kStates, static_cast<uint16_t>(sizeof(kStates) / sizeof(kStates[0])),
                   kTransitions, static_cast<uint16_t>(sizeof(kTransitions) / sizeof(kTransitions[0])),
                   kNodeConnected, /*max_depth=*/2U);

    node1.context().node_id = 101U;
    node2.context().node_id = 102U;
    node3.context().node_id = 103U;
    node4.context().node_id = 104U;

    coact::Event init_e;
    init_e.signal = 0U;
    init_e.pool_id = 0U;
    init_e.ref_ctr = 0U;
    node1.init(init_e);
    node2.init(init_e);
    node3.init(init_e);
    node4.init(init_e);

    coact::Runtime<coact::DefaultConfig, coact::pal::Posix> rt(pal);
    if (!rt.bind(&node1) || !rt.bind(&node2) || !rt.bind(&node3) || !rt.bind(&node4)) {
        std::printf("bind failed\n");
        return 1;
    }
    rt.initialize();
    rt.start();

    /* Scripted scenarios: (target, signal) pairs, submitted in order. All
       nodes are Normal class -> one FIFO partition -> deterministic order. */
    struct Step {
        coact::TargetId target;
        uint16_t signal;
    };
    const Step script[] = {
        /* Node 101: normal operation, stays Connected */
        { coact::TargetId(1U), kHeartbeatOk }, { coact::TargetId(1U), kHeartbeatOk },
        { coact::TargetId(1U), kHeartbeatOk }, { coact::TargetId(1U), kHeartbeatOk },
        /* Node 102: two misses -> Suspect -> recovery */
        { coact::TargetId(2U), kHeartbeatOk }, { coact::TargetId(2U), kHeartbeatMiss }, { coact::TargetId(2U), kHeartbeatMiss },
        { coact::TargetId(2U), kHeartbeatMiss }, { coact::TargetId(2U), kHeartbeatOk }, { coact::TargetId(2U), kHeartbeatOk },
        /* Node 103: five misses -> Disconnected -> reconnect */
        { coact::TargetId(3U), kHeartbeatOk }, { coact::TargetId(3U), kHeartbeatMiss }, { coact::TargetId(3U), kHeartbeatMiss },
        { coact::TargetId(3U), kHeartbeatMiss }, { coact::TargetId(3U), kHeartbeatMiss }, { coact::TargetId(3U), kHeartbeatMiss },
        { coact::TargetId(3U), kHeartbeatMiss }, { coact::TargetId(3U), kReconnect }, { coact::TargetId(3U), kHeartbeatOk },
        /* Node 104: immediate disconnect then reconnect */
        { coact::TargetId(4U), kHeartbeatOk }, { coact::TargetId(4U), kHeartbeatOk }, { coact::TargetId(4U), kDisconnect },
        { coact::TargetId(4U), kReconnect }, { coact::TargetId(4U), kHeartbeatOk },
    };

    std::printf("=== coact node-manager demo: %d events, 4 node AOs ===\n",
                static_cast<int>(sizeof(script) / sizeof(script[0])));
    coact::EventQos qos{false, false};
    for (const Step& st : script) {
        std::printf(">> node %u <- %s\n", st.target.raw(), signal_name(st.signal));
        coact::Event* e = pool.alloc(st.signal);
        if (nullptr == e) {
            std::printf("pool exhausted\n");
            break;
        }
        rt.coordinator().submit_from_task(st.target, e, qos);
    }

    /* Drain all four AOs (pending()==0 per AO) via the type-erased base. */
    coact::AoBase* nodes[] = { &node1, &node2, &node3, &node4 };
    for (int w = 0; w < 400; ++w) {
        bool drained = true;
        for (coact::AoBase* n : nodes) {
            if (0U != n->pending().load()) {
                drained = false;
            }
        }
        if (drained) {
            break;
        }
        usleep(5000);
    }
    rt.stop();

    std::printf("\n=== final node states ===\n");
    const NodeContext* all[] = { &node1.context(), &node2.context(),
                                 &node3.context(), &node4.context() };
    const char* final_names[] = { node1.hsm_current_state_name(),
                                  node2.hsm_current_state_name(),
                                  node3.hsm_current_state_name(),
                                  node4.hsm_current_state_name() };
    int ni = 0;
    for (const NodeContext* c : all) {
        std::printf("Node %u: connected=%s missed=%u total=%u [%s]\n",
                    c->node_id, c->connected ? "true" : "false",
                    c->missed_heartbeats, c->total_heartbeats,
                    (final_names[ni] != nullptr) ? final_names[ni] : "?");
        ++ni;
    }
    std::printf("pool.used: %u\n", static_cast<unsigned>(pool.used()));
    return 0;
}
