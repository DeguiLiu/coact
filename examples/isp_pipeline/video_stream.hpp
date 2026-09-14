/*
 * ===========================================================================
 * video_stream.hpp —— 视频流 FSM + 打包（合并 PIC/TEMP 双流）
 * ===========================================================================
 * 功能描述
 *   - VideoFsmAo：合并 PIC + TEMP 两条 stream 的 FSM，IDLE → READY → RUNNING；
 *     PIC 7 节点（ISP_CUT_ZOOM / VIDEO_CUT_ZOOM / PSD / OSD_0 / OSD_1 /
 *     SOUT / OUT）init 顺序，TEMP 4 节点；deinit 逆向；子状态按 stream 分支
 *     （PIC_* / TEMP_*）由事件 kind bit 路由。命令源 kVideoCmd。
 *   - VideoPackAo：合并 PIC/TEMP 打包，子态 PIC_ACTIVE/TEMP_ACTIVE → SOUT →
 *     ACTIVE，SoutDmaWorker 的 kSoutDone[0/1] 按 cmd_arg 区分两流；
 *     PIC 输出 kPicPacked → WRAPE；TEMP 输出 kTempPacked → MIPI sink。
 *   - Quiesce（Change16517 仿真）：EventQuiesce / PollQuiesce / QuiescePolicy；
 *     SOUT 帧边界 idle ack（kSoutIdle）事件驱动确认，避免闪屏。
 *   对应 RS500 module/video（video_lsv_stream_manager、video_com_stream_base）
 *   与 SOUT writeback 路径。
 *
 * 与其他文件的关系
 *   - 上游：main/orchestrator 发 kVideoCmd（kVInit/kVStart/kVStop/kVDeinit）；
 *     [isp_chain] 发 kEnhanceDonePic/kEnhanceDoneTemp → pack；SoutDmaWorker
 *     的 kSoutDone 回到 pack。include common.hpp + isp_chain.hpp（事件/类型
 *     一致性；本文件独立编译，isp_chain.hpp 仅为类型面引用）。
 *   - 下游：kPicPacked 送 [output_itf] WrapeAo；kTempPacked 送 MipiSinkAo；
 *     kVideoReady 汇到 OrchestratorAo；kSoutIdle 给 RecfgOrchestrator 收敛
 *     重配事务的 quiesce 阶段。
 *   - 依赖：VideoFsmTrait（3ms RTC budget）；kVStart 后 FSM 切到 RUNNING 才
 *     接受产帧事件，与 session gate（kRunning 之内）配合。
 *
 * 文字图（视频流视角）
 * ┌──────────────────────────────────────────────────────────────────┐
 * │ 例子系统数据流（→事件信号  ⇒DDR/黑板数据）                        │
 * │                                                                  │
 * │  [sensor_irsc]──kFrameIrscOut──▶[isp_chain]──kHlFused/Done──▶   │
 * │                                  [video_stream]──kPicPacked──▶  │
 * │                              (本文件：FSM/VideoPack/quiesce)     │
 * │                                  [output_itf]──kFrameEof──▶     │
 * │                                  [winhost 观察者]                │
 * │                                                                  │
 * │  编排: [main.cpp] ──kIrscCmd/kVideoCmd──▶ 各模块 AO             │
 * │  重配: [recfg_session] ◀──kRecfgReq── main；门控 g_session      │
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
#include "coact/hsm.hpp"
#include "coact/runtime.hpp"

#include "common.hpp"
#include "isp_chain.hpp"

namespace isp_demo {

// ---------------------------------------------------------------------------
// VideoStreamFSM: mirrors video_lsv_stream_manager + video_com_stream_base.
// Init forward (nodes[i].init -> stream.init_drv), deinit reverse, FSM
// IDLE -> READY -> RUNNING. PIC and TEMP both go through this template.
// ---------------------------------------------------------------------------
enum VideoFsmState : uint8_t { kVideoIdle = 0U, kVideoReady = 1U, kVideoRunning = 2U };
enum VideoCmd : uint8_t { kVInit = 0U, kVStart = 1U, kVStop = 2U, kVDeinit = 3U };
enum VideoStreamKind : uint8_t { kPicStream = 0U, kTempStream = 1U };

// Merged Video FSM context: ONE AO, TWO per-path mirrors (PIC + TEMP). The
// two streams ran structurally identical FSMs in two AOs; the merge moves the
// stream distinction into the HSM states (PIC_*/TEMP_* subtrees) while the
// context keeps one mirror struct per path, selected by the event's kind bit.
struct VideoPathMirror {
    VideoFsmState fsm{kVideoIdle};
    uint32_t init_step{0U};
    uint32_t deinit_step{0U};
    uint8_t node_count{0U};
    VideoStreamKind kind{kPicStream};
    uint32_t rejected_cmds{0U};
};

struct VideoCtx {
    PoolT*   pool{nullptr};
    Rt*      rt{nullptr};
    TargetId self_target{};
    TargetId orchestrator{};
    // Node sequences in init order; counts differ (PIC 7, TEMP 4).
    static constexpr uint8_t kMaxNodes = 7U;
    const char* node_names[kMaxNodes]{};
    VideoPathMirror pic{};
    VideoPathMirror temp{};

    // ---- deinit quiesce accounting (the Change16517 simulation) ----
    uint32_t sout_ioctls{0U};       // SOUT stop sequence ioctl count
    uint32_t sout_quiesce_us{0U};   // SOUT stop -> idle latency
    uint32_t fmt_ioctls{0U};        // Format888 stop sequence ioctl count
    uint32_t fmt_quiesce_us{0U};    // Format888 stop -> idle latency
    bool     fmt_event_driven{false}; // true = unified event-driven fix path

    [[nodiscard]] VideoPathMirror& mirror_of(VideoStreamKind k) noexcept
    {
        return (kPicStream == k) ? pic : temp;
    }
};
static_assert(std::is_standard_layout<VideoPathMirror>::value,
              "VideoPathMirror is the per-stream FSM mirror of the merged AO");
static_assert(std::is_trivially_copyable<VideoPathMirror>::value,
              "VideoPathMirror must be memcpy-able (mirror snapshot/restore)");
static_assert(std::is_standard_layout<VideoCtx>::value,
              "VideoCtx is the merged video FSM context (two mirrors)");
static_assert(std::is_trivially_copyable<VideoCtx>::value,
              "VideoCtx holds only pointers + counters + mirrors");

inline const char* video_state_name(VideoFsmState s)
{
    switch (s) {
        case kVideoIdle: return "IDLE";
        case kVideoReady: return "READY";
        case kVideoRunning: return "RUNNING";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Deinit quiesce: two stop-confirmation mechanisms, one per hardware block.
//
// This models the Change16517 finding: the SAME deinit flow confirms SOUT
// quiesce via interrupt events (0 CPU, us-precision) but Format888 via a
// 500-iteration status poll (500 ioctls, ms-precision) — because the FMT888
// register group exposes no IDLE interrupt source.
//
// The strategies below are compile-time policies; `kFmtHasIdleIrq` is the
// hardware capability knob. The unified fix path (interrupt source added) is
// selected with if constexpr — zero runtime branching, and the poll loop
// code is not even instantiated when the interrupt exists.
// ---------------------------------------------------------------------------

// Hardware capability: does the Format888 block expose an IDLE interrupt?
// (Currently false — the documented reason for the polling fallback.)
inline constexpr bool kFmtHasIdleIrq = false;

// Simulated hardware stop latency (us): how long after STOP the block
// actually reaches idle. SOUT stops at the frame boundary (fast); the
// Format888 converter must drain its pipeline (slower).
inline constexpr uint32_t kSoutStopLatencyUs = 3000U;
inline constexpr uint32_t kFmtStopLatencyUs = 25000U;   // 25ms drain

// Simulated tick granularity: rt_thread_delay(1) rounds to this.
inline constexpr uint32_t kPollTickUs = 1000U;

// ---- Strategy 1: event-driven quiesce (SOUT today; FMT888 after the fix) --
// One ioctl to stop + interrupt enable/disable; the IDLE event wakes us.
struct EventQuiesce {
    // Returns quiesce latency in us. ioctl cost: 3 (stop, int_enable, int_disable).
    [[nodiscard]] static uint32_t wait(uint32_t hw_latency_us, uint32_t& ioctls) noexcept
    {
        ioctls += 3U;
        // The ISR fires at hardware-idle: us-precision observation.
        // (In the coact model this is literally an event: kSoutIdle/kFmtIdle
        // arrive from the block AO; the waiter never spins.)
        return hw_latency_us;
    }
};

// ---- Strategy 2: status polling (Format888 today) -------------------------
// One ioctl to stop, then poll STATUS.RUNNING every tick until it clears.
struct PollQuiesce {
    [[nodiscard]] static uint32_t wait(uint32_t hw_latency_us, uint32_t& ioctls) noexcept
    {
        ioctls += 1U;   // the STOP ioctl
        // Poll loop: STATUS_GET ioctl per tick until hardware stops. Every
        // iteration is an ioctl + context switch (rt_thread_delay(1)).
        const uint32_t polls =
            (hw_latency_us + kPollTickUs - 1U) / kPollTickUs;
        ioctls += polls;
        // Precision is tick-granular: we may observe idle up to one tick late.
        return polls * kPollTickUs;
    }
};

// The unified quiesce facade: one type, two compile-time behaviors. If the
// block has an interrupt source, the event path is instantiated; otherwise
// the poll path. Callers see the same signature either way.
template <bool HasIdleIrq>
struct QuiescePolicy {
    [[nodiscard]] static uint32_t quiesce(uint32_t hw_latency_us,
                                          uint32_t& ioctls) noexcept
    {
        if constexpr (HasIdleIrq) {
            return EventQuiesce::wait(hw_latency_us, ioctls);
        } else {
            return PollQuiesce::wait(hw_latency_us, ioctls);
        }
    }
};

using SoutQuiesce   = QuiescePolicy<true>;    // SOUT_INT.IDLE exists
using FmtQuiesceNow = QuiescePolicy<kFmtHasIdleIrq>;  // today: poll
using FmtQuiesceFix = QuiescePolicy<true>;    // after the mid-term fix


// CRTP FSM skeleton: the per-cmd behavior is identical between the two
// streams; every per-stream difference is a Derived hook (mirror_of / label /
// kind_value / ack), resolved statically through the Derived type — the same
// static-polymorphism family as WorkerBase::derived().execute(), expressed
// with static hooks because the base holds no per-instance state.
template <typename Derived>
struct VideoFsmNode {
    // Select this stream's mirror through the Derived hook.
    [[nodiscard]] static VideoPathMirror& mirror(VideoCtx& ctx) noexcept
    {
        return Derived::mirror_of(ctx);
    }

    // kVInit: IDLE -> READY (guarded arc — the transition table only accepts
    // it from the path's IDLE state; a re-init lands on the reject arc).
    static void on_init(VideoCtx& ctx, const Event&)
    {
        VideoPathMirror& m = mirror(ctx);
        /* 阻塞记账（原真睡 400+120*n+200us）：video FSM init 的驱动同步耗时。
           改为记账不睡眠——链上累计 ~1.44ms 低于 3ms RTC 预算、无断路器
           断言依赖真实时间流逝，真睡只占住 Dispatcher 拖慢所有 AO 派发。
           kRtcBudgetNs 的预算语义见 VideoFsmTrait 注释（保留为契约文档） */
        m.init_step += 1U + m.node_count + 1U;
        m.fsm = kVideoReady;
        Derived::ack(ctx);
        std::printf("[video/%s] IDLE -> READY (%u nodes)\n",
                    Derived::label(), m.node_count);
        HsmTrace::transition("video_fsm",
                             (kPicStream == m.kind) ? "PIC_I" : "TEMP_I",
                             static_cast<uint16_t>(Sig::kVideoCmd),
                             (kPicStream == m.kind) ? "PIC_R" : "TEMP_R");
        g_log.record_from_task<LogLevel::kInfo, kEvtVideoCmd>(
            Derived::kind_value(), kVInit, m.node_count);
    }

    // kVStart: READY -> RUNNING (guarded: only from READY).
    static void on_start(VideoCtx& ctx, const Event&)
    {
        VideoPathMirror& m = mirror(ctx);
        /* 阻塞记账（原真睡 300us）：DEV_ISP_STREAM_CTRL_STREAM_ENABLE 的
           流控寄存器写耗时。无断言依赖，改记账不睡眠（同 on_init 理由） */
        m.fsm = kVideoRunning;
        std::printf("[video/%s] READY -> RUNNING\n", Derived::label());
        HsmTrace::transition("video_fsm",
                             (kPicStream == m.kind) ? "PIC_R" : "TEMP_R",
                             static_cast<uint16_t>(Sig::kVideoCmd),
                             (kPicStream == m.kind) ? "PIC_G" : "TEMP_G");
        g_log.record_from_task<LogLevel::kInfo, kEvtVideoCmd>(
            Derived::kind_value(), kVStart, m.fsm);
    }

    // kVStop: RUNNING -> READY (guarded: only from RUNNING).
    static void on_stop(VideoCtx& ctx, const Event&)
    {
        VideoPathMirror& m = mirror(ctx);
        /* 阻塞记账（原真睡 200us）：video FSM stop 的流控驱动同步耗时。
           无断言依赖，改记账不睡眠（同 on_init 理由） */
        m.fsm = kVideoReady;
        std::printf("[video/%s] RUNNING -> READY\n", Derived::label());
        HsmTrace::transition("video_fsm",
                             (kPicStream == m.kind) ? "PIC_G" : "TEMP_G",
                             static_cast<uint16_t>(Sig::kVideoCmd),
                             (kPicStream == m.kind) ? "PIC_R" : "TEMP_R");
    }

    // kVDeinit: READY -> IDLE with the quiesce-confirmation narrative.
    static void on_deinit(VideoCtx& ctx, const Event&)
    {
        VideoPathMirror& m = mirror(ctx);
        // PIC-only: the two quiesce mechanisms comparison (Change16517).
        if (kPicStream == m.kind) {
            ctx.sout_quiesce_us =
                SoutQuiesce::quiesce(kSoutStopLatencyUs, ctx.sout_ioctls);
            ctx.fmt_quiesce_us =
                FmtQuiesceNow::quiesce(kFmtStopLatencyUs, ctx.fmt_ioctls);
            ctx.fmt_event_driven = false;
            std::printf("[video/PIC] deinit quiesce: SOUT event-driven "
                        "(%u ioctls, %u us) | FMT888 poll (%u ioctls, %u us) "
                        "-> INCONSISTENT mechanisms\n",
                        ctx.sout_ioctls, ctx.sout_quiesce_us,
                        ctx.fmt_ioctls, ctx.fmt_quiesce_us);
            uint32_t fix_ioctls = 0U;
            const uint32_t fix_us =
                FmtQuiesceFix::quiesce(kFmtStopLatencyUs, fix_ioctls);
            std::printf("[video/PIC] deinit quiesce (unified fix): FMT888 "
                        "event-driven (%u ioctls, %u us) -> consistent\n",
                        fix_ioctls, fix_us);
        }
        // reverse: nodes deinit in reverse, then stream deinit
        /* 阻塞记账（原真睡 80*n+150us）：各节点逆序 node.deinit_drv +
           stream.deinit_drv 驱动同步耗时。无断言依赖，改记账不睡眠 */
        m.deinit_step += m.node_count + 1U;
        m.fsm = kVideoIdle;
        std::printf("[video/%s] READY -> IDLE (deinit reverse)\n",
                    Derived::label());
        HsmTrace::transition("video_fsm",
                             (kPicStream == m.kind) ? "PIC_R" : "TEMP_R",
                             static_cast<uint16_t>(Sig::kVideoCmd),
                             (kPicStream == m.kind) ? "PIC_I" : "TEMP_I");
        g_log.record_from_task<LogLevel::kInfo, kEvtVideoCmd>(
            Derived::kind_value(), kVDeinit, m.deinit_step);
    }
};

// ---- derived: PIC stream ---------------------------------------------------
struct PicFsmNode : VideoFsmNode<PicFsmNode> {
    static constexpr VideoStreamKind kind_value() noexcept { return kPicStream; }
    static constexpr const char* label() noexcept { return "PIC"; }
    static VideoPathMirror& mirror_of(VideoCtx& ctx) noexcept { return ctx.pic; }
    static void ack(VideoCtx& ctx)
    {
        Layout* ack = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(Sig::kVideoReady));
        if (nullptr == ack) { return; }
        ack->meta.cmd_arg = kVInit;
        ack->meta.reply_to = ctx.orchestrator;
        ack->meta.payload_kind = 0U;   // PIC
        Payload* p = reinterpret_cast<Payload*>(&ack->payload[0]);
        p->control[0] = 'V'; p->control[1] = 'P'; p->control[2] = 'R'; p->control[3] = 'Y';
        ctx.rt->coordinator().submit_from_task(ctx.orchestrator, &ack->event, {true, false});
    }
};

// ---- derived: TEMP stream --------------------------------------------------
struct TempFsmNode : VideoFsmNode<TempFsmNode> {
    static constexpr VideoStreamKind kind_value() noexcept { return kTempStream; }
    static constexpr const char* label() noexcept { return "TEMP"; }
    static VideoPathMirror& mirror_of(VideoCtx& ctx) noexcept { return ctx.temp; }
    static void ack(VideoCtx& ctx)
    {
        Layout* ack = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(Sig::kVideoReady));
        if (nullptr == ack) { return; }
        ack->meta.cmd_arg = kVInit;
        ack->meta.reply_to = ctx.orchestrator;
        ack->meta.payload_kind = 1U;   // TEMP
        Payload* p = reinterpret_cast<Payload*>(&ack->payload[0]);
        p->control[0] = 'V'; p->control[1] = 'T'; p->control[2] = 'R'; p->control[3] = 'Y';
        ctx.rt->coordinator().submit_from_task(ctx.orchestrator, &ack->event, {true, false});
    }
};

// Event routing: kVideoCmd carries the stream in flags bit0 (command-channel
// stamp), the cmd in cmd_arg. Guards + actions live in the transition table.
// Product-state table: the coact HSM has ONE active leaf (no orthogonal
// regions), so two independent stream FSMs in one AO are expressed as the
// reachable PRODUCT of their states — PIC x TEMP, 3x3 = 9 states. State
// names encode (PIC phase, TEMP phase): first letter PIC, second TEMP,
// I=IDLE R=READY G=RUNNING. This is the "real HSM" form of the merge: the
// transition table is the complete, closed set of legal state pairs.
enum : int8_t {
    kVfRoot = 0,
    kVfII = 1,   // PIC IDLE,     TEMP IDLE
    kVfRI = 2,   // PIC READY,    TEMP IDLE
    kVfGI = 3,   // PIC RUNNING,  TEMP IDLE
    kVfIR = 4,   // PIC IDLE,     TEMP READY
    kVfRR = 5,   // PIC READY,    TEMP READY
    kVfGR = 6,   // PIC RUNNING,  TEMP READY
    kVfIT = 7,   // PIC IDLE,     TEMP RUNNING
    kVfRT = 8,   // PIC READY,    TEMP RUNNING
    kVfGT = 9,   // PIC RUNNING,  TEMP RUNNING
};

inline const StateDef<VideoCtx> kVideoStates[] = {
    { -1,       nullptr, nullptr, "Root" },
    { kVfRoot,  nullptr, nullptr, "PIC_I/TEMP_I" },
    { kVfRoot,  nullptr, nullptr, "PIC_R/TEMP_I" },
    { kVfRoot,  nullptr, nullptr, "PIC_G/TEMP_I" },
    { kVfRoot,  nullptr, nullptr, "PIC_I/TEMP_R" },
    { kVfRoot,  nullptr, nullptr, "PIC_R/TEMP_R" },
    { kVfRoot,  nullptr, nullptr, "PIC_G/TEMP_R" },
    { kVfRoot,  nullptr, nullptr, "PIC_I/TEMP_G" },
    { kVfRoot,  nullptr, nullptr, "PIC_R/TEMP_G" },
    { kVfRoot,  nullptr, nullptr, "PIC_G/TEMP_G" },
};

inline void onVideoCmd(VideoCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    const VideoStreamKind stream =
        (0U != (e.meta.flags & 0x1U)) ? kTempStream : kPicStream;
    const VideoCmd cmd = static_cast<VideoCmd>(e.meta.cmd_arg);
    if (kPicStream == stream) {
        switch (cmd) {
            case kVInit:   PicFsmNode::on_init(ctx, evt);   break;
            case kVStart:  PicFsmNode::on_start(ctx, evt);  break;
            case kVStop:   PicFsmNode::on_stop(ctx, evt);   break;
            case kVDeinit: PicFsmNode::on_deinit(ctx, evt); break;
        }
    } else {
        switch (cmd) {
            case kVInit:   TempFsmNode::on_init(ctx, evt);   break;
            case kVStart:  TempFsmNode::on_start(ctx, evt);  break;
            case kVStop:   TempFsmNode::on_stop(ctx, evt);   break;
            case kVDeinit: TempFsmNode::on_deinit(ctx, evt); break;
        }
    }
}

// Reject arc: a kVideoCmd that is illegal in the active state (e.g. START
// in IDLE). Counted per path; the demo drives one deliberately to exercise
// the guard-reject path of the merged HSM.
inline void onVideoCmdRejected(VideoCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    const VideoStreamKind stream =
        (0U != (e.meta.flags & 0x1U)) ? kTempStream : kPicStream;
    VideoPathMirror& m = ctx.mirror_of(stream);
    ++m.rejected_cmds;
    HsmTrace::rejection("video_fsm",
                        (kPicStream == stream) ? "PIC" : "TEMP",
                        static_cast<uint16_t>(Sig::kVideoCmd),
                        "illegal cmd in current state");
}

// cmd_arg/flags extractors for the arc guards.
[[nodiscard]] inline VideoCmd video_cmd_of(const Event& evt) noexcept
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    return static_cast<VideoCmd>(e.meta.cmd_arg);
}
[[nodiscard]] inline bool cmd_is_pic(const Event& evt) noexcept
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    return 0U == (e.meta.flags & 0x1U);
}
[[nodiscard]] inline bool cmd_is_init(const Event& evt) noexcept
{ return kVInit == video_cmd_of(evt); }
[[nodiscard]] inline bool cmd_is_start(const Event& evt) noexcept
{ return kVStart == video_cmd_of(evt); }
[[nodiscard]] inline bool cmd_is_stop(const Event& evt) noexcept
{ return kVStop == video_cmd_of(evt); }
[[nodiscard]] inline bool cmd_is_deinit(const Event& evt) noexcept
{ return kVDeinit == video_cmd_of(evt); }
[[nodiscard]] inline bool pic_and_init(const VideoCtx&, const Event& e) noexcept
{ return cmd_is_pic(e) && cmd_is_init(e); }
[[nodiscard]] inline bool pic_and_start(const VideoCtx&, const Event& e) noexcept
{ return cmd_is_pic(e) && cmd_is_start(e); }
[[nodiscard]] inline bool pic_and_stop(const VideoCtx&, const Event& e) noexcept
{ return cmd_is_pic(e) && cmd_is_stop(e); }
[[nodiscard]] inline bool pic_and_deinit(const VideoCtx&, const Event& e) noexcept
{ return cmd_is_pic(e) && cmd_is_deinit(e); }
[[nodiscard]] inline bool temp_and_init(const VideoCtx&, const Event& e) noexcept
{ return !cmd_is_pic(e) && cmd_is_init(e); }
[[nodiscard]] inline bool temp_and_start(const VideoCtx&, const Event& e) noexcept
{ return !cmd_is_pic(e) && cmd_is_start(e); }
[[nodiscard]] inline bool temp_and_stop(const VideoCtx&, const Event& e) noexcept
{ return !cmd_is_pic(e) && cmd_is_stop(e); }
[[nodiscard]] inline bool temp_and_deinit(const VideoCtx&, const Event& e) noexcept
{ return !cmd_is_pic(e) && cmd_is_deinit(e); }

#define VF_ARC(Src, Guard, Dst)                                            \
    { Src, static_cast<uint16_t>(Sig::kVideoCmd), Dst,                     \
      TransitionKind::External, Guard, onVideoCmd }

// The closed product table. Legal arcs: one per (source pair, stream cmd)
// where the stream's source phase matches the command's precondition.
// Reject arcs: START of a stream whose phase is IDLE (the guard falls
// through the legal arcs and lands here) — same-signal Self transitions
// that count + trace instead of silently dropping.
inline const TransitionDef<VideoCtx> kVideoTransitions[] = {
    // PIC init: PIC IDLE -> READY (any TEMP phase)
    VF_ARC(kVfII, pic_and_init, kVfRI),
    VF_ARC(kVfIR, pic_and_init, kVfRR),
    VF_ARC(kVfIT, pic_and_init, kVfRT),
    // PIC start: PIC READY -> RUNNING
    VF_ARC(kVfRI, pic_and_start, kVfGI),
    VF_ARC(kVfRR, pic_and_start, kVfGR),
    VF_ARC(kVfRT, pic_and_start, kVfGT),
    // PIC stop: PIC RUNNING -> READY
    VF_ARC(kVfGI, pic_and_stop, kVfRI),
    VF_ARC(kVfGR, pic_and_stop, kVfRR),
    VF_ARC(kVfGT, pic_and_stop, kVfRT),
    // PIC deinit: PIC READY -> IDLE
    VF_ARC(kVfRI, pic_and_deinit, kVfII),
    VF_ARC(kVfRR, pic_and_deinit, kVfIR),
    VF_ARC(kVfRT, pic_and_deinit, kVfIT),
    // TEMP init: TEMP IDLE -> READY (any PIC phase)
    VF_ARC(kVfII, temp_and_init, kVfIR),
    VF_ARC(kVfRI, temp_and_init, kVfRR),
    VF_ARC(kVfGI, temp_and_init, kVfGR),
    // TEMP start: TEMP READY -> RUNNING
    VF_ARC(kVfIR, temp_and_start, kVfIT),
    VF_ARC(kVfRR, temp_and_start, kVfRT),
    VF_ARC(kVfGR, temp_and_start, kVfGT),
    // TEMP stop: TEMP RUNNING -> READY
    VF_ARC(kVfIT, temp_and_stop, kVfIR),
    VF_ARC(kVfRT, temp_and_stop, kVfRR),
    VF_ARC(kVfGT, temp_and_stop, kVfGR),
    // TEMP deinit: TEMP READY -> IDLE
    VF_ARC(kVfIR, temp_and_deinit, kVfII),
    VF_ARC(kVfRR, temp_and_deinit, kVfRI),
    VF_ARC(kVfGR, temp_and_deinit, kVfGI),
    // Rejects: START while the stream is IDLE (Self + counter + trace).
    { kVfII, static_cast<uint16_t>(Sig::kVideoCmd), kVfII,
      TransitionKind::Self, pic_and_start, onVideoCmdRejected },
    { kVfIR, static_cast<uint16_t>(Sig::kVideoCmd), kVfIR,
      TransitionKind::Self, pic_and_start, onVideoCmdRejected },
    { kVfIT, static_cast<uint16_t>(Sig::kVideoCmd), kVfIT,
      TransitionKind::Self, pic_and_start, onVideoCmdRejected },
    { kVfII, static_cast<uint16_t>(Sig::kVideoCmd), kVfII,
      TransitionKind::Self, temp_and_start, onVideoCmdRejected },
    { kVfRI, static_cast<uint16_t>(Sig::kVideoCmd), kVfRI,
      TransitionKind::Self, temp_and_start, onVideoCmdRejected },
    { kVfGI, static_cast<uint16_t>(Sig::kVideoCmd), kVfGI,
      TransitionKind::Self, temp_and_start, onVideoCmdRejected },
};
#undef VF_ARC

// The MERGED video FSM AO: both stream FSMs live in one HSM (PIC_* and
// TEMP_* subtrees as sibling states); the per-path mirrors live in the ctx.
struct VideoFsmTrait {
    static LogicalPrio logical_prio() { return static_cast<LogicalPrio>(47); }
    static PriorityClass priority_class() { return PriorityClass::Normal;
    }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    // The FSM's init/deinit chains deliberately model node bring-up/teardown
    // with usleep sums of ~1.5ms (RS500 drivers really are that slow); a 1ms
    // budget made the dispatcher RTC-timeout breaker quarantine the AO and
    // DROP the stop commands (observed as DroppedOverload under stress).
    // 3ms matches the real teardown bound.
    static constexpr uint64_t kRtcBudgetNs = 3000000ULL;
};
using VideoFsmAo = coact::Ao<VideoCtx, Hsm<VideoCtx>, VideoFsmTrait>;


// ---------------------------------------------------------------------------
// VideoPack AO (merged): one active object, two HSM sub-states for the PIC
// and TEMP streams — the two pack chains were structurally identical, so the
// stream distinction lives in the STATE MACHINE instead of in two AOs. The
// CRTP base factors the shared skeleton (latency accounting, SOUT writeback
// request, frame release); the derived node supplies the per-stream policy
// hooks (label / region / transform / sink) — static dispatch, no vtables.
//
// Per-stream HSM (both, in parallel — sub-states are orthogonal regions of
// the merged AO's table; events are routed by guard on the flags bit, the
// same trick the FusedNode pair uses):
//
//   PIC_ACTIVE  --(kEnhanceDone)-->  PIC_SOUT
//   PIC_SOUT    --(kSoutDone[0])-->  PIC_ACTIVE      (frame -> WRAPE)
//   TEMP_ACTIVE --(kTempChainDone)--> TEMP_SOUT
//   TEMP_SOUT   --(kSoutDone[1])-->  TEMP_ACTIVE     (frame -> MIPI)
//
// The kSoutDone guard discriminates the two completion channels by cmd_arg,
// so a single SoutDmaWorker drives both streams without crosstalk.
// ---------------------------------------------------------------------------
struct VideoPackCtx {
    PoolT*    pool{nullptr};
    Rt*       rt{nullptr};
    DdrCtx*   ddr{nullptr};
    TargetId  orchestrator{};
    TargetId  pic_sink{};     // WRAPE
    TargetId  temp_sink{};    // MIPI sink
    TargetId  self_target{};  // this AO (self-submitted drop completions)
    SoutDmaWorker* sout_channel{nullptr};

    // ---- shared with the per-path mirror contexts (kept public for CRTP) --
    // Per-path parking rings (overlap-tolerant, frame-id matched) — same
    // discipline as FusedNodeCtx::parked.
    static constexpr uint8_t kParkDepth = 4U;
    uint32_t  pic_frames{0U};
    uint32_t  pic_total_us{0U};
    uint32_t  pic_last_pack_us{0U};
    uint32_t  pic_sout_subs{0U};
    uint32_t  pic_sout_done{0U};
    uint32_t  pic_sout_rejects{0U};
    uint32_t  pic_sout_stale{0U};   // completions matching no parked frame
    uint32_t  pic_in_flight{0U};
    IoMeta    pic_parked[kParkDepth]{};
    uint8_t   pic_park_head{0U};
    uint32_t  temp_frames{0U};
    uint32_t  temp_total_us{0U};
    uint32_t  temp_last_pack_us{0U};
    uint32_t  temp_sout_subs{0U};
    uint32_t  temp_sout_done{0U};
    uint32_t  temp_sout_rejects{0U};
    uint32_t  temp_sout_stale{0U};  // completions matching no parked frame
    uint32_t  temp_in_flight{0U};
    IoMeta    temp_parked[kParkDepth]{};
    uint8_t   temp_park_head{0U};
};
static_assert(std::is_standard_layout<VideoPackCtx>::value,
              "VideoPackCtx is the merged pack context (pair-state parking)");
static_assert(std::is_trivially_copyable<VideoPackCtx>::value,
              "VideoPackCtx must be memcpy-able (no hidden ownership)");

// CRTP base: Derived provides the compile-time policy surface
//   static constexpr uint16_t kPathId();         // SOUT channel discriminator
//   static constexpr DdrId in_region();          // stream input DDR region
//   static constexpr uint32_t pack_latency_us(); // stream node-sequence time
//   static void pack(uint8_t* in);               // stream-specific transform
//   static void account(VideoPackCtx&, uint32_t);        // latency counters
//   static void park(VideoPackCtx&, const IoMeta&);      // descriptor parking
//   static void count_sub(VideoPackCtx&); / count_reject(ctx)
//   static void on_sout_done(VideoPackCtx&);     // release downstream
template <typename Derived>
struct VideoPackNode {
    // Phase 1: data-plane repack (per-stream node sequence) + SOUT writeback
    // request. The frame descriptor is parked in the ctx until the writeback
    // IRQ returns; the parking slot is emptied with std::exchange in phase 2
    // (ownership moves out in one step, no read-then-clear race window).
    static void on_input(VideoPackCtx& ctx, const Event& evt)
    {
        const Layout& e = *reinterpret_cast<const Layout*>(&evt);

        uint8_t in[kSlotPayloadBytes];
        if (!ctx.ddr->read(Derived::in_region(), e.meta.buffer_idx,
                           e.meta.frame_id, in)) {
            fill_dn(in, e.meta.frame_id);
        }

        /* 阻塞记账（原真睡 pack_latency_us）：PIC/TEMP 打包（PSD/OSD 等 SOUT
           前置处理）的驱动业务延迟。latency 统计只依赖模拟值，不真睡 */
        Derived::pack(in);                       // stream-specific transform
        Derived::account(ctx, Derived::pack_latency_us());
        // Pack milestone (throttled): the pack-convergence point of the data
        // chain, one line per 10 frames per stream. Uses the per-path counter
        // Derived::account() just advanced (PIC=pic_frames, TEMP=temp_frames).
        {
            const uint32_t n = (Derived::kPathId() == 0U) ? ctx.pic_frames
                                                          : ctx.temp_frames;
            if (n % 10U == 0U) {
                std::printf("[pack] path=%s frames_packed=%u (frame_id=%u)\n",
                            (Derived::kPathId() == 0U) ? "pic" : "temp",
                            n, e.meta.frame_id);
            }
        }

        const SoutJob job{Derived::kPathId(), e.meta.frame_id};
        if (ctx.sout_channel->submit(job)) {
            Derived::park(ctx, e.meta);          // descriptor -> IRQ window
            Derived::count_sub(ctx);
        } else {
            // Busy SOUT channel: DROP the frame — the completion is
            // SELF-SUBMITTED for this frame so the pair-state HSM walks home
            // through its normal arc (counted, never a stall).
            Derived::count_reject(ctx);
            std::printf("[warn] sout_dma submit rejected (busy), frame=%u\n",
                        static_cast<unsigned>(e.meta.frame_id));
            Derived::park(ctx, e.meta);
            self_complete_sout(ctx, Derived::kPathId(), e.meta.frame_id);
        }
    }

    // Self-submitted kSoutDone for the drop path (see request_irq pattern).
    static void self_complete_sout(VideoPackCtx& ctx, uint16_t path,
                                   uint32_t frame_id)
    {
        Layout* e = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(Sig::kSoutDone));
        if (nullptr != e) {
            e->meta.cmd_arg = path;
            e->meta.frame_id = frame_id;
            ctx.rt->coordinator().submit_from_task(ctx.self_target,
                                                   &e->event, {false, false});
        }
    }

    // Phase 2: SOUT writeback done -> release the matching frame downstream.
    static void on_sout_done(VideoPackCtx& ctx, const Event& evt)
    {
        Derived::on_sout_done(ctx, evt);
    }
};

// ---- PIC path policy (CRTP derived): YUV422 repack, PIC sink ----
struct PicPackNode : VideoPackNode<PicPackNode> {
    static constexpr uint16_t kPathId() noexcept { return 0U; }
    static constexpr DdrId in_region() noexcept { return DdrId::kDdrPicOut; }
    static constexpr uint32_t pack_latency_us() noexcept { return kLat.pic_video_us; }

    // ISP_CUT_ZOOM -> VIDEO_CUT_ZOOM -> PSD -> OSD_0 -> OSD_1 -> SOUT -> OUT:
    // byte-preserving SOUT repack + OSD stamp (2x YUV422 expansion is a
    // local buffer op; pixels stay in DDR, only the descriptor moves).
    static void pack(uint8_t* in)
    {
        std::array<uint8_t, 2U * kSlotPayloadBytes> sout{};
        for (uint16_t i = 0; i < kSlotPayloadBytes; ++i) {
            sout[2U * i]      = in[i];                            // Y
            sout[2U * i + 1U] = static_cast<uint8_t>(in[i] >> 4); // Cb/Cr proxy
        }
    }
    static void account(VideoPackCtx& ctx, uint32_t us)
    {
        ctx.pic_last_pack_us = us;
        ctx.pic_total_us += us;
        ++ctx.pic_frames;
    }
    static void park(VideoPackCtx& ctx, const IoMeta& m)
    {
        ctx.pic_parked[ctx.pic_park_head] = m;
        ctx.pic_park_head = static_cast<uint8_t>(
            (ctx.pic_park_head + 1U) % VideoPackCtx::kParkDepth);
        ++ctx.pic_in_flight;
    }
    static void count_sub(VideoPackCtx& ctx) { ++ctx.pic_sout_subs; }
    static void count_reject(VideoPackCtx& ctx) { ++ctx.pic_sout_rejects; }

    // SOUT writeback completed: release the matching parked frame (frame-id
    // match against the parking ring — interleaving-tolerant) to the WRAPE.
    static void on_sout_done(VideoPackCtx& ctx, const Event& evt)
    {
        const Layout& e = *reinterpret_cast<const Layout*>(&evt);
        IoMeta* hit = nullptr;
        for (uint8_t i = 0U; i < VideoPackCtx::kParkDepth; ++i) {
            if (ctx.pic_parked[i].frame_id == e.meta.frame_id) {
                hit = &ctx.pic_parked[i];
                break;
            }
        }
        if (nullptr == hit) { ++ctx.pic_sout_stale; return; }
        Layout* out = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(Sig::kPicPacked));
        if (nullptr == out) {
            *hit = IoMeta{};
            if (ctx.pic_in_flight > 0U) { --ctx.pic_in_flight; }
            ++ctx.pic_sout_rejects;
            return;
        }
        out->meta = std::exchange(*hit, IoMeta{});   // ownership moves out
        out->meta.payload_kind = 1U;
        Payload* p = reinterpret_cast<Payload*>(&out->payload[0]);
        p->control[0] = 'P'; p->control[1] = 'I'; p->control[2] = 'C';
        p->control[3] = static_cast<uint8_t>(0xA0U + (out->meta.frame_id & 0x0FU));
        p->ddr_slot = out->meta.buffer_idx; p->ddr_id = DdrId::kDdrPicOut;
        if (ctx.pic_in_flight > 0U) { --ctx.pic_in_flight; }
        ++ctx.pic_sout_done;
        ctx.rt->coordinator().submit_from_task(ctx.pic_sink, &out->event, {false, false});
    }
};

// ---- TEMP path policy (CRTP derived): Y16 identity pass, MIPI sink ----
struct TempPackNode : VideoPackNode<TempPackNode> {
    static constexpr uint16_t kPathId() noexcept { return 1U; }
    static constexpr DdrId in_region() noexcept { return DdrId::kDdrTemp; }
    static constexpr uint32_t pack_latency_us() noexcept { return kLat.temp_video_us; }

    // ISP_CUT_ZOOM -> VIDEO_CUT_ZOOM -> PSD -> OUT: Y16 preserving pass
    // (PSD maps temp code to gray; identity here).
    static void pack(uint8_t*) noexcept {}
    static void account(VideoPackCtx& ctx, uint32_t us)
    {
        ctx.temp_last_pack_us = us;
        ctx.temp_total_us += us;
        ++ctx.temp_frames;
    }
    static void park(VideoPackCtx& ctx, const IoMeta& m)
    {
        ctx.temp_parked[ctx.temp_park_head] = m;
        ctx.temp_park_head = static_cast<uint8_t>(
            (ctx.temp_park_head + 1U) % VideoPackCtx::kParkDepth);
        ++ctx.temp_in_flight;
    }
    static void count_sub(VideoPackCtx& ctx) { ++ctx.temp_sout_subs; }
    static void count_reject(VideoPackCtx& ctx) { ++ctx.temp_sout_rejects; }

    // SOUT writeback completed: release the matching parked frame to MIPI.
    static void on_sout_done(VideoPackCtx& ctx, const Event& evt)
    {
        const Layout& e = *reinterpret_cast<const Layout*>(&evt);
        IoMeta* hit = nullptr;
        for (uint8_t i = 0U; i < VideoPackCtx::kParkDepth; ++i) {
            if (ctx.temp_parked[i].frame_id == e.meta.frame_id) {
                hit = &ctx.temp_parked[i];
                break;
            }
        }
        if (nullptr == hit) { ++ctx.temp_sout_stale; return; }
        Layout* out = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(Sig::kTempPacked));
        if (nullptr == out) {
            *hit = IoMeta{};
            if (ctx.temp_in_flight > 0U) { --ctx.temp_in_flight; }
            ++ctx.temp_sout_rejects;
            return;
        }
        out->meta = std::exchange(*hit, IoMeta{});   // ownership moves out
        out->meta.payload_kind = 2U;
        Payload* p = reinterpret_cast<Payload*>(&out->payload[0]);
        p->control[0] = 'T'; p->control[1] = 'E'; p->control[2] = 'M'; p->control[3] = 'P';
        p->ddr_slot = out->meta.buffer_idx; p->ddr_id = DdrId::kDdrTemp;
        if (ctx.temp_in_flight > 0U) { --ctx.temp_in_flight; }
        ++ctx.temp_sout_done;
        ctx.rt->coordinator().submit_from_task(ctx.temp_sink, &out->event, {false, false});
    }
};

// ---- merged AO HSM: the writeback pair-state machine ----------------------
// The coact HSM has one active leaf, so the two independent per-stream
// writeback windows are expressed as the PRODUCT of their two phases
// (ACTIVE / SOUT_PENDING): 2x2 = 4 states. BA = both streams active;
// PS/TS = one stream parked in its SOUT writeback window; BS = both parked.
// Every (state, event) pair is covered by an explicit arc — there is no
// silent-drop window even when both streams park simultaneously.
enum : int8_t {
    kPackRoot = 0,
    kPackBA = 1,   // PIC ACTIVE,    TEMP ACTIVE
    kPackPS = 2,   // PIC SOUT_PEND, TEMP ACTIVE
    kPackTS = 3,   // PIC ACTIVE,    TEMP SOUT_PEND
    kPackBS = 4,   // PIC SOUT_PEND, TEMP SOUT_PEND
};

inline const StateDef<VideoPackCtx> kPackStates[] = {
    { -1,          nullptr, nullptr, "Root" },
    { kPackRoot,   nullptr, nullptr, "PIC_A/TEMP_A" },
    { kPackRoot,   nullptr, nullptr, "PIC_SOUT/TEMP_A" },
    { kPackRoot,   nullptr, nullptr, "PIC_A/TEMP_SOUT" },
    { kPackRoot,   nullptr, nullptr, "PIC_SOUT/TEMP_SOUT" },
};

inline void onEnhanceDonePack(VideoPackCtx& ctx, const Event& evt)
{
    PicPackNode::on_input(ctx, evt);
    HsmTrace::transition("pack", "PIC_A",
                         static_cast<uint16_t>(Sig::kEnhanceDonePic),
                         "PIC_SOUT");
}
inline void onTempChainDonePack(VideoPackCtx& ctx, const Event& evt)
{
    TempPackNode::on_input(ctx, evt);
    HsmTrace::transition("pack", "TEMP_A",
                         static_cast<uint16_t>(Sig::kEnhanceDoneTemp),
                         "TEMP_SOUT");
}
// Per-path SOUT-done actions: the arc contract resolves the wire signal ONCE,
// on the kSoutDone External arc (the action re-stamps cmd_arg -> signal before
// dispatching the per-path id); these handlers run on the per-path arcs only.
inline void onSoutDonePicPack(VideoPackCtx& ctx, const Event& evt)
{
    PicPackNode::on_sout_done(ctx, evt);   // ring match inside
    HsmTrace::transition("pack", "PIC_SOUT",
                         static_cast<uint16_t>(Sig::kSoutDonePic), "PIC_A");
}
inline void onSoutDoneTempPack(VideoPackCtx& ctx, const Event& evt)
{
    TempPackNode::on_sout_done(ctx, evt);  // ring match inside
    HsmTrace::transition("pack", "TEMP_SOUT",
                         static_cast<uint16_t>(Sig::kSoutDoneTemp), "TEMP_A");
}

// The SOUT channel stamps the owning path in cmd_arg (command channel).
// Per-path SOUT-done signal ids: the single shared SoutDmaWorker stamps the
// owning path in cmd_arg, and the completion event carries it; the AO re-stamps
// the wire id onto the per-path id (cmd_arg -> signal) so the pair-state table
// can discriminate the two writeback completions BY SIGNAL — the same "distinct
// signal per arc contract" the merged product HSMs require of every fan-in
// producer (guards on payload bits are NOT usable as the only discriminator
// once two arcs share one (state, signal) cell).
[[nodiscard]] inline uint16_t sout_sig_of(const Event& evt) noexcept
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    return (0U == e.meta.cmd_arg)
        ? static_cast<uint16_t>(Sig::kSoutDonePic)
        : static_cast<uint16_t>(Sig::kSoutDoneTemp);
}
[[nodiscard]] inline bool sout_is_pic(const VideoPackCtx&, const Event& evt) noexcept
{
    return static_cast<uint16_t>(Sig::kSoutDonePic) == evt.signal;
}
[[nodiscard]] inline bool sout_is_temp(const VideoPackCtx&, const Event& evt) noexcept
{
    return static_cast<uint16_t>(Sig::kSoutDoneTemp) == evt.signal;
}

// Wire-signal restamp: the SoutDmaWorker delivers kSoutDone (channel id) with
// the owning path in cmd_arg. The pair-state table resolves the per-path
// completion BY SIGNAL; this action re-stamps the event id, then re-submits
// to SELF so the per-path arc fires on the per-path id. The self-submit is
// non-critical (a completion for an already-released frame is a counted
// stale, never a boot-critical command).
inline void onSoutDoneRestamp(VideoPackCtx& ctx, const Event& evt)
{
    const uint16_t path_sig = sout_sig_of(evt);
    Layout* e = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(path_sig);
    if (nullptr == e) { return; }
    const Layout& in = *reinterpret_cast<const Layout*>(&evt);
    e->meta = in.meta;
    e->meta.cmd_arg = 0U;   // consumed: the path now lives in the signal id
    ctx.rt->coordinator().submit_from_task(ctx.self_target, &e->event,
                                           {false, false});
}

// Complete (state, event) coverage: an input for one stream is processed
// regardless of the OTHER stream's writeback phase (the pair state is the
// observable "window open" indicator; the parking rings carry the truth).
inline const TransitionDef<VideoPackCtx> kPackTransitions[] = {
    // PIC input (kEnhanceDonePic): park the PIC writeback, from any state.
    { kPackBA, static_cast<uint16_t>(Sig::kEnhanceDonePic), kPackPS,
      TransitionKind::External, nullptr, onEnhanceDonePack },
    { kPackTS, static_cast<uint16_t>(Sig::kEnhanceDonePic), kPackBS,
      TransitionKind::External, nullptr, onEnhanceDonePack },
    { kPackPS, static_cast<uint16_t>(Sig::kEnhanceDonePic), kPackPS,
      TransitionKind::Internal, nullptr, onEnhanceDonePack },
    { kPackBS, static_cast<uint16_t>(Sig::kEnhanceDonePic), kPackBS,
      TransitionKind::Internal, nullptr, onEnhanceDonePack },
    // TEMP input (kEnhanceDoneTemp): park the TEMP writeback, from any state.
    { kPackBA, static_cast<uint16_t>(Sig::kEnhanceDoneTemp), kPackTS,
      TransitionKind::External, nullptr, onTempChainDonePack },
    { kPackPS, static_cast<uint16_t>(Sig::kEnhanceDoneTemp), kPackBS,
      TransitionKind::External, nullptr, onTempChainDonePack },
    { kPackTS, static_cast<uint16_t>(Sig::kEnhanceDoneTemp), kPackTS,
      TransitionKind::Internal, nullptr, onTempChainDonePack },
    { kPackBS, static_cast<uint16_t>(Sig::kEnhanceDoneTemp), kPackBS,
      TransitionKind::Internal, nullptr, onTempChainDonePack },
    // Wire completion (kSoutDone, path in cmd_arg): resolve to the per-path id
    // via the restamp action, from any state.
    { kPackBA, static_cast<uint16_t>(Sig::kSoutDone), kPackBA,
      TransitionKind::Internal, nullptr, onSoutDoneRestamp },
    { kPackPS, static_cast<uint16_t>(Sig::kSoutDone), kPackPS,
      TransitionKind::Internal, nullptr, onSoutDoneRestamp },
    { kPackTS, static_cast<uint16_t>(Sig::kSoutDone), kPackTS,
      TransitionKind::Internal, nullptr, onSoutDoneRestamp },
    { kPackBS, static_cast<uint16_t>(Sig::kSoutDone), kPackBS,
      TransitionKind::Internal, nullptr, onSoutDoneRestamp },
    // SOUT done (PIC): release the matching PIC frame, from any state.
    { kPackPS, static_cast<uint16_t>(Sig::kSoutDonePic), kPackBA,
      TransitionKind::External, nullptr, onSoutDonePicPack },
    { kPackBS, static_cast<uint16_t>(Sig::kSoutDonePic), kPackTS,
      TransitionKind::External, nullptr, onSoutDonePicPack },
    { kPackBA, static_cast<uint16_t>(Sig::kSoutDonePic), kPackBA,
      TransitionKind::Internal, nullptr, onSoutDonePicPack },
    { kPackTS, static_cast<uint16_t>(Sig::kSoutDonePic), kPackTS,
      TransitionKind::Internal, nullptr, onSoutDonePicPack },
    // SOUT done (TEMP): release the matching TEMP frame, from any state.
    { kPackTS, static_cast<uint16_t>(Sig::kSoutDoneTemp), kPackBA,
      TransitionKind::External, nullptr, onSoutDoneTempPack },
    { kPackBS, static_cast<uint16_t>(Sig::kSoutDoneTemp), kPackPS,
      TransitionKind::External, nullptr, onSoutDoneTempPack },
    { kPackBA, static_cast<uint16_t>(Sig::kSoutDoneTemp), kPackBA,
      TransitionKind::Internal, nullptr, onSoutDoneTempPack },
    { kPackPS, static_cast<uint16_t>(Sig::kSoutDoneTemp), kPackPS,
      TransitionKind::Internal, nullptr, onSoutDoneTempPack },
};


// AO alias: merged pack AO (ctx lives in this module).
using VideoPackAo = coact::Ao<VideoPackCtx, Hsm<VideoPackCtx>, AoTrait<45>>;


}  // namespace isp_demo
