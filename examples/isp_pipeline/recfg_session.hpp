/*
 * ===========================================================================
 * recfg_session.hpp —— 重配事务 + 会话门控 + ISP 管线初始化序列
 * ===========================================================================
 * 功能描述
 *   - 会话门控（SessionState）：BOOT → INIT → RUNNING → RUNNING.RECFG_TXN
 *     → DEINIT → STOPPED 六相位，原子 g_session 单入口推进，子 AO guard
 *     直接读原子；SessionEventComposite 保留事件面广播能力（当前设计下
 *     publish 默认 no-op，由 guard 直读原子）。
 *   - IspCtx / IspPipelineAO 状态表：isp_init_pipeline → 8 节点正向 init
 *     序列（node[i].init ack-to-ack），不实际实例化 per-node AO（避免注册
 *     表膨胀），仅保留状态机语义；BUSY/READY 跟踪。
 *   - RecfgOrchAo：五步重配事务 precheck → snapshot → quiesce → apply →
 *     resume+commit（RecfgStage enum），X1→X2 重配、layout_version 递增、
 *     注入陈旧 SOUT 故障（DMO+SOUT 拒绝路径）、AddrCache 失效。
 *   - AddrCache / PeriphRegCache / BypassGuard / DisplayMirror：版本守卫
 *     地址缓存、寄存器影子缓存与配对旁路守卫、镜像变换（DisplayMirror
 *     与 DetRect 的 transform），对应嵌入式显示链路原子重配设计.md §5。
 *   对应 RS500 module/rte + camera（execute_config_phases_ex /
 *   rte_call_sync）+ 嵌入式显示链路原子重配设计。
 *
 * 与其他文件的关系
 *   - 上游：main.cpp 发 kRecfgReq（X1→X2）→ RecfgOrchAo；kRecfgStage 自驱
 *     推进；RecfgOrchAo 在 APPLY 阶段请求 kSoutIdle 让 video_stream 进入
 *     quiesce。include common.hpp（SessionState/g_session/session_advance）。
 *   - 下游：kRecfgDone 送 [output_itf] OrchestratorAo；原子 g_session 被全
 *     部子 AO 的 guard 读取（kRunning 之外冻结 stream-shaping 转移）；事件
 *     面 kSessionState 由 SessionEventComposite.publish() 广播给登记过的
 *     TargetId（IRSC/VideoFSM/PackVid/Enhance/TPD）。
 *   - 依赖：SrMagx 枚举 / FrameGeometry 结构 / DmoQuiesce 步骤（事件驱动
 *     确认 SOUT 空闲，作为 Change16517 修复路径）。
 *
 * 文字图（重配视角）
 * ┌──────────────────────────────────────────────────────────────────┐
 * │ 例子系统数据流（→事件信号  ⇒DDR/黑板数据）                        │
 * │                                                                  │
 * │  [sensor_irsc]──kFrameIrscOut──▶[isp_chain]──kHlFused/Done──▶   │
 * │                                  [video_stream]──kPicPacked──▶  │
 * │                                  [output_itf]──kFrameEof──▶     │
 * │                                  [winhost 观察者]                │
 * │                                                                  │
 * │  编排: [main.cpp] ──kIrscCmd/kVideoCmd──▶ 各模块 AO             │
 * │  重配: [recfg_session] ◀──kRecfgReq── main；门控 g_session      │
 * │   (本文件：5步事务/会话相位/IspPipeline/Addr/Periph/Bypass)      │
 * │  共享: [common.hpp] 词汇+DdrCtx+黑板 / PAL: g_pal (SemOps 等)    │
 * └──────────────────────────────────────────────────────────────────┘
 * ===========================================================================
 */
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

#include "coact/ao.hpp"
#include "coact/bitfield.hpp"
#include "coact/hsm.hpp"
#include "coact/runtime.hpp"

#include "common.hpp"

namespace isp_demo {

// ---------------------------------------------------------------------------
// The SYSTEM SESSION: hierarchical HSM linkage via session gating.
//
// The coact HSM is a per-AO run-time machine; cross-AO containment (a sub
// HSM living inside a master state) is expressed the embedded way: a SESSION
// STATE that child-AO guards consult. The master flow (preview session ->
// pipeline segments -> per-segment FSMs) broadcasts kSessionState whenever it
// advances; every child transition arc whose legality depends on the phase
// carries a guard reading this state. This is the standard "layered FSM +
// session gating" equivalent of nested HSMs under a flat per-AO dispatch
// model — the linkage is on the EVENT PLANE (one broadcast, N guards), never
// shared mutable state.
//
//     BOOT -> INIT -> RUNNING -> DEINIT -> STOPPED
//                   ^   |  ^      |
//                   |   v  |      v
//                   |  RECFG_TXN  STOPPING
//                   |_____________|
// The recfg transaction runs INSIDE RUNNING (RUNNING.RECFG_TXN): while it is
// active, stream-shaping transitions in child FSMs are frozen by guards.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// IspPipelineAO: mirrors isp_init_pipeline -> node init forward.
// IspNodes run as their own small AOs; the pipeline just sequences init
// ack-to-ack in correct order and tracks BUSY/READY.
// ---------------------------------------------------------------------------
struct IspCtx {
    PoolT*    pool{nullptr};
    Rt*       rt{nullptr};
    TargetId  orchestrator{};
    // Node targets in init order (per isp_config.json front-end).
    static constexpr uint8_t kNodeCount = 8U;
    std::array<TargetId, kNodeCount> nodes{};
    bool      busy{false};
    uint32_t  init_count{0U};
};

inline void onIspCmd(IspCtx& ctx, const Event& evt)
{
    (void)evt;
    /* 阻塞记账（原真睡 800us）：isp_init_pipeline 的锁 + JSON 解析同步耗时。
       改为记账不睡眠——该延迟无时序断言依赖（total_us 统计语义不变），
       且真睡会占住 Dispatcher 线程、阻塞所有 AO 的事件派发 */
    // Cascade init to each node (init_drv in order).
    for (uint8_t i = 0; i < IspCtx::kNodeCount; ++i) {
        Layout* n = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(Sig::kIspCmd));
        if (nullptr == n) { return; }
        n->meta.cmd_arg = i;
        n->meta.reply_to = ctx.orchestrator;
        ctx.rt->coordinator().submit_from_task(ctx.nodes[i], &n->event, {false, false});
    }
    ctx.busy = true;
}

inline void onIspReadyAck(IspCtx& ctx, const Event&)
{
    ++ctx.init_count;
    if (ctx.init_count >= IspCtx::kNodeCount) {
        Layout* ack = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(Sig::kIspReady));
        if (nullptr != ack) {
            ack->meta.result = 0;
            ack->meta.payload_kind = 3U;
            ack->meta.reply_to = ctx.orchestrator;
            Payload* p = reinterpret_cast<Payload*>(&ack->payload[0]);
            p->control[0] = 'I'; p->control[1] = 'S'; p->control[2] = 'P'; p->control[3] = 'R';
            ctx.rt->coordinator().submit_from_task(ctx.orchestrator, &ack->event, {true, false});
        }
    }
}

COACT_HSM_STATES(kIspStates, IspCtx, "Root", "Active");
inline const TransitionDef<IspCtx> kIspTransitions[] = {
    { 1, static_cast<uint16_t>(Sig::kIspCmd), 1, TransitionKind::Internal, nullptr, onIspCmd },
    { 1, static_cast<uint16_t>(Sig::kIspReady), 1, TransitionKind::Internal, nullptr, onIspReadyAck },
};

// We don't actually instantiate one AO per node (would bloat the registry
// past kMaxAo). Instead the orchestrator synthesizes the 8 kIspReady acks
// directly into its own queue at startup, modeling the nodes' init completion.


// ---------------------------------------------------------------------------
// Toy register map: the register-as-field-group structure the real hardware
// has (RS500 SOUT / AI control registers). Each register is a uint32 backing
// word; each field is a BitFieldView — an INDEPENDENT RMW that never touches
// its neighbors. This is the structure "register = one uint32 scalar" hides;
// the SOUT opcode / mode contract inversion class of failures (and the AI
// control-code 0x0008 semantic reversal) lived exactly here, because the
// scalar model made it trivially easy to overwrite one field while reading
// or writing another.
//
//   REG_SOUT_CTRL:  [0]   enable   (SoutCtrlEnable) FSM-owned stream gate
//                   [4:7] opcode   (SoutCtrlOpcode) e.g. 0x2=frame, 0xA=recfg
//                   [8:9] mode     (SoutCtrlMode)   stream mode the FSM owns
//   REG_AI_CTRL:    [0]   bypass   (AiCtrlBypass)   SR feature kill switch
//                   [1:2] magx     (AiCtrlMagx)     SR factor selector
//
// Hardware correspondence: a register write in the driver is always a
// per-field RMW, so reading side-effects can never leak into adjacent fields.
// g_sout_ctrl is the simulated REG_SOUT_CTRL hardware word: single writer
// (recfg APPLY action, Dispatcher thread); main thread reads it only after
// the recfg transaction window has closed (same discipline as g_zoom /
// g_sel / g_wrape — "WHY THIS IS NOT A BLACKBOARD").
// ---------------------------------------------------------------------------
struct SoutCtrlTag {};
struct AiCtrlTag   {};

using SoutCtrlEnable = coact::BitFieldView<SoutCtrlTag, 0U, 1U>;
using SoutCtrlOpcode = coact::BitFieldView<SoutCtrlTag, 4U, 4U>;
using SoutCtrlMode   = coact::BitFieldView<SoutCtrlTag, 8U, 2U>;
using AiCtrlBypass   = coact::BitFieldView<AiCtrlTag,   0U, 1U>;
using AiCtrlMagx     = coact::BitFieldView<AiCtrlTag,   1U, 2U>;

// Compile-time guard: the five field views in this register block must
// never share a bit — disjoint masks prove it at template instantiation.
static_assert(coact::fields_disjoint<SoutCtrlEnable, SoutCtrlOpcode>(),
              "SoutEnable / SoutOpcode fields overlap");
static_assert(coact::fields_disjoint<SoutCtrlEnable, SoutCtrlMode>(),
              "SoutEnable / SoutMode fields overlap");
static_assert(coact::fields_disjoint<SoutCtrlOpcode, SoutCtrlMode>(),
              "SoutOpcode / SoutMode fields overlap");
static_assert(coact::fields_disjoint<AiCtrlBypass, AiCtrlMagx>(),
              "AiBypass / AiMagx fields overlap");

// Toy REG_SOUT_CTRL opcode vocabulary (stand-in for the SOUT opcodes).
inline constexpr std::uint32_t kSoutOpcodeFrame = 0x2U;
inline constexpr std::uint32_t kSoutOpcodeRecfg = 0xAU;
inline constexpr std::uint32_t kSoutModeActive  = 0x2U;
inline constexpr std::uint32_t kSoutEnableOn    = 0x1U;

// PeriphRegCache indices the demo scenarios use for the bit-field layer.
// Picked to NOT overlap with scenarios A/B/C's scalar indices (0..5).
inline constexpr std::uint16_t kRegSoutCtrl = 6U;
inline constexpr std::uint16_t kRegAiCtrl   = 7U;

// The simulated REG_SOUT_CTRL hardware word: enabled, framing opcode, in
// active mode at boot. Single writer below (recfg APPLY); readers observe
// it from the main thread only after the recfg transaction closed.
inline std::uint32_t g_sout_ctrl{(kSoutModeActive << 8U)
                                 | (kSoutOpcodeFrame << 4U)
                                 | kSoutEnableOn};


// ---------------------------------------------------------------------------
// RecfgOrchestrator AO: the runtime atomic-reconfiguration transaction.
//
// Models the five-step method (precheck -> snapshot -> quiesce -> apply ->
// resume+commit) as an event-driven HSM. Key properties, each fixing a
// documented failure mode:
//   - Stage enum replaces boolean combinations; recovery is a closed switch.
//   - old/target snapshots: software state commits LAST (std::exchange only
//     after first-frame confirmation), never before hardware.
//   - Precheck is read-only: a rejected request touches zero hardware.
//   - Fault injection reproduces the "X2 premature EOF": during APPLY the
//     SOUT layer frames with the OLD geometry -> the sink sees a 1/4-frame
//     truncation, exactly like the 655360B capture. A second pass without
//     injection (single-authority geometry) frames correctly.
//   - layout_version bump invalidates stale address caches on first query.
// ---------------------------------------------------------------------------
struct RecfgAoCtx {
    PoolT*   pool{nullptr};
    Rt*      rt{nullptr};
    TargetId self_target{};          // this AO (simulated hw acks come home)
    TargetId report_to{};            // who gets kRecfgDone

    RecfgStage stage{RecfgStage::kIdle};
    SrMagx     active_magx{SrMagx::kX1};
    SrMagx     target_magx{SrMagx::kX1};
    FrameGeometry active_geom{};     // committed authority
    FrameGeometry target_geom{};
    FrameGeometry old_geom_snap{};   // snapshot for recovery
    uint32_t   layout_version{1U};
    bool       inject_stale_sout{false};

    // Stream mode the request arrived under (PreviewCmdInfo.mode): DMO =
    // passive mode, whose SOUT quiesce sequence is unreliable (Change 16105:
    // a DMO+SOUT combination slipped through validation). See DmoQuiesce
    // above for the two-step fix; the precheck guard is step 1.
    StreamMode stream_mode{StreamMode::k3Loop};

    // Fault evidence gathered from the sink: bytes the SOUT actually framed
    // vs the authority's bytes_per_frame().
    uint32_t   observed_frame_bytes{0U};
    uint32_t   stale_cache_hits{0U};   // queries against a dead layout version
    uint32_t   recfgs_committed{0U};
    uint32_t   recfgs_failed{0U};
    uint32_t   dmo_rejects{0U};        // precheck rejects for stream_mode=DMO

    // advance() drives the stage machine; each stage is one RTC step.
    void advance();
};

// Version-guarded address record: a stale record dies on FIRST query.
// (Implements the doc's "版本号" fix for the stale-address failure.)
struct AddrRecord {
    uint32_t version{0U};
    uint32_t addr{0U};
    bool     valid{false};
};

struct AddrCache {
    uint32_t layout_version{1U};
    AddrRecord rec{};

    // Explicit context conversion: `if (cache)` reads "has a live record" —
    // explicit so an AddrCache never silently becomes an int/pointer.
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return rec.valid && rec.version == layout_version;
    }

    // Query: hit only if the record was written under the CURRENT layout.
    [[nodiscard]] bool query(uint32_t& out_addr) noexcept
    {
        if (rec.valid && rec.version == layout_version) {
            out_addr = rec.addr;
            return true;
        }
        if (rec.valid && rec.version != layout_version) {
            // stale record: dead on first query — the failure mode becomes
            // a miss, never a wrong-address hit.
            rec = AddrRecord{};
        }
        return false;
    }
    void store(uint32_t addr) noexcept
    {
        // Placement new: construct the record IN PLACE inside the cache slot
        // — no temporary, no copy; the same discipline the event pool uses
        // (::new (block) Layout{}) and a DMA descriptor write models.
        ::new (static_cast<void*>(&rec)) AddrRecord{layout_version, addr, true};
    }
};

inline AddrCache g_addr_cache;   // demo-wide: invalidated by layout bump

// ---------------------------------------------------------------------------
// Per-arc transition actions. Each action does ONE stage's work, updates the
// mirror stage enum, and chains the next event — the HSM topology (above)
// owns the control flow; these functions own the effects.
// ---------------------------------------------------------------------------

// Helper: submit a self-addressed event (the simulated hardware ack).
static void rc_self_submit(RecfgAoCtx& ctx, Sig sig)
{
    Layout* e = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(
        static_cast<uint16_t>(sig));
    if (nullptr != e) {
        e->meta.reply_to = ctx.self_target;
        ctx.rt->coordinator().submit_from_task(ctx.self_target, &e->event,
                                               {false, false});
    }
}

// Arc: Idle -> Precheck (request accepted; reject recovers to Idle).
inline void rcEnterPrecheck(RecfgAoCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    ctx.target_magx = static_cast<SrMagx>(e.meta.cmd_arg & 0xFFU);
    ctx.inject_stale_sout = (0U != (e.meta.cmd_arg & 0x100U));

    // Read-only validation: zero hardware touch on reject.
    if (!magx_supported(ctx.target_magx)) {
        std::printf("[recfg] PRECHECK REJECT: magx=%u unsupported\n",
                    static_cast<unsigned>(ctx.target_magx));
        /* Structured: a0=1(precheck-reject) a1=magx. */
        g_log.record_from_task<LogLevel::kWarn, kEvtRecfg>(
            0U, 1U, static_cast<uint32_t>(ctx.target_magx));
        ++ctx.recfgs_failed;
        ctx.stage = RecfgStage::kIdle;
        HsmTrace::rejection("recfg", "Idle",
                            static_cast<uint16_t>(Sig::kRecfgReq),
                            "X4 unsupported (capability matrix)");
        // Self-drive home: the kRecfgStage arc walks the HSM back to Idle.
        rc_self_submit(ctx, Sig::kRecfgStage);
        return;
    }
    // DMO guard (two-step fix, step 1 — 嵌入式显示链路原子重配设计.md §5.3):
    // partial RECFG requires the ACTIVE-mode stop-stable sequence
    // (SOUT_STOP_FRAME -> frame-boundary IDLE). Under DMO (passive mode) that
    // sequence is unreliable — the request takes the full rebuild path
    // instead. Step 2 (DmoQuiesce: upstream stop + STREAM_DONE +
    // PASSIVE_DATA_LOSS fallback) stays compile-time-only until hardware
    // verification flips kDmoQuiesceVerified.
    if (StreamMode::kDmo == ctx.stream_mode && !DmoQuiesce::allow_partial_recfg()) {
        std::printf("[recfg] PRECHECK REJECT: stream_mode=DMO "
                    "(passive quiesce unverified, full rebuild required)\n");
        /* Structured: a0=1(precheck-reject) a1=0xFE(DMO marker). */
        g_log.record_from_task<LogLevel::kWarn, kEvtRecfg>(0U, 1U, 0xFEU);
        ++ctx.recfgs_failed;
        ++ctx.dmo_rejects;
        ctx.stage = RecfgStage::kIdle;
        HsmTrace::rejection("recfg", "Idle",
                            static_cast<uint16_t>(Sig::kRecfgReq),
                            "stream_mode=DMO (full rebuild)");
        // Same self-driven walk-home as the capability reject: the window
        // closes on the transaction's own terminal arc.
        rc_self_submit(ctx, Sig::kRecfgStage);
        return;
    }
    // Snapshot (commit happens LAST).
    ctx.old_geom_snap = ctx.active_geom;
    ctx.target_geom = apply_magx(ctx.active_geom, ctx.target_magx);
    ctx.stage = RecfgStage::kQuiescing;
    HsmTrace::transition("recfg", "Idle", static_cast<uint16_t>(Sig::kRecfgReq),
                         "Quiescing");
    std::printf("[recfg] precheck OK (magx x%u): %ux%u -> %ux%u\n",
                static_cast<unsigned>(ctx.target_magx),
                ctx.old_geom_snap.width, ctx.old_geom_snap.height,
                ctx.target_geom.width, ctx.target_geom.height);
    /* Structured: a0=2(precheck-ok) a1=magx a2=old_wh a3=new_wh. */
    g_log.record_from_task<LogLevel::kInfo, kEvtRecfg>(
        0U, 2U, static_cast<uint32_t>(ctx.target_magx),
        (static_cast<uint32_t>(ctx.old_geom_snap.width) << 16U)
            | ctx.old_geom_snap.height,
        (static_cast<uint32_t>(ctx.target_geom.width) << 16U)
            | ctx.target_geom.height);
    // SOUT_STOP_FRAME is issued by the Quiescing ENTRY action (below): the
    // transition action runs BEFORE the state switch, so self-submitting the
    // idle ack here would race the topology.
}

// Terminal action: the caller already updated the mirror; this arc only
// exists so the HSM topology matches it (every terminal outcome returns to
// Idle through a declared transition, never by mutation). It also closes the
// session's recfg-transaction window (RUNNING.RECFG_TXN -> RUNNING).
// SELF-DRIVEN WINDOW CLOSE: every transaction outcome (precheck reject,
// commit, recover) funnels through this single arc, so the window is closed
// exactly here — by the transaction's own terminal action on the Dispatcher
// thread, never by an external observer. The exchange only fires when the
// window is actually open, so back-to-back transactions close it once each.
// Same thread-safety class as the main-thread advance: the atomic exchange
// plus the (unbuffered) trace print.
inline void rcGoHome(RecfgAoCtx& ctx, const Event&)
{
    (void)ctx;
    if (SessionState::kRecfgTxn
        == g_session.load(std::memory_order_relaxed)) {
        session_advance(SessionState::kRunning, "recfg txn terminal (self)");
    }
    HsmTrace::transition("recfg", "PrecheckOrCommit",
                         static_cast<uint16_t>(Sig::kRecfgStage), "Idle");
}

// Quiescing ENTRY: issue SOUT_STOP_FRAME; the idle ack (frame boundary)
// arrives as an event once this state owns the flow.
inline void rcQuiescingEntry(RecfgAoCtx& ctx)
{
    HsmTrace::transition("recfg", "Quiescing",
                         static_cast<uint16_t>(Sig::kSoutIdle), "Applying");
    rc_self_submit(ctx, Sig::kSoutIdle);
}

// Resume ENTRY: issue SOUT_START; the first frame drives the commit arc.
inline void rcResumeEntry(RecfgAoCtx& ctx)
{
    HsmTrace::transition("recfg", "Resuming",
                         static_cast<uint16_t>(Sig::kFirstFrame), "Commit");
    rc_self_submit(ctx, Sig::kFirstFrame);
}

// Arc: Quiescing -> Applying (SOUT idle at the frame boundary).
inline void rcEnterSync(RecfgAoCtx& ctx, const Event&);   // chained below
inline void rcEnterApply(RecfgAoCtx& ctx, const Event&)
{
    ctx.stage = RecfgStage::kApplying;
    // Dependency order: AI -> DMA width -> geometry -> clock -> SOUT.
    // SINGLE-AUTHORITY rule: every layer reads target_geom.
    //
    // SOUT_CTRL: re-issue the opcode for the new geometry through the field
    // view — only opcode[4:7] moves; mode[8:9] (the stream mode the FSM
    // owns) and enable[0] MUST survive the reconfiguration. A whole-word
    // rewrite here would invert the SOUT opcode/mode contract — the failure
    // class this bit-field structure exists to prevent. Same word, two
    // fields, one read-modify-write — exactly the "register = field group"
    // property the scalar PeriphRegCache lost.
    SoutCtrlOpcode::write(g_sout_ctrl, kSoutOpcodeRecfg);
    if (ctx.inject_stale_sout) {
        // FAULT INJECTION (the documented bug): SOUT frames with the OLD
        // geometry while AI already produces X2 — truncated transfer.
        ctx.observed_frame_bytes = ctx.old_geom_snap.bytes_per_frame();
        std::printf("[recfg] FAULT: SOUT frames %u B (stale X1) "
                    "but stream is %u B (X2) -> truncated\n",
                    ctx.observed_frame_bytes, ctx.target_geom.bytes_per_frame());
        /* Structured: a0=3(fault) a1=observed_B a2=target_B. */
        g_log.record_from_task<LogLevel::kWarn, kEvtRecfg>(
            0U, 3U, ctx.observed_frame_bytes,
            ctx.target_geom.bytes_per_frame());
    } else {
        ctx.observed_frame_bytes = ctx.target_geom.bytes_per_frame();
    }
    ctx.stage = RecfgStage::kSyncing;
    // FS_SYNC is its own arc (Apply -> Syncing); self-drive it.
    rc_self_submit(ctx, Sig::kRecfgStage);
}

// Arc: Applying -> Syncing (self-driven after the register writes).
inline void rcEnterSync(RecfgAoCtx& ctx, const Event&)
{
    // Layout version bump: stale address records die on their next query.
    const uint32_t previous =
        std::exchange(ctx.layout_version, ctx.layout_version + 1U);
    g_addr_cache.layout_version = ctx.layout_version;
    std::printf("[recfg] sync: layout_version %u -> %u "
                "(stale address records die on first query)\n",
                previous, ctx.layout_version);
    /* Structured: a0=4(sync) a1=prev_version a2=new_version. */
    g_log.record_from_task<LogLevel::kInfo, kEvtRecfg>(
        0U, 4U, previous, ctx.layout_version);
    ctx.stage = RecfgStage::kResuming;
    // Syncing -> Resuming is its own arc; the Resume ENTRY issues SOUT_START.
    rc_self_submit(ctx, Sig::kRecfgStage);
}

// Arc: Resuming -> Commit (first frame proved the new config live).
inline void rcEnterCommit(RecfgAoCtx& ctx, const Event&)
{
    // Commit boundary: the first frame must PROVE the target geometry. A
    // truncated frame RECOVERS to the snapshot — software never commits a
    // config the hardware disproved.
    if (ctx.observed_frame_bytes != ctx.target_geom.bytes_per_frame()) {
        std::printf("[recfg] RECOVER: frame %u B != authority %u B -> "
                    "rollback to old snapshot\n",
                    ctx.observed_frame_bytes,
                    ctx.target_geom.bytes_per_frame());
        /* Structured: a0=5(recover) a1=observed_B a2=authority_B. */
        g_log.record_from_task<LogLevel::kWarn, kEvtRecfg>(
            0U, 5U, ctx.observed_frame_bytes,
            ctx.target_geom.bytes_per_frame());
        ctx.active_geom = ctx.old_geom_snap;
        ctx.active_magx = SrMagx::kX1;
        ++ctx.recfgs_failed;
        ctx.stage = RecfgStage::kIdle;
        rc_self_submit(ctx, Sig::kRecfgStage);
        return;
    }
    // Final submit: only NOW does software commit.
    ctx.active_magx = std::exchange(ctx.target_magx, SrMagx::kX1);
    ctx.active_geom = std::exchange(ctx.target_geom, FrameGeometry{});
    ++ctx.recfgs_committed;
    std::printf("[recfg] COMMIT: active=x%u %ux%u (%u B), "
                "observed frame = %u B (authority match)\n",
                static_cast<unsigned>(ctx.active_magx),
                ctx.active_geom.width, ctx.active_geom.height,
                ctx.active_geom.bytes_per_frame(),
                ctx.observed_frame_bytes);
    /* Structured: a0=6(commit) a1=magx a2=active_wh a3=frame_B. */
    g_log.record_from_task<LogLevel::kInfo, kEvtRecfg>(
        0U, 6U, static_cast<uint32_t>(ctx.active_magx),
        (static_cast<uint32_t>(ctx.active_geom.width) << 16U)
            | ctx.active_geom.height,
        ctx.observed_frame_bytes);
    ctx.stage = RecfgStage::kIdle;
    rc_self_submit(ctx, Sig::kRecfgStage);
}

// HSM states: one state per reconfig stage. External transitions with NULL
// entry/exit actions; the stage actions run in the transition actions and
// the ctx.stage mirror (kept for the summary/recovery enumeration).
enum : int8_t {
    kRcRoot     = 0,
    kRcIdle     = 1,   // waiting for a request
    kRcPrecheck = 2,   // UNREACHABLE (historical): no arc ever enters this
                       // state (precheck validation runs in the Idle->Quiescing
                       // transition action). Kept as a table placeholder — the
                       // kRecfgStates[] rows are index-addressed, so removing
                       // this entry would shift every later state's index.
    kRcQuiesce  = 3,   // SOUT_STOP issued, waiting frame-boundary IDLE
    kRcApply    = 4,   // dependency-ordered register writes
    kRcSync     = 5,   // FS_SYNC + layout version bump
    kRcResume   = 6,   // SOUT_START, waiting first frame
    kRcCommit   = 7,   // final submit / recover decision
};

// Transition actions (one per arc; the advance() logic split per state).
void rcEnterPrecheck(RecfgAoCtx& ctx, const Event& evt);
void rcEnterQuiesce(RecfgAoCtx& ctx, const Event& evt);
void rcEnterApply(RecfgAoCtx& ctx, const Event& evt);
void rcEnterSync(RecfgAoCtx& ctx, const Event& evt);
void rcEnterResume(RecfgAoCtx& ctx, const Event& evt);
void rcEnterCommit(RecfgAoCtx& ctx, const Event& evt);

// Entry actions: hardware commands are issued ONCE THE STATE IS ENTERED —
// never from a transition action (which runs before the state switch).
inline void rcQuiescingEntry(RecfgAoCtx& ctx);   // SOUT_STOP_FRAME
inline void rcResumeEntry(RecfgAoCtx& ctx);      // SOUT_START

inline const StateDef<RecfgAoCtx> kRecfgStates[] = {
    { -1, nullptr, nullptr, "Root" },
    { kRcRoot, nullptr, nullptr, "Idle" },
    { kRcRoot, nullptr, nullptr, "Precheck" },
    { kRcRoot, rcQuiescingEntry, nullptr, "Quiescing" },
    { kRcRoot, nullptr, nullptr, "Applying" },
    { kRcRoot, nullptr, nullptr, "Syncing" },
    { kRcRoot, rcResumeEntry, nullptr, "Resuming" },
    { kRcRoot, nullptr, nullptr, "Commit" },
};

// Stage guards: an arc only fires when the mirror stage matches, so the
// event vocabulary stays small while the topology stays explicit.
[[nodiscard]] inline bool at_precheck(const RecfgAoCtx& c, const Event&) noexcept
{ return RecfgStage::kPrecheck == c.stage; }
[[nodiscard]] inline bool at_quiesce(const RecfgAoCtx& c, const Event&) noexcept
{ return RecfgStage::kQuiescing == c.stage; }
[[nodiscard]] inline bool at_resume(const RecfgAoCtx& c, const Event&) noexcept
{ return RecfgStage::kResuming == c.stage; }
// Terminal-walk guards: the self-submitted kRecfgStage that carries a
// transaction home must only fire when the ctx mirror is in that terminal
// posture. A STALE kRecfgStage (queued by the previous transaction while a
// new request already re-entered) otherwise aborts the live transaction —
// the exact interleaving the stress runs exposed.
[[nodiscard]] inline bool at_reject_home(const RecfgAoCtx& c, const Event&) noexcept
{ return RecfgStage::kIdle == c.stage; }
[[nodiscard]] inline bool at_commit_home(const RecfgAoCtx& c, const Event&) noexcept
{ return RecfgStage::kIdle == c.stage; }
// Self-driven pipeline arcs: each arc's own self-submit is recognized by the
// mirror's stage value as left by the PREVIOUS action (rcEnterApply leaves
// kSyncing; rcEnterSync leaves kResuming); anything else is a stale event and
// falls through to the absorbing Internal arcs.
[[nodiscard]] inline bool at_syncing(const RecfgAoCtx& c, const Event&) noexcept
{ return RecfgStage::kSyncing == c.stage; }
[[nodiscard]] inline bool at_resuming(const RecfgAoCtx& c, const Event&) noexcept
{ return RecfgStage::kResuming == c.stage; }

inline const TransitionDef<RecfgAoCtx> kRecfgTransitions[] = {
    // Idle -> Quiescing: request accepted; the action validates (a reject
    // self-recovers home from the Quiescing terminal arc below).
    { kRcIdle, static_cast<uint16_t>(Sig::kRecfgReq), kRcQuiesce,
      TransitionKind::External, nullptr, rcEnterPrecheck },
    // Quiescing -> Applying: the frame-boundary IDLE ack arrived.
    { kRcQuiesce, static_cast<uint16_t>(Sig::kSoutIdle), kRcApply,
      TransitionKind::External, at_quiesce, rcEnterApply },
    // Resuming -> Commit: the first frame proved the new config.
    { kRcResume, static_cast<uint16_t>(Sig::kFirstFrame), kRcCommit,
      TransitionKind::External, at_resume, rcEnterCommit },
    // Self-driven pipeline arcs: Apply -> Syncing -> Resuming, each guarded
    // by its stage mirror (stale events fall through to the absorbers below).
    { kRcApply, static_cast<uint16_t>(Sig::kRecfgStage), kRcSync,
      TransitionKind::External, at_syncing, rcEnterSync },
    { kRcApply, static_cast<uint16_t>(Sig::kRecfgStage), kRcApply,
      TransitionKind::Internal, nullptr, nullptr },   // stale stage event
    { kRcSync, static_cast<uint16_t>(Sig::kRecfgStage), kRcResume,
      TransitionKind::External, at_resuming, nullptr },
    { kRcSync, static_cast<uint16_t>(Sig::kRecfgStage), kRcSync,
      TransitionKind::Internal, nullptr, nullptr },   // stale stage event
    // Terminal arcs: Quiescing (reject) / Commit (done) walk home to Idle.
    // Stage-guarded (see at_reject_home): only the terminal posture's own
    // self-submit fires; a stale one falls through to the no-op foreign arc
    // below, which absorbs it without disturbing the live transaction.
    { kRcQuiesce, static_cast<uint16_t>(Sig::kRecfgStage), kRcIdle,
      TransitionKind::External, at_reject_home, rcGoHome },
    { kRcQuiesce, static_cast<uint16_t>(Sig::kRecfgStage), kRcQuiesce,
      TransitionKind::Internal, nullptr, nullptr },   // stale stage event
    { kRcCommit, static_cast<uint16_t>(Sig::kRecfgStage), kRcIdle,
      TransitionKind::External, at_commit_home, rcGoHome },
    { kRcCommit, static_cast<uint16_t>(Sig::kRecfgStage), kRcCommit,
      TransitionKind::Internal, nullptr, nullptr },   // stale stage event
};

// ---------------------------------------------------------------------------
// Blackboard C: register-mirror domain (regmap-style register cache; models
// the cache-coherency doc's three failure scenarios and their regmap-style
// protocol fix). Same four-element contract as the other blackboards:
//   W (writer): the register-cache scenario in main, THROUGH write() only
//       (the single entry; a bare assignment elsewhere is the 2.1 fault the
//       drift auditor exists to catch). cache_bypass/cache_only modes are
//       entered/left via paired RAII guards with a mandatory resync.
//   R (reader): the scenario's verify paths and the drift auditor (both
//       copies compared); reads happen on the scenario thread.
//   S (sync):   no lock needed — the whole scenario runs on one thread with
//       no in-flight events between the copies; coherency is enforced by
//       PROTOCOL (write-through single entry + sync convergence point), not
//       by mutual exclusion. That is the point the demo makes: coherency of
//       two copies of one truth is a protocol property, not a lock property.
//   I (invalidation): bypass mode forks the copies (hardware drifts); the
//       paired guard's resync on exit invalidates the fork before it can
//       propagate. cache_only freezes hardware and defers shadow commits to
//       the sync boundary — dirty_regs tracks exactly which fields are stale.
//
// State: software shadow (the authority) + simulated hardware registers.
// Protocol: write-through single entry, paired cache_bypass with resync,
// cache_only with dirty set and a sync convergence point that ASSERTS it is
// not called while still cache_only, plus a drift auditor.
//
// Failure scenarios simulated:
//   2.1 param backflow: a bare bypass write (factory test) forks the two
//       copies; a later full re-push replays STALE shadow values over the
//       tuned registers. The paired guard (RAII) resyncs on exit, so the
//       drift never survives.
//   3.5 cache_only + dirty: frame-atomic parameter application accumulates
//       dirty during the freeze window; sync commits them at the boundary.
//   2.3 coordinate desync: display mirrors, detection boxes don't — fixed
//       by routing ALL coordinates through one transform authority.
// ---------------------------------------------------------------------------
constexpr uint16_t kRegCount = 8U;

// The two copies of the truth.
struct PeriphState {
    std::array<uint32_t, kRegCount> shadow{};    // software (authoritative)
    std::array<uint32_t, kRegCount> hardware{};  // simulated registers
};

struct PeriphRegCache {
    PeriphState st{};
    bool cache_only{false};
    bool cache_bypass{false};
    bool cache_dirty{false};
    std::array<bool, kRegCount> dirty_regs{};

    uint32_t bypass_writes{0U};    // stats: bare vs guarded
    uint32_t drift_events{0U};     // auditor findings
    uint32_t syncs{0U};

    // Runtime first-line guard (the regmap WARN_ON): mode combinations that
    // make no sense are rejected at the setter, not in code review.
    [[nodiscard]] bool modes_legal() const noexcept
    {
        return !(cache_only && cache_bypass);
    }

    // Write-through single entry: both copies update in one call. The ONLY
    // sanctioned way to change a parameter — bypassing this function is a
    // structural violation the auditor will catch.
    void write(uint16_t reg, uint32_t val) noexcept
    {
        if (cache_bypass) {
            // Controlled bypass: hardware only; resync on guard exit.
            st.hardware[reg] = val;
            ++bypass_writes;
            return;
        }
        if (cache_only) {
            // Device unwritable (freeze window): record in the shadow, mark
            // dirty; sync commits later at the frame boundary.
            st.shadow[reg] = val;
            cache_dirty = true;
            dirty_regs[reg] = true;
            return;
        }
        // Default write-through: hardware first, shadow only on success.
        st.hardware[reg] = val;
        st.shadow[reg] = val;
    }

    // Bit-field entry: the SAME three-flag protocol as write() (bypass ->
    // hardware only; cache_only -> shadow + dirty; default -> write-through),
    // but the value lands through a BitFieldView RMW — only the field's
    // bits move; adjacent fields in the same word are NEVER touched. The
    // sanctioned way to change one parameter that shares a word with others
    // (the scalar write() lands the whole 32-bit value and would clobber
    // its neighbors — the failure mode the bit-field layer exists to stop).
    template <typename Field>
    void write_field(std::uint16_t reg, std::uint32_t val) noexcept
    {
        if (cache_bypass) {
            Field::write(st.hardware[reg], val);
            ++bypass_writes;
            return;
        }
        if (cache_only) {
            Field::write(st.shadow[reg], val);
            cache_dirty = true;
            dirty_regs[reg] = true;
            return;
        }
        Field::write(st.hardware[reg], val);
        Field::write(st.shadow[reg], val);
    }

    // Field read from the shadow copy (the authority).
    template <typename Field>
    [[nodiscard]] std::uint32_t read_field(std::uint16_t reg) const noexcept
    {
        return Field::read(st.shadow[reg]);
    }

    // Full parameter re-push (the 2.1 failure trigger): replays the shadow
    // into hardware. If the shadow is stale (bare bypass forked the copies),
    // this silently overwrites tuned hardware values.
    void full_repush() noexcept
    {
        for (uint16_t r = 0; r < kRegCount; ++r) {
            st.hardware[r] = st.shadow[r];
        }
    }

    // resync: pull the CURRENT hardware values back into the shadow (the
    // cache_bypass exit companion). After this the two copies agree again.
    void resync_from_hw() noexcept
    {
        st.shadow = st.hardware;
    }

    // Convergence point: write all dirty shadow values into hardware.
    // ASSERTS it is not called while still in cache_only (the doc's -EINVAL
    // precondition: "in a not-truly-written state, refuse to commit").
    [[nodiscard]] bool sync() noexcept
    {
        if (cache_only) { return false; }   // refuse: still frozen
        for (uint16_t r = 0; r < kRegCount; ++r) {
            if (dirty_regs[r]) {
                st.hardware[r] = st.shadow[r];
                dirty_regs[r] = false;
            }
        }
        cache_dirty = false;
        ++syncs;
        return true;
    }

    // Drift auditor (debug-time periodic check): warn-only, never repair —
    // surfaces delayed drift at the moment it happens.
    void audit(const char* when) noexcept
    {
        for (uint16_t r = 0; r < kRegCount; ++r) {
            if (st.shadow[r] != st.hardware[r]) {
                ++drift_events;
                std::printf("[audit] %s: reg %u drift cache=0x%x hw=0x%x\n",
                            when, r, st.shadow[r], st.hardware[r]);
            }
        }
    }
};

// RAII paired-bypass guard: entering factory test flips cache_bypass; ANY
// exit path resyncs the shadow from hardware. The pairing is structural —
// an early return cannot skip the resync (the doc's "bypass must be paired").
class BypassGuard {
public:
    explicit BypassGuard(PeriphRegCache& c) noexcept : c_(c)
    {
        c_.cache_bypass = true;
    }
    ~BypassGuard()
    {
        c_.cache_bypass = false;
        c_.resync_from_hw();   // exit = align, always
    }
    BypassGuard(const BypassGuard&) = delete;
    BypassGuard& operator=(const BypassGuard&) = delete;

private:
    PeriphRegCache& c_;
};

// ---------------------------------------------------------------------------
// Coordinate transform authority (2.3 fix): ONE place that maps display
// geometry changes onto business coordinates. Points, lines and rects all
// pass through it — nobody transforms coordinates on their own.
// ---------------------------------------------------------------------------
struct DisplayMirror {
    bool horizontal{false};
    uint16_t width{0U};

    [[nodiscard]] constexpr uint16_t map_x(uint16_t x) const noexcept
    {
        return horizontal ? static_cast<uint16_t>(width - 1U - x) : x;
    }
};

struct DetPoint { uint16_t x, y; };
struct DetRect  { uint16_t x0, y0, x1, y1; };

// The single transform authority.
[[nodiscard]] constexpr DetPoint transform(DetPoint p, const DisplayMirror& m) noexcept
{
    return DetPoint{ m.map_x(p.x), p.y };
}
[[nodiscard]] constexpr DetRect transform(DetRect r, const DisplayMirror& m) noexcept
{
    // Corners swap under mirror; re-normalize so x0 <= x1.
    const uint16_t nx0 = m.map_x(r.x0);
    const uint16_t nx1 = m.map_x(r.x1);
    return DetRect{ nx0 < nx1 ? nx0 : nx1, r.y0, nx0 < nx1 ? nx1 : nx0, r.y1 };
}


// AO alias: runtime-reconfiguration transaction AO.
using RecfgOrchAo    = coact::Ao<RecfgAoCtx, Hsm<RecfgAoCtx>, AoTrait<61>>;


}  // namespace isp_demo
