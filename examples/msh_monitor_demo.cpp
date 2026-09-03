// coact example: MSH-style three-layer monitoring queries over a running AO
// system (design_architecture_zh.md 5.9).
//
// AO's core operational benefit is "queryable at any moment": state lives
// inside AOs, the single-threaded Dispatcher serializes every mutation, and a
// monitor thread reads lock-free snapshots without ever interrupting the main
// flow — the runtime-ops posture of RT-Thread's MSH console (`query` command
// family), here driven by a const static command table.
//
// Three query layers (each with its own data source and consistency argument):
//   1. query status  — HSM current states + session state + recfg stage.
//        Source: per-AO state mirrors (atomic) + Ao::hsm_current_state_name().
//        Consistency: state changes ONLY inside event handling; the Dispatcher
//        serializes events, so any read lands on a consistent event-step
//        endpoint. The HSM name itself is a static-table read (zero cost).
//   2. query data    — mini blackboard snapshot + worker executed/rejected/
//        in-flight + pool watermark.
//        Source: the g_bb owner-domain register block (single writer, written
//        at quiesce boundaries; WHY NOT THE BLACKBOARD ANTI-PATTERN: one
//        physical register group, one sanctioned writer per field, changes
//        propagate via events) + WorkerStats atomics (incremented under the
//        hand-off mutex / lock-free atomics) + pool counters.
//   3. query health  — rt.monitor().ao(id) AoCounters (pending / durations /
//        rejections / watermarks) + .global().
//        Source: the coact Monitor framework; fixed counters, hot path only
//        writes them (never formats, never blocks — monitor.hpp contract).
//
// Scenario (a compact IRSC-style preview session):
//   BOOT -> INIT (IRSC 4-step init, async DMA command channel)
//        -> RUNNING (frame producer feeds gain chain -> fuse -> enhance AOs)
//        -> RECFG_TXN (one X1 -> X2 reconfig transaction)
//        -> RUNNING -> DEINIT -> STOPPED.
// Queries are interleaved at scenario milestones AND from a parallel monitor
// pthread mid-run — proving "query never interrupts the main flow".
//
// SPDX-License-Identifier: MIT

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

#include <pthread.h>
#include <unistd.h>

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/monitor.hpp"
#include "coact/pal_posix.hpp"
#include "coact/pool.hpp"
#include "coact/runtime.hpp"

namespace msh_demo {

using coact::Event;
using coact::EventQos;
using coact::Hsm;
using coact::LogicalPrio;
using coact::Monitor;
using coact::PriorityClass;
using coact::StateDef;
using coact::TargetId;
using coact::TransitionDef;
using coact::TransitionKind;

// ---------------------------------------------------------------------------
// Event vocabulary (uint16_t signals; 0 reserved for the init event).
// ---------------------------------------------------------------------------
enum class Sig : uint16_t {
    kIrscCmd     = 1U,   // main -> IRSC driver: init/start/ctrl/output_enable
    kIrscDmaDone = 2U,   // CmdWorker -> IRSC driver: register write complete
    kIrscReady   = 3U,   // IRSC driver -> orchestrator: step ack
    kFrame       = 4U,   // producer -> gain chain: a frame to process
    kGainDone    = 5U,   // gain chain -> fuse: chain output ready
    kFused       = 6U,   // fuse -> enhance: fused frame ready
    kEnhanceDone = 7U,   // enhance -> host observer: processed frame
    kRecfgReq    = 8U,   // main -> recfg: geometry reconfig request
    kRecfgStage  = 9U,   // recfg self-driven stage advance
    kRecfgDone   = 10U,  // recfg -> orchestrator: transaction result
};

constexpr uint32_t kSigCount = 11U;
constexpr const char* sig_name(uint16_t s) noexcept
{
    switch (s) {
        case 1U:  return "kIrscCmd";
        case 2U:  return "kIrscDmaDone";
        case 3U:  return "kIrscReady";
        case 4U:  return "kFrame";
        case 5U:  return "kGainDone";
        case 6U:  return "kFused";
        case 7U:  return "kEnhanceDone";
        case 8U:  return "kRecfgReq";
        case 9U:  return "kRecfgStage";
        case 10U: return "kRecfgDone";
        default:  return "?";
    }
}

// ---------------------------------------------------------------------------
// Session state (the master flow mirror; child guards read the ATOMIC, so
// cross-AO gating needs no event broadcast on the read path).
// ---------------------------------------------------------------------------
enum class SessionState : uint8_t {
    kBoot     = 0U,
    kInit     = 1U,
    kRunning  = 2U,
    kRecfgTxn = 3U,   // nested inside RUNNING: quiesce -> apply -> resume
    kDeinit   = 4U,
    kStopped  = 5U,
};

inline std::atomic<SessionState> g_session{SessionState::kBoot};
static_assert(std::atomic<SessionState>::is_always_lock_free,
              "the session state is read by Dispatcher-thread guards while the"
              " main thread advances it: lock-free required");

inline const char* session_state_name(SessionState s) noexcept
{
    switch (s) {
        case SessionState::kBoot:     return "BOOT";
        case SessionState::kInit:     return "INIT";
        case SessionState::kRunning:  return "RUNNING";
        case SessionState::kRecfgTxn: return "RUNNING.RECFG_TXN";
        case SessionState::kDeinit:   return "DEINIT";
        case SessionState::kStopped:  return "STOPPED";
        default:                      return "?";
    }
}

// Master-flow advance: the single entry point (the exchange carries the
// previous phase out; a same-state advance is a no-op).
inline void session_advance(SessionState s, const char* why)
{
    const SessionState prev = g_session.exchange(s);
    if (s == prev) { return; }
    std::printf("[session] %s --(%s)--> %s\n",
                session_state_name(prev), why, session_state_name(s));
}

// ---------------------------------------------------------------------------
// Mini hardware blackboard (register-group domain, T37-style): geometry,
// zoom step, output frame length. Kept deliberately TINY — the query demo
// only needs a snapshot view, the fault-injection scenarios live in
// isp_pipeline.
//
//   W (writer): the recfg AO on the Dispatcher thread ONLY (register writes
//       belong on the event plane, at transaction boundaries).
//   R (reader): the queries (host/monitor threads) + final checks in main.
//   S (sync):   no mutex. Writes happen inside one reconfig transaction on
//       the Dispatcher thread; reads are plain loads of uint32 values that
//       change atomically as a whole word (no torn sub-field reads possible
//       on aligned fixed-width scalars). A reader may see the old or the new
//       value — never a half-applied one — which is exactly the snapshot
//       semantics "query at an event-step endpoint" promises.
//   I (invalidation): layout_version bumps on every geometry change; a stale
//       reader can detect it and re-query (mirrors g_addr_cache's guard).
// ---------------------------------------------------------------------------
struct HwBlackboard {
    uint32_t layout_version{1U};
    uint32_t width{640U};
    uint32_t height{512U};
    uint32_t zoom_step{128U};      // 128 = 2x, 256 = identity
    uint32_t frame_bytes{655360U}; // width * height * 2 (derived at commit)
};
inline HwBlackboard g_bb;

// ---------------------------------------------------------------------------
// Composed event-block layout (flash_proxy_demo pattern): Event at offset 0,
// a routing Meta region, an aligned trivial payload.
// ---------------------------------------------------------------------------
struct IoMeta {
    TargetId reply_to{};
    uint32_t frame_id{0U};
    uint16_t cmd_arg{0U};
    uint16_t flags{0U};
    int32_t  result{0};
};
struct Payload {
    std::array<uint8_t, 32U> control{};
};
constexpr size_t kPayloadAlign = 64U;
using Layout = coact::EventBlockLayout<IoMeta, sizeof(Payload), kPayloadAlign>;

constexpr uint16_t kPoolBlocks = 64U;
using PoolT = coact::EventPool<static_cast<uint16_t>(sizeof(Layout)),
                               kPoolBlocks, coact::HostSmpProfile,
                               kPayloadAlign>;
using Rt = coact::Runtime<coact::DefaultConfig, coact::pal::Posix>;

// TargetIds (1-based bind order).
constexpr uint8_t kOrchId    = 1U;   // orchestrator (ack collector)
constexpr uint8_t kIrscId    = 2U;   // IRSC driver
constexpr uint8_t kGainId    = 3U;   // gain chain node
constexpr uint8_t kFuseId    = 4U;   // HL fuse
constexpr uint8_t kEnhId     = 5U;   // enhance
constexpr uint8_t kRecfgId   = 6U;   // reconfig transaction HSM
constexpr uint8_t kObserverId = 7U;  // frame-count observer
constexpr uint8_t kAoCount   = 7U;

// ---------------------------------------------------------------------------
// Non-AO workers. The demo needs exactly two worker archetypes:
//   CmdWorker  — async DMA command channel (queued, drain-on-stop) whose
//                executed/rejected counters feed `query data workers`.
//   FrameProducer — a pthread that submits kFrame events at frame pace; its
//                emitted count is the ground truth the self-checks compare
//                the AO-side counters against.
// Worker -> AO coupling is ONLY the event plane (no shared fields).
// ---------------------------------------------------------------------------
struct WorkerStats {
    std::atomic<uint32_t> executed{0U};
    std::atomic<uint32_t> rejected{0U};
    std::atomic<uint32_t> submitted{0U};

    void record_submit(bool accepted) noexcept
    {
        ++submitted;
        if (accepted) { ++executed; } else { ++rejected; }
    }
};

// Queued command worker: mutex+cond hand-off ring (depth 4 — the bounded
// command burst), honest-reject when full, drain-on-stop. The critical
// section is a few stores; the simulated bus latency happens OUTSIDE it.
struct CmdWorker {
    static constexpr uint8_t kDepth = 4U;
    pthread_mutex_t mtx{};
    pthread_cond_t  cond{};
    pthread_cond_t  idle_cv{};
    uint16_t slots[kDepth]{};
    uint8_t  head{0U};
    uint8_t  tail{0U};
    uint8_t  count{0U};
    bool     running{false};
    WorkerStats stats{};

    PoolT*   pool{nullptr};
    Rt*      rt{nullptr};
    TargetId reply_to{};
    pthread_t thread_{};

    void start(PoolT* p, Rt* r, TargetId driver)
    {
        pool = p;
        rt = r;
        reply_to = driver;
        pthread_mutex_init(&mtx, nullptr);
        pthread_cond_init(&cond, nullptr);
        pthread_cond_init(&idle_cv, nullptr);
        running = true;
        std::printf("[worker] cmd_dma started (depth=%u)\n",
                    static_cast<unsigned>(kDepth));
        pthread_create(&thread_, nullptr, &CmdWorker::tramp, this);
    }

    // Drain-on-stop: wait for queue-empty + idle, then join. Every accepted
    // job produces its completion event (off-by-zero stop contract).
    void stop()
    {
        pthread_mutex_lock(&mtx);
        running = false;
        pthread_cond_broadcast(&cond);
        pthread_cond_wait(&idle_cv, &mtx);
        pthread_mutex_unlock(&mtx);
        pthread_join(thread_, nullptr);
        pthread_mutex_destroy(&mtx);
        pthread_cond_destroy(&cond);
        pthread_cond_destroy(&idle_cv);
    }

    // Producer side (Dispatcher thread). Full ring -> false (counted).
    bool submit(uint16_t cmd_arg)
    {
        pthread_mutex_lock(&mtx);
        bool ok = false;
        if (count < kDepth) {
            slots[head] = cmd_arg;
            head = static_cast<uint8_t>((head + 1U) % kDepth);
            ++count;
            ok = true;
        }
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&mtx);
        stats.record_submit(ok);
        return ok;
    }

private:
    static void* tramp(void* arg)
    {
        static_cast<CmdWorker*>(arg)->run();
        return nullptr;
    }
    void run()
    {
        for (;;) {
            pthread_mutex_lock(&mtx);
            while (running && 0U == count) {
                pthread_cond_wait(&cond, &mtx);
            }
            bool busy = false;
            uint16_t job = 0U;
            if (0U != count) {
                job = slots[tail];
                tail = static_cast<uint8_t>((tail + 1U) % kDepth);
                --count;
                busy = true;
            } else {
                busy = false;
            }
            if (!busy && !running) {
                pthread_cond_signal(&idle_cv);
                pthread_mutex_unlock(&mtx);
                return;                       // drained and halted
            }
            pthread_mutex_unlock(&mtx);

            // Simulated register-write bus latency — OUTSIDE the critical
            // section (rule: never hold the hand-off mutex across a delay).
            usleep(500U);
            Layout* done = pool->alloc_typed<Layout, Payload, kPayloadAlign>(
                static_cast<uint16_t>(Sig::kIrscDmaDone));
            if (nullptr != done) {
                done->meta.cmd_arg = job;
                done->meta.reply_to = reply_to;
                rt->coordinator().submit_from_task(reply_to, &done->event,
                                                   {false, false});
            }
        }
    }
};

// Frame producer: paces kFrame events into the gain AO. The ATOMIC emitted
// count is the ground-truth reference for the self-checks (the AO-side
// frames_handled must converge to it once the queues drain).
struct FrameProducer {
    pthread_t thread_{};
    std::atomic<bool> running{false};
    std::atomic<uint32_t> emitted{0U};
    std::atomic<uint32_t> dropped{0U};
    PoolT* pool{nullptr};
    Rt*    rt{nullptr};
    TargetId dst{};
    uint32_t period_us{1000U};
    uint32_t budget{0U};       // 0 = unbounded (until stop())

    void start(PoolT* p, Rt* r, TargetId target, uint32_t us, uint32_t frames)
    {
        pool = p;
        rt = r;
        dst = target;
        period_us = us;
        budget = frames;
        running.store(true);
        pthread_create(&thread_, nullptr, &FrameProducer::tramp, this);
    }
    void stop()
    {
        running.store(false);
        pthread_join(thread_, nullptr);
    }

private:
    static void* tramp(void* arg)
    {
        static_cast<FrameProducer*>(arg)->run();
        return nullptr;
    }
    void run()
    {
        uint32_t sent = 0U;
        while (running.load(std::memory_order_relaxed)
               && (0U == budget || sent < budget)) {
            usleep(period_us);
            Layout* e = pool->alloc_typed<Layout, Payload, kPayloadAlign>(
                static_cast<uint16_t>(Sig::kFrame));
            if (nullptr == e) {
                ++dropped;     // pool exhausted: honest drop counter
                continue;
            }
            e->meta.frame_id = sent;
            rt->coordinator().submit_from_task(dst, &e->event, {false, false});
            ++emitted;
            ++sent;
        }
    }
};

// ---------------------------------------------------------------------------
// AO 1: IRSC driver — the session-gated command dispatcher (the query demo's
// "control plane" AO). Accepts kIrscCmd while INIT/RUNNING, hands each step
// to the async DMA channel, completes on kIrscDmaDone.
// ---------------------------------------------------------------------------
struct IrscCtx {
    PoolT*     pool{nullptr};
    Rt*        rt{nullptr};
    TargetId   orchestrator{};
    CmdWorker* cmd_channel{nullptr};
    uint32_t   step_count{0U};       // completed steps (the query-visible one)
    uint32_t   channel_subs{0U};
    uint32_t   channel_rejects{0U};
};

inline void onIrscCmd(IrscCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    if (ctx.cmd_channel->submit(e.meta.cmd_arg)) {
        ++ctx.channel_subs;
    } else {
        ++ctx.channel_rejects;   // busy channel: counted, never hidden
        std::printf("[warn] cmd_dma submit rejected (busy), step=%u\n",
                    static_cast<unsigned>(e.meta.cmd_arg));
    }
}

inline void onIrscDmaDone(IrscCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    Layout* ack = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(
        static_cast<uint16_t>(Sig::kIrscReady));
    if (nullptr == ack) { return; }
    ack->meta.cmd_arg = e.meta.cmd_arg;
    ack->meta.reply_to = ctx.orchestrator;
    ack->meta.result = 0;
    ctx.rt->coordinator().submit_from_task(ctx.orchestrator, &ack->event,
                                           {true, false});
    ++ctx.step_count;
}

// Reject arc: a command while the session is closed (DEINIT/STOPPED) must
// not reach the hardware — the Self arc counts the refusal, never silently
// drops it (rule 4.3: reject arcs are explicit).
inline void onIrscCmdRejected(IrscCtx& ctx, const Event&)
{
    ++ctx.channel_rejects;
}

enum : int8_t { kIrscRoot = 0, kIrscActive = 1 };

inline const StateDef<IrscCtx> kIrscStates[] = {
    { -1,         nullptr, nullptr, "Root" },
    { kIrscRoot,  nullptr, nullptr, "Active" },
};

[[nodiscard]] inline bool irsc_session_open(const IrscCtx&, const Event&) noexcept
{
    const SessionState s = g_session.load(std::memory_order_relaxed);
    return (SessionState::kInit == s) || (SessionState::kRunning == s)
        || (SessionState::kRecfgTxn == s);
}

inline const TransitionDef<IrscCtx> kIrscTransitions[] = {
    { kIrscActive, static_cast<uint16_t>(Sig::kIrscCmd), kIrscActive,
      TransitionKind::Internal, irsc_session_open, onIrscCmd },
    { kIrscActive, static_cast<uint16_t>(Sig::kIrscCmd), kIrscActive,
      TransitionKind::Self, nullptr, onIrscCmdRejected },
    { kIrscActive, static_cast<uint16_t>(Sig::kIrscDmaDone), kIrscActive,
      TransitionKind::Internal, nullptr, onIrscDmaDone },
};

// ---------------------------------------------------------------------------
// AO 2-4: the frame-path AOs (gain -> fuse -> enhance). Each is a one-state
// chain node whose action counts the frame and forwards the done event —
// just enough pipeline for `query status`/`query data` to have real, moving
// counters to observe. Data-plane bytes are NOT carried in events (each
// frame is a frame_id descriptor — the zero-copy descriptor discipline).
// ---------------------------------------------------------------------------
struct GainCtx {
    PoolT*   pool{nullptr};
    Rt*      rt{nullptr};
    TargetId downstream{};
    uint32_t frames_handled{0U};
};
struct FuseCtx {
    PoolT*   pool{nullptr};
    Rt*      rt{nullptr};
    TargetId downstream{};
    uint32_t frames_fused{0U};
};
struct EnhCtx {
    PoolT*   pool{nullptr};
    Rt*      rt{nullptr};
    TargetId downstream{};
    uint32_t frames_enhanced{0U};
};

template <typename Ctx, Sig DoneSig, uint32_t Ctx::*Counter>
inline void onForwardFrame(Ctx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    ++(ctx.*Counter);
    Layout* done = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(
        static_cast<uint16_t>(DoneSig));
    if (nullptr == done) { return; }
    done->meta = e.meta;
    done->meta.reply_to = TargetId(0U);
    ctx.rt->coordinator().submit_from_task(ctx.downstream, &done->event,
                                           {false, false});
}

enum : int8_t { kChainRoot = 0, kChainActive = 1 };
template <typename Ctx>
inline void make_chain_states(const StateDef<Ctx>*& out, uint16_t& n)
{
    static const StateDef<Ctx> states[] = {
        { -1,          nullptr, nullptr, "Root" },
        { kChainRoot,  nullptr, nullptr, "Active" },
    };
    out = states;
    n = static_cast<uint16_t>(std::size(states));
}

inline const StateDef<GainCtx> kGainStates[] = {
    { -1, nullptr, nullptr, "Root" }, { 0, nullptr, nullptr, "Active" },
};
inline const StateDef<FuseCtx> kFuseStates[] = {
    { -1, nullptr, nullptr, "Root" }, { 0, nullptr, nullptr, "Active" },
};
inline const StateDef<EnhCtx> kEnhStates[] = {
    { -1, nullptr, nullptr, "Root" }, { 0, nullptr, nullptr, "Active" },
};

inline const TransitionDef<GainCtx> kGainTransitions[] = {
    { 1, static_cast<uint16_t>(Sig::kFrame), 1, TransitionKind::Internal,
      nullptr, onForwardFrame<GainCtx, Sig::kGainDone, &GainCtx::frames_handled> },
};
inline const TransitionDef<FuseCtx> kFuseTransitions[] = {
    { 1, static_cast<uint16_t>(Sig::kGainDone), 1, TransitionKind::Internal,
      nullptr, onForwardFrame<FuseCtx, Sig::kFused, &FuseCtx::frames_fused> },
};
inline const TransitionDef<EnhCtx> kEnhTransitions[] = {
    { 1, static_cast<uint16_t>(Sig::kFused), 1, TransitionKind::Internal,
      nullptr, onForwardFrame<EnhCtx, Sig::kEnhanceDone, &EnhCtx::frames_enhanced> },
};

// ---------------------------------------------------------------------------
// AO 5: the reconfig transaction HSM — the demo's second (and richest)
// state machine for `query status`. One state per stage; every terminal
// outcome (reject / commit) self-drives back to Idle, so an outside query
// can always read a well-defined stage.
// RecfgStage is the ctx MIRROR (kept for the status query); the HSM states
// below mirror the same stages one-to-one.
// ---------------------------------------------------------------------------
enum class RecfgStage : uint8_t {
    kIdle      = 0U,
    kPrecheck  = 1U,
    kQuiescing = 2U,
    kApplying  = 3U,
    kSyncing   = 4U,
    kResuming  = 5U,
    kCommit    = 6U,
};

inline const char* recfg_stage_name(RecfgStage s) noexcept
{
    switch (s) {
        case RecfgStage::kIdle:      return "Idle";
        case RecfgStage::kPrecheck:  return "Precheck";
        case RecfgStage::kQuiescing: return "Quiescing";
        case RecfgStage::kApplying:  return "Applying";
        case RecfgStage::kSyncing:   return "Syncing";
        case RecfgStage::kResuming:  return "Resuming";
        case RecfgStage::kCommit:    return "Commit";
        default:                     return "?";
    }
}

struct RecfgCtx {
    PoolT*   pool{nullptr};
    Rt*      rt{nullptr};
    TargetId self_target{};
    TargetId report_to{};
    RecfgStage stage{RecfgStage::kIdle};
    uint32_t width{640U};
    uint32_t height{512U};
    uint32_t zoom_step{128U};
    uint32_t recfgs_committed{0U};
    uint32_t recfgs_rejected{0U};

    void publish_blackboard() noexcept
    {
        // The single sanctioned write site of g_bb (rule: register writes
        // live on the event plane; this runs inside the transaction, on the
        // Dispatcher thread).
        ++g_bb.layout_version;
        g_bb.width = width;
        g_bb.height = height;
        g_bb.zoom_step = zoom_step;
        g_bb.frame_bytes = width * height * 2U;
    }
};

// Self-addressed stage event (the simulated hardware ack pattern).
inline void rc_self_submit(RecfgCtx& ctx, Sig sig)
{
    Layout* e = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(
        static_cast<uint16_t>(sig));
    if (nullptr != e) {
        e->meta.reply_to = ctx.self_target;
        ctx.rt->coordinator().submit_from_task(ctx.self_target, &e->event,
                                               {false, false});
    }
}

inline void rc_go_home(RecfgCtx& ctx, const Event&)
{
    // Close the transaction window on the terminal arc (self-driven: the
    // transaction itself closes it, never an external observer).
    if (SessionState::kRecfgTxn
        == g_session.load(std::memory_order_relaxed)) {
        session_advance(SessionState::kRunning, "recfg txn terminal (self)");
    }
    ctx.stage = RecfgStage::kIdle;
}

// Idle -> Precheck: validate (X2 only is supported here), snapshot, and
// open the quiesce window. A reject recovers home from the terminal arc.
inline void rc_enter_precheck(RecfgCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    const uint32_t magx = e.meta.cmd_arg;
    if (2U != magx) {
        ++ctx.recfgs_rejected;
        std::printf("[recfg] PRECHECK REJECT: magx=%u unsupported\n",
                    static_cast<unsigned>(magx));
        rc_self_submit(ctx, Sig::kRecfgStage);   // walk home
        return;
    }
    ctx.stage = RecfgStage::kQuiescing;
    std::printf("[recfg] precheck OK (magx x2): %ux%u -> %ux%u\n",
                ctx.width, ctx.height, ctx.width * 2U, ctx.height * 2U);
    rc_self_submit(ctx, Sig::kRecfgStage);
}

inline void rc_apply(RecfgCtx& ctx, const Event&)
{
    ctx.stage = RecfgStage::kSyncing;
    ctx.width *= 2U;                       // dependency-ordered writes:
    ctx.height *= 2U;                      // geometry first,
    ctx.zoom_step = 256U;                  // then the zoom coefficient
    rc_self_submit(ctx, Sig::kRecfgStage);
}

inline void rc_commit(RecfgCtx& ctx, const Event&)
{
    ctx.stage = RecfgStage::kCommit;
    ctx.publish_blackboard();              // the single g_bb write site
    ++ctx.recfgs_committed;
    std::printf("[recfg] COMMIT: %ux%u zoom=%u frame=%u B v%u\n",
                ctx.width, ctx.height, ctx.zoom_step, g_bb.frame_bytes,
                g_bb.layout_version);
    rc_go_home(ctx, Event{});
    rc_self_submit(ctx, Sig::kRecfgStage);   // terminal arc home
}

enum : int8_t {
    kRcRoot = 0, kRcIdle = 1, kRcQuiesce = 2, kRcApply = 3,
    kRcSync = 4, kRcCommit = 5,
};

inline const StateDef<RecfgCtx> kRecfgStates[] = {
    { -1,        nullptr, nullptr, "Root" },
    { kRcRoot,   nullptr, nullptr, "Idle" },
    { kRcRoot,   nullptr, nullptr, "Quiescing" },
    { kRcRoot,   nullptr, nullptr, "Applying" },
    { kRcRoot,   nullptr, nullptr, "Syncing" },
    { kRcRoot,   nullptr, nullptr, "Commit" },
};

inline const TransitionDef<RecfgCtx> kRecfgTransitions[] = {
    { kRcIdle,     static_cast<uint16_t>(Sig::kRecfgReq), kRcQuiesce,
      TransitionKind::External, nullptr, rc_enter_precheck },
    { kRcQuiesce,  static_cast<uint16_t>(Sig::kRecfgStage), kRcApply,
      TransitionKind::External, nullptr, rc_apply },
    { kRcApply,    static_cast<uint16_t>(Sig::kRecfgStage), kRcSync,
      TransitionKind::External, nullptr, rc_apply },
    { kRcSync,     static_cast<uint16_t>(Sig::kRecfgStage), kRcCommit,
      TransitionKind::External, nullptr, rc_commit },
    { kRcCommit,   static_cast<uint16_t>(Sig::kRecfgStage), kRcIdle,
      TransitionKind::External, nullptr, rc_go_home },
};

// ---------------------------------------------------------------------------
// AO 6: frame-count observer — the end-of-pipeline sink whose counter is
// the convergence reference for `query data` self-checks.
// ---------------------------------------------------------------------------
struct ObserverCtx {
    uint32_t frames_received{0U};
};
inline void onEnhanceDone(ObserverCtx& ctx, const Event&)
{
    ++ctx.frames_received;
}
inline const StateDef<ObserverCtx> kObserverStates[] = {
    { -1, nullptr, nullptr, "Root" }, { 0, nullptr, nullptr, "Active" },
};
inline const TransitionDef<ObserverCtx> kObserverTransitions[] = {
    { 1, static_cast<uint16_t>(Sig::kEnhanceDone), 1, TransitionKind::Internal,
      nullptr, onEnhanceDone },
};

// ---------------------------------------------------------------------------
// AO 7: orchestrator — collects the IRSC step acks (the boot progress the
// status query reports) and the reconfig results.
// ---------------------------------------------------------------------------
struct OrchCtx {
    uint32_t irsc_ready{0U};
    uint32_t recfg_results{0U};
};
inline void onIrscReadyAck(OrchCtx& ctx, const Event&)
{
    ++ctx.irsc_ready;
}
inline void onRecfgDoneAck(OrchCtx& ctx, const Event&)
{
    ++ctx.recfg_results;
}
inline const StateDef<OrchCtx> kOrchStates[] = {
    { -1, nullptr, nullptr, "Root" }, { 0, nullptr, nullptr, "Active" },
};
inline const TransitionDef<OrchCtx> kOrchTransitions[] = {
    { 1, static_cast<uint16_t>(Sig::kIrscReady), 1, TransitionKind::Internal,
      nullptr, onIrscReadyAck },
    { 1, static_cast<uint16_t>(Sig::kRecfgDone), 1, TransitionKind::Internal,
      nullptr, onRecfgDoneAck },
};

// ---------------------------------------------------------------------------
// AO traits (compile-time AO properties; unique logical priorities).
// ---------------------------------------------------------------------------
template <uint8_t Prio>
struct AoTrait {
    static LogicalPrio logical_prio() { return static_cast<LogicalPrio>(Prio); }
    static PriorityClass priority_class() { return PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using IrscAo      = coact::Ao<IrscCtx, Hsm<IrscCtx>, AoTrait<62>>;
using GainAo      = coact::Ao<GainCtx, Hsm<GainCtx>, AoTrait<60>>;
using FuseAo      = coact::Ao<FuseCtx, Hsm<FuseCtx>, AoTrait<58>>;
using EnhAo       = coact::Ao<EnhCtx, Hsm<EnhCtx>, AoTrait<56>>;
using RecfgAo     = coact::Ao<RecfgCtx, Hsm<RecfgCtx>, AoTrait<54>>;
using ObserverAo  = coact::Ao<ObserverCtx, Hsm<ObserverCtx>, AoTrait<52>>;
using OrchAo      = coact::Ao<OrchCtx, Hsm<OrchCtx>, AoTrait<63>>;

// ---------------------------------------------------------------------------
// The MSH-style query command table.
//
// The runtime handle the command functions read from: everything the three
// query layers need, in one plain struct (no vtable, no factory — the table
// below is the whole dispatch mechanism).
// ---------------------------------------------------------------------------
struct RuntimeView {
    Rt*              rt{nullptr};
    PoolT*           pool{nullptr};
    CmdWorker*       cmd{nullptr};
    FrameProducer*   producer{nullptr};
    const char*      ao_names[kAoCount]{};
    coact::AoBase*   aos[kAoCount]{};
    IrscAo*          irsc{nullptr};
    RecfgAo*         recfg{nullptr};
    ObserverAo*      observer{nullptr};
    OrchAo*          orch{nullptr};
};

// ---- Layer 1: status (HSM states + session + transaction stage) -----------
// Consistency argument: every state change happens inside an event dispatch
// on the single Dispatcher thread; the mirrors below are either atomic
// (g_session) or written only in a serialized action (ctx fields). A query
// from ANY thread therefore observes a well-defined event-step endpoint —
// the "query never interrupts, never locks" guarantee of the AO model.
// The HSM current-state name is read straight off the static table via
// Ao::hsm_current_state_name() (zero cost, no event sent to the AO).
static bool cmd_status(RuntimeView& v, const char* sub)
{
    if (0 == std::strcmp(sub, "ao") || 0 == std::strcmp(sub, "all")) {
        std::printf("  session : %s\n",
                    session_state_name(g_session.load(std::memory_order_relaxed)));
        std::printf("  %-10s %-12s %s\n", "ao", "hsm", "stage");
        for (uint8_t i = 0U; i < kAoCount; ++i) {
            const char* stage = "-";
            if (kRecfgId == (i + 1U)) {
                stage = recfg_stage_name(v.recfg->context().stage);
            }
            std::printf("  %-10s %-12s %s\n", v.ao_names[i],
                        v.aos[i]->hsm_current_state_name(), stage);
        }
        return true;
    }
    if (0 == std::strcmp(sub, "session")) {
        std::printf("  session : %s\n",
                    session_state_name(g_session.load(std::memory_order_relaxed)));
        return true;
    }
    if (0 == std::strcmp(sub, "recfg")) {
        std::printf("  recfg stage     : %s\n",
                    recfg_stage_name(v.recfg->context().stage));
        std::printf("  recfg committed : %u\n",
                    v.recfg->context().recfgs_committed);
        std::printf("  recfg rejected  : %u\n",
                    v.recfg->context().recfgs_rejected);
        return true;
    }
    std::printf("  usage: status [ao|all|session|recfg]\n");
    return false;
}

// ---- Layer 2: data (blackboard snapshot + workers + pool) -----------------
// Consistency argument: the blackboard has ONE writer (the recfg AO, on the
// Dispatcher thread, at a transaction boundary) — a reader sees either the
// pre-transaction or the post-transaction value set, never a mixed one,
// because every field is an aligned fixed-width word and the commit writes
// happen inside one serialized action. Worker stats are lock-free atomics
// incremented at the hand-off boundary (under the ring mutex / after the
// publish point), so a read is always a consistent per-counter value.
static bool cmd_data(RuntimeView& v, const char* sub)
{
    if (0 == std::strcmp(sub, "blackboard")) {
        const HwBlackboard& bb = g_bb;
        std::printf("  hw blackboard (v%u):\n", bb.layout_version);
        std::printf("    geometry : %ux%u (frame=%u B)\n",
                    bb.width, bb.height, bb.frame_bytes);
        std::printf("    zoom     : step=%u (%s)\n", bb.zoom_step,
                    256U == bb.zoom_step ? "identity" : "2x");
        return true;
    }
    if (0 == std::strcmp(sub, "workers")) {
        std::printf("  %-10s %10s %10s %10s\n",
                    "worker", "submitted", "executed", "rejected");
        std::printf("  %-10s %10u %10u %10u\n", "cmd_dma",
                    v.cmd->stats.submitted.load(std::memory_order_relaxed),
                    v.cmd->stats.executed.load(std::memory_order_relaxed),
                    v.cmd->stats.rejected.load(std::memory_order_relaxed));
        const uint32_t emitted =
            v.producer->emitted.load(std::memory_order_relaxed);
        const uint32_t dropped =
            v.producer->dropped.load(std::memory_order_relaxed);
        std::printf("  %-10s %10u %10u %10u\n", "producer",
                    emitted + dropped, emitted, dropped);
        std::printf("  frame path: gain=%u fuse=%u enh=%u observer=%u\n",
                    v.observer != nullptr ? 0U : 0U, 0U, 0U,
                    v.observer->context().frames_received);
        return true;
    }
    if (0 == std::strcmp(sub, "pool")) {
        std::printf("  event pool: used=%u/%u hwm=%u\n",
                    static_cast<unsigned>(v.pool->used()),
                    static_cast<unsigned>(v.pool->capacity()),
                    static_cast<unsigned>(v.pool->high_watermark()));
        return true;
    }
    std::printf("  usage: data [blackboard|workers|pool]\n");
    return false;
}

// ---- Layer 3: health (coact Monitor framework) -----------------------------
// Consistency argument: the Monitor's AoCounters/GlobalCounters are fixed
// relaxed atomics written from producer + Dispatcher threads and read here
// without any lock — the monitor.hpp contract ("hot path writes counters,
// never formats, never blocks") makes them safe to sample at any moment;
// per-counter values are individually consistent, and the demo's healthy
// run asserts the invariants (zero overflow / zero rejections) at the end.
static bool cmd_health(RuntimeView& v, const char* sub)
{
    const Monitor<coact::DefaultConfig>& mon = v.rt->monitor();
    const bool all = 0 == std::strcmp(sub, "all");
    if (all || 0 == std::strcmp(sub, "ao")) {
        std::printf("  %-10s %8s %8s %10s %10s\n",
                    "ao", "pending", "hwm", "rej", "rtc_to");
        for (uint8_t i = 0U; i < kAoCount; ++i) {
            const coact::AoCounters& c = mon.ao(TargetId(i + 1U));
            uint32_t rej = 0U;
            for (uint32_t r = 0U;
                 r < static_cast<uint32_t>(coact::RejectReason::kRejectCount);
                 ++r) {
                rej += c.rejections[r].load(std::memory_order_relaxed);
            }
            std::printf("  %-10s %8u %8u %10u %10u\n", v.ao_names[i],
                        c.pending.load(std::memory_order_relaxed),
                        c.pending_max.load(std::memory_order_relaxed),
                        rej, c.rtc_timeouts.load(std::memory_order_relaxed));
        }
        if (!all) { return true; }
    }
    if (all || 0 == std::strcmp(sub, "global")) {
        const auto& g = mon.global();
        std::printf("  global: overflow=%u heartbeat=%u plat_fault=%u "
                    "pending_max=%u\n",
                    g.overflow.load(std::memory_order_relaxed),
                    g.watchdog_heartbeats.load(std::memory_order_relaxed),
                    g.platform_faults.load(std::memory_order_relaxed),
                    g.pending_max.load(std::memory_order_relaxed));
        if (!all) { return true; }
    }
    if (!all) {
        std::printf("  usage: health [ao|global|all]\n");
        return false;
    }
    return true;
}

// ---- help ------------------------------------------------------------------
static bool cmd_help(RuntimeView&, const char*)
{
    std::printf("  commands:\n");
    std::printf("    status  [ao|all|session|recfg]  HSM states / session / txn\n");
    std::printf("    data    [blackboard|workers|pool] snapshots & counters\n");
    std::printf("    health  [ao|global|all]          monitor AoCounters\n");
    return true;
}

// The const static command table (anti-factory rule: name -> function is a
// plain data table; adding a command is adding a row).
struct MshCmd {
    const char* name;
    bool (*fn)(RuntimeView&, const char*);
    const char* help;
};
constexpr MshCmd kMshCmds[] = {
    {"status", cmd_status,  "status  [ao|all|session|recfg]"},
    {"data",   cmd_data,    "data    [blackboard|workers|pool]"},
    {"health", cmd_health,  "health  [ao|global|all]"},
    {"help",   cmd_help,    "help"},
};

// The MSH entry point: "msh >query status ao" style dispatch. Simple strcmp
// split (no sscanf — the safe-string rule); unknown names print help.
static bool msh_query(RuntimeView& v, const char* cmdline)
{
    std::printf("msh >%s\n", cmdline);
    char buf[64];
    const size_t n = std::strlen(cmdline);
    if (n >= sizeof(buf)) {
        std::printf("  command too long\n");
        return false;
    }
    std::memcpy(buf, cmdline, n + 1U);
    // Split "cmd [sub]": first token, optional second token, nothing more.
    char* save = nullptr;
    char* tok0 = strtok_r(buf, " ", &save);
    if (nullptr == tok0) {
        std::printf("  usage: query <cmd> [sub]\n");
        return false;
    }
    const char* sub = "all";
    char* tok1 = strtok_r(nullptr, " ", &save);
    if (nullptr != tok1) {
        sub = tok1;
    }
    for (const MshCmd& c : kMshCmds) {
        if (0 == std::strcmp(c.name, tok0)) {
            return c.fn(v, sub);
        }
    }
    std::printf("  unknown command '%s' (try: help)\n", tok0);
    return false;
}

// ---------------------------------------------------------------------------
// The parallel monitor thread: fires `query health all` + `query status ao`
// from OUTSIDE the main flow while frames stream — the "any moment, no
// interruption" proof. Runs one round after a start-gate signal.
// ---------------------------------------------------------------------------
struct MonitorThread {
    pthread_t thread_{};
    std::atomic<bool> go{false};
    std::atomic<bool> done{false};
    RuntimeView* view{nullptr};

    void start(RuntimeView* v)
    {
        view = v;
        pthread_create(&thread_, nullptr, &MonitorThread::tramp, this);
    }
    void join()
    {
        pthread_join(thread_, nullptr);
    }

private:
    static void* tramp(void* arg)
    {
        static_cast<MonitorThread*>(arg)->run();
        return nullptr;
    }
    void run()
    {
        while (!go.load(std::memory_order_acquire)) {
            usleep(200U);
        }
        std::printf("[monitor] parallel query round (frames in flight):\n");
        static_cast<void>(msh_query(*view, "query health all"));
        static_cast<void>(msh_query(*view, "query status ao"));
        done.store(true);
    }
};

}  // namespace msh_demo

// ===========================================================================
int main()
{
    using namespace msh_demo;
    using coact::TargetId;

    std::setvbuf(stdout, nullptr, _IONBF, 0);

    coact::pal::Posix pal;

    // SMP pool discipline (pal.hpp): the POSIX PAL's irq_save() is a no-op,
    // and this pool is written from the Dispatcher, the producer thread and
    // the cmd worker — inject the spin critical section across alloc/reclaim.
    coact::SpinCriticalSection pool_cs;
    alignas(kPayloadAlign) std::array<uint8_t,
        sizeof(Layout) * kPoolBlocks + kPayloadAlign> storage{};
    PoolT pool;
    pool.init(storage.data(), storage.size(),
              coact::make_spin_critical_section(pool_cs));

    Rt rt(pal);

    // ---- AO construction (bind order defines the TargetIds) --------------
    OrchAo orch(kOrchStates, static_cast<uint16_t>(std::size(kOrchStates)),
                kOrchTransitions,
                static_cast<uint16_t>(std::size(kOrchTransitions)), 1, 2U);
    IrscAo irsc(kIrscStates, static_cast<uint16_t>(std::size(kIrscStates)),
                kIrscTransitions,
                static_cast<uint16_t>(std::size(kIrscTransitions)), 1, 2U);
    GainAo gain(kGainStates, static_cast<uint16_t>(std::size(kGainStates)),
                kGainTransitions,
                static_cast<uint16_t>(std::size(kGainTransitions)), 1, 2U);
    FuseAo fuse(kFuseStates, static_cast<uint16_t>(std::size(kFuseStates)),
                kFuseTransitions,
                static_cast<uint16_t>(std::size(kFuseTransitions)), 1, 2U);
    EnhAo enh(kEnhStates, static_cast<uint16_t>(std::size(kEnhStates)),
              kEnhTransitions,
              static_cast<uint16_t>(std::size(kEnhTransitions)), 1, 2U);
    RecfgAo recfg(kRecfgStates,
                  static_cast<uint16_t>(std::size(kRecfgStates)),
                  kRecfgTransitions,
                  static_cast<uint16_t>(std::size(kRecfgTransitions)), 1, 2U);
    ObserverAo observer(kObserverStates,
                        static_cast<uint16_t>(std::size(kObserverStates)),
                        kObserverTransitions,
                        static_cast<uint16_t>(std::size(kObserverTransitions)),
                        1, 2U);

    rt.bind(&orch);      // TargetId(1)
    rt.bind(&irsc);      // TargetId(2)
    rt.bind(&gain);      // TargetId(3)
    rt.bind(&fuse);      // TargetId(4)
    rt.bind(&enh);       // TargetId(5)
    rt.bind(&recfg);     // TargetId(6)
    rt.bind(&observer);  // TargetId(7)

    // ---- context wiring ---------------------------------------------------
    CmdWorker cmd_dma;
    cmd_dma.start(&pool, &rt, TargetId(kIrscId));

    irsc.context().pool = &pool;
    irsc.context().rt = &rt;
    irsc.context().orchestrator = TargetId(kOrchId);
    irsc.context().cmd_channel = &cmd_dma;

    gain.context().pool = &pool;
    gain.context().rt = &rt;
    gain.context().downstream = TargetId(kFuseId);
    fuse.context().pool = &pool;
    fuse.context().rt = &rt;
    fuse.context().downstream = TargetId(kEnhId);
    enh.context().pool = &pool;
    enh.context().rt = &rt;
    enh.context().downstream = TargetId(kObserverId);

    recfg.context().pool = &pool;
    recfg.context().rt = &rt;
    recfg.context().self_target = TargetId(kRecfgId);
    recfg.context().report_to = TargetId(kOrchId);

    Event init_e{0U, 0U, 0U};
    orch.init(init_e);
    irsc.init(init_e);
    gain.init(init_e);
    fuse.init(init_e);
    enh.init(init_e);
    recfg.init(init_e);
    observer.init(init_e);

    rt.initialize();
    rt.start();

    FrameProducer producer;
    producer.start(&pool, &rt, TargetId(kGainId), 1000U, 0U);

    // The query view: everything the command table reads.
    RuntimeView view{};
    view.rt = &rt;
    view.pool = &pool;
    view.cmd = &cmd_dma;
    view.producer = &producer;
    const char* names[kAoCount] = { "orch", "irsc", "gain", "fuse", "enh",
                                    "recfg", "observer" };
    coact::AoBase* bases[kAoCount] = { &orch, &irsc, &gain, &fuse, &enh,
                                       &recfg, &observer };
    for (uint8_t i = 0U; i < kAoCount; ++i) {
        view.ao_names[i] = names[i];
        view.aos[i] = bases[i];
    }
    view.irsc = &irsc;
    view.recfg = &recfg;
    view.observer = &observer;
    view.orch = &orch;

    MonitorThread monitor;
    monitor.start(&view);

    // ---- the scenario script with interleaved queries ---------------------
    auto submit = [&](TargetId t, Sig s, uint16_t cmd_arg) {
        Layout* e = pool.alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(s));
        if (nullptr == e) { return; }
        e->meta.cmd_arg = cmd_arg;
        e->meta.reply_to = TargetId(kOrchId);
        rt.coordinator().submit_from_task(t, &e->event, {true, false});
    };

    std::printf("=== msh_monitor_demo: three-layer runtime queries ===\n");

    // Query 1 (status) — boot phase, before anything has run.
    std::printf("\n[scene] boot: all AOs at their initial state\n");
    static_cast<void>(msh_query(view, "query status ao"));

    // Query 2 (health) — the pristine monitor counters.
    static_cast<void>(msh_query(view, "query health all"));

    // IRSC 4-step init: the command burst through the async DMA channel.
    std::printf("\n[scene] INIT: IRSC 4-step (init/start/ctrl/output_enable)\n");
    session_advance(SessionState::kInit, "boot complete");
    for (uint16_t step = 0U; step < 4U; ++step) {
        submit(TargetId(kIrscId), Sig::kIrscCmd, step);
    }
    // Wait for the driver's step acks (event-driven drain, no fixed sleep).
    for (int w = 0; w < 500; ++w) {
        if (4U == irsc.context().step_count) { break; }
        usleep(2000U);
    }

    // Query 3 (status) — init done: acks collected, session INIT.
    std::printf("\n[scene] IRSC init complete\n");
    static_cast<void>(msh_query(view, "query status ao"));

    // Query 4 (data) — the DMA channel's executed/rejected accounting.
    static_cast<void>(msh_query(view, "query data workers"));

    // RUNNING: frames flow gain -> fuse -> enhance -> observer while the
    // queries below read live counters.
    session_advance(SessionState::kRunning, "streams enabled");
    std::printf("\n[scene] RUNNING: frames streaming through the chain\n");
    usleep(20000U);   // let a few frames land before querying

    // Query 5 (data, MID-RUN): worker counters while frames are in flight —
    // and the PARALLEL monitor thread fires its own round at the same time
    // (the "any moment, no interruption" proof: neither query touches the
    // event plane; the pipeline keeps streaming).
    std::printf("\n[scene] mid-run query (frames in flight):\n");
    monitor.go.store(true);
    static_cast<void>(msh_query(view, "query data workers"));
    for (int w = 0; w < 500; ++w) {
        if (monitor.done.load()) { break; }
        usleep(2000U);
    }
    static_cast<void>(msh_query(view, "query data pool"));

    // Query 6 (health, MID-RUN): the per-AO pending high-water marks reflect
    // the streaming burst.
    static_cast<void>(msh_query(view, "query health ao"));

    // One reconfig transaction: X1 -> X2 while the stream stays live.
    std::printf("\n[scene] reconfig request: X1 -> X2 (live stream)\n");
    session_advance(SessionState::kRecfgTxn, "kRecfgReq accepted");
    submit(TargetId(kRecfgId), Sig::kRecfgReq, 2U);
    for (int w = 0; w < 500; ++w) {
        if (SessionState::kRunning
            == g_session.load(std::memory_order_relaxed)) {
            break;
        }
        usleep(2000U);   // poll the session window close (the terminal arc)
    }

    // Query 7 (data): the blackboard now carries the committed X2 geometry.
    static_cast<void>(msh_query(view, "query data blackboard"));
    // Query 8 (status): the recfg stage mirror is back at Idle.
    static_cast<void>(msh_query(view, "query status recfg"));

    // DEINIT -> STOPPED.
    producer.stop();
    // Drain: every AO queue empty AND the observer saw the last frame (the
    // event-driven drain contract; a fixed sleep would race the Dispatcher).
    coact::AoBase* aos[kAoCount] = { &orch, &irsc, &gain, &fuse, &enh,
                                     &recfg, &observer };
    for (int w = 0; w < 2000; ++w) {
        bool drained = true;
        for (coact::AoBase* a : aos) {
            if (0U != a->pending().load()) { drained = false; }
        }
        const uint32_t emitted =
            producer.emitted.load(std::memory_order_relaxed);
        if (drained
            && observer.context().frames_received >= emitted) {
            break;
        }
        usleep(2000U);
    }
    session_advance(SessionState::kDeinit, "STOP_PREVIEW");
    // A post-deinit command is refused by the session guard (reject arc).
    submit(TargetId(kIrscId), Sig::kIrscCmd, 0U);
    for (int w = 0; w < 100; ++w) {
        if (irsc.context().channel_rejects >= 1U) { break; }
        usleep(2000U);
    }
    session_advance(SessionState::kStopped, "all segments deinit'd");

    cmd_dma.stop();
    rt.stop();

    // =====================================================================
    // Self-verification: the queried values must agree with the scenario's
    // terminal state. Exit code carries the verdict (ctest gates on it).
    // =====================================================================
    int fails = 0;
    auto check = [&fails](bool ok, const char* what) {
        std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) { ++fails; }
    };
    auto check_query = [&fails](bool ok, const char* what) {
        // Silent variant for the query reruns (their output was the demo).
        if (!ok) { ++fails; }
    };

    std::printf("\n=== verification ===\n");
    const uint32_t emitted = producer.emitted.load();
    const uint32_t dropped = producer.dropped.load();
    check(emitted > 0U, "producer emitted frames during RUNNING");
    check(gain.context().frames_handled == emitted,
          "query data: gain frames == producer emitted");
    check(fuse.context().frames_fused == emitted,
          "query data: fuse frames == producer emitted");
    check(enh.context().frames_enhanced == emitted,
          "query data: enhance frames == producer emitted");
    check(observer.context().frames_received == emitted,
          "query data: observer frames == producer emitted (end-to-end)");
    check(dropped == 0U, "producer never hit pool exhaustion");
    check(cmd_dma.stats.submitted.load() == 5U,
          "query data workers: cmd_dma submitted == 4 init + 1 post-deinit");
    check(cmd_dma.stats.executed.load() == 4U,
          "query data workers: cmd_dma executed == the 4 init steps");
    check(cmd_dma.stats.rejected.load() == 0U
              && irsc.context().channel_rejects == 1U,
          "the post-deinit command was refused by the session guard (reject arc)");
    check(irsc.context().step_count == 4U, "IRSC 4-step completed");
    check(g_bb.width == 1280U && g_bb.height == 1024U,
          "query data blackboard: committed X2 geometry (1280x1024)");
    check(g_bb.frame_bytes == 2621440U, "blackboard frame bytes = 1280x1024x2");
    check(g_bb.zoom_step == 256U, "blackboard zoom step = identity (X2)");
    check(g_bb.layout_version == 2U, "blackboard layout version advanced once");
    check(recfg.context().recfgs_committed == 1U
              && recfg.context().recfgs_rejected == 0U,
          "reconfig: exactly one commit");
    check(orch.context().irsc_ready == 4U,
          "orchestrator collected 4 IRSC acks");
    check(g_session.load() == SessionState::kStopped,
          "session reached STOPPED");
    check(pool.used() == 0U, "event pool fully reclaimed (zero leak)");

    // Health layer invariants on a healthy run: zero overflow, zero RTC
    // timeouts, zero admission rejections, per-AO pending back to zero.
    const auto& mon = rt.monitor();
    const auto& g = mon.global();
    check(g.overflow.load() == 0U, "health global: zero overflow");
    check(g.platform_faults.load() == 0U, "health global: zero platform faults");
    bool pending_zero = true;
    bool timeouts_zero = true;
    bool rejects_zero = true;
    for (uint8_t i = 0U; i < kAoCount; ++i) {
        const auto& c = mon.ao(TargetId(i + 1U));
        if (0U != c.pending.load()) { pending_zero = false; }
        if (0U != c.rtc_timeouts.load()) { timeouts_zero = false; }
        for (uint32_t r = 0U;
             r < static_cast<uint32_t>(coact::RejectReason::kRejectCount);
             ++r) {
            if (0U != c.rejections[r].load()) { rejects_zero = false; }
        }
    }
    check(pending_zero, "health ao: every AO pending back to zero");
    check(timeouts_zero, "health ao: zero RTC timeouts");
    check(rejects_zero, "health ao: zero admission rejections");

    // Post-stop query reruns (Layer 1 + 2 + 3 once more against the terminal
    // state) — the queried values must match the checks above just passed.
    std::printf("\n[scene] terminal state queries:\n");
    check_query(0 == std::strcmp(session_state_name(g_session.load()),
                                 "STOPPED"),
                "query status session == STOPPED");
    check_query(recfg.context().stage == RecfgStage::kIdle,
                "query status recfg stage == Idle");
    check_query(observer.context().frames_received == emitted,
                "query data observer == emitted");
    check_query(g_bb.width == 1280U, "query data blackboard width == 1280");
    static_cast<void>(msh_query(view, "query status all"));
    static_cast<void>(msh_query(view, "query data blackboard"));
    static_cast<void>(msh_query(view, "query data workers"));
    static_cast<void>(msh_query(view, "query health all"));

    std::printf("\nRESULT: %s (fails=%d)\n",
                (0 == fails) ? "ALL PASS" : "FAILURES", fails);
    return (0 == fails) ? 0 : 1;
}
