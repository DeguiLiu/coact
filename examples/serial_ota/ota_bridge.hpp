// coact bridge that carries the serial-OTA host side on the coact event chain.
//
// Hybrid design: newosp provides the OTA state machines (device), the
// BehaviorTree host driver and the UART FIFOs; coact carries the *event link*
// between the frame parser (main thread) and the OtaHost (coact Dispatcher
// thread). Because OtaHost::OnResponse() and host.Tick() both mutate HostContext,
// they must run on the SAME thread to avoid a data race — so the Ao owns
// OtaHost and both are confined to the coact Dispatcher thread. The device
// stays on the main thread; the two threads communicate only through the
// SPSC UART FIFOs (thread-safe).
//
// Events:
//   kSigHostTick   -> host.Tick() one step (terminates OTA, sets done flag)
//   kSigHostFrame  -> pooled FrameEvent carrying a host-targeted frame
//                     (parsed on main thread) -> host.OnResponse(frame)
//
// Signals are routed to this single Ao via TargetId kOtaBridgeTargetId.
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/pool.hpp"

#include "host.hpp"   // ota::OtaHost (newosp)
#include "protocol.hpp"

namespace ota_bridge {

// Coact signals for this bridge Ao.
enum Signal : uint16_t {
    kSigHostTick = 1U,
    kSigHostFrame = 2U
};

// Payload-bearing coact event: carries a full parsed frame. coact::Event has
// no payload field, so the frame is copied into the pooled block (base first
// member, so &frame_event.base == the Event* the pool returns).
struct FrameEvent {
    coact::Event base;
    ota::Frame frame;
};

// Ao context: the host + a completion flag. Only touched on the coact
// Dispatcher thread (inside the Ao's dispatch), never on the main thread.
struct OtaBridgeCtx {
    ota::OtaHost* host = nullptr;
    std::atomic<bool>* done = nullptr;
};

// Traits: single Normal-class AO, staged (not direct).
struct BridgeTraits {
    static coact::LogicalPrio logical_prio() { return 10U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

// --- HSM handlers ----------------------------------------------------------

inline void tick_host(OtaBridgeCtx& ctx, const coact::Event&)
{
    if (nullptr == ctx.host) {
        return;
    }
    const auto st = ctx.host->Tick();
    if (st == osp::NodeStatus::kSuccess || st == osp::NodeStatus::kFailure) {
        if (ctx.done != nullptr) {
            ctx.done->store(true, std::memory_order_release);
        }
    }
}

inline void forward_frame(OtaBridgeCtx& ctx, const coact::Event& evt)
{
    if (nullptr == ctx.host) {
        return;
    }
    // evt is the base of a FrameEvent (pool block = sizeof(FrameEvent)).
    const FrameEvent& fe = reinterpret_cast<const FrameEvent&>(evt);
    ctx.host->OnResponse(fe.frame);
}

inline const coact::StateDef<OtaBridgeCtx> kStates[] = {
    { -1, nullptr, nullptr },   // single root state
};

inline const coact::TransitionDef<OtaBridgeCtx> kTransitions[] = {
    { 0, kSigHostTick,  0, coact::TransitionKind::Internal, nullptr, tick_host },
    { 0, kSigHostFrame, 0, coact::TransitionKind::Internal, nullptr, forward_frame },
};

using BridgeHsm = coact::Hsm<OtaBridgeCtx>;
using BridgeAo = coact::Ao<OtaBridgeCtx, BridgeHsm, BridgeTraits>;

// One AO per demo; TargetId 1 routes both events to it.
inline constexpr coact::TargetId kOtaBridgeTargetId = 1U;

}  // namespace ota_bridge
