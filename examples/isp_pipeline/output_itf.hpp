/*
 * ===========================================================================
 * output_itf.hpp —— 输出接口层（sink 校验 + WRAPE 成帧 + WinHost + 编排）
 * ===========================================================================
 * 功能描述
 *   - UsbSinkAo / MipiSinkAo：DDR 字节级校验 sink，对比 PIC/TEMP tag 与
 *     DdrCtx 实际字节（byte_mismatch / tag_mismatch / frame_overrun 计数）；
 *     MIPI 路径带 TxParkDepth=2 的 lane parking 环，与 MipiIrqWorker 完成
 *     对账（kMipiTxDone / kMipiStreamErr 仅计数不切 FSM）。
 *   - WrapeAo：按 FrameGeometry 给 PIC payload 成帧，读 HwStateBlackboard
 *     的 zoom/stream_sel/wrape 三块，决定是否切帧/重定边界；交付给
 *     UsbDmaWorker（来自 sensor_irsc.hpp）搬运。
 *   - WinHostAo：Windows 主机观察者（LowAoTrait），接收 kFrameEof + 计数
 *     kUsbErrInt，纯被动（不驱动状态机）。
 *   - OrchestratorAo：收集 kIrscReady / kIspReady / kVideoReady ack，对应
 *     RS500 的 execute_config_phases_ex() + rte_call_sync 响应合并建模。
 *   - HwStateBlackboard：硬件状态黑板（ZoomBlock / StreamSelBlock /
 *     WrapeBlock 四要素），生产者写一次 / 消费者读多次；WrapeAo 落帧前
 *     必须读取以避免闪屏。
 *   对应 RS500 module/video/com/outITF（video_com_uvc.c / wrape）+ module/
 *   common/service_layer/out_itf_svc + rte 服务层。
 *
 * 与其他文件的关系
 *   - 上游：[video_stream] VideoPackAo 发 kPicPacked → WrapeAo；kTempPacked
 *     → MipiSinkAo；kVideoReady → OrchestratorAo；[sensor_irsc] UsbDmaWorker
 *     搬完发 kFrameEof / kUsbErrInt → WinHostAo；[isp_chain] MipiIrqWorker
 *     发 kMipiTxDone / kMipiStreamErr → MipiSinkAo。include common.hpp +
 *     isp_chain.hpp（事件/FrameStamp 类型一致）。
 *   - 下游：WinHostAo 是观察者终端；OrchestratorAo 不发事件，仅统计 ready
 *     计数（断言端在 main.cpp）；kPicPacked→WRAPE→UsbDmaWorker→kFrameEof
 *     构成 USB 数据路径闭环。
 *   - 依赖：HighAoTrait（orch）/ LowAoTrait（winhost）/ AoTrait<38..40>
 *     （wrape/sinks），符合 RTE 优先级分区。
 *
 * 文字图（输出视角）
 * ┌──────────────────────────────────────────────────────────────────┐
 * │ 例子系统数据流（→事件信号  ⇒DDR/黑板数据）                        │
 * │                                                                  │
 * │  [sensor_irsc]──kFrameIrscOut──▶[isp_chain]──kHlFused/Done──▶   │
 * │                                  [video_stream]──kPicPacked──▶  │
 * │                                  [output_itf]──kFrameEof──▶     │
 * │                              (本文件：sinks/WRAPE/WinHost/Orch)  │
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
#include <cstring>

#include "coact/ao.hpp"
#include "coact/hsm.hpp"
#include "coact/runtime.hpp"

#include "common.hpp"
#include "isp_chain.hpp"

namespace isp_demo {

// ---------------------------------------------------------------------------
// Sinks: USB (PIC) and MIPI (TEMP).
// ---------------------------------------------------------------------------
struct SinkCtx {
    const char* name{""};
    const char* expected_tag{""};
    DdrCtx*    ddr{nullptr};        // serve point: verify real DDR bytes
    uint32_t    frames_received{0U};
    uint32_t    total_us{0U};
    uint32_t    first_frame_id{0xFFFFFFFFU};
    uint32_t    last_frame_id{0U};
    uint32_t    tag_mismatch{0U};
    uint32_t    byte_mismatch{0U};
    uint32_t    frame_overrun{0U};   // DDR slot recycled before read (real
                                     // 3-frame ring overrun behavior)
    bool        banner_printed{false};
    // MIPI path only: async CSI TX completion channel.
    PoolT*      pool{nullptr};       // event plane (self-submitted completions)
    Rt*         rt{nullptr};
    MipiIrqWorker* tx_channel{nullptr};
    TargetId    self_target{};       // this AO (self-submitted completions)
    // Lane parking ring: frame ids handed to the TX line buffer and not yet
    // confirmed. Depth 2 matches the channel depth; completion matches by id.
    static constexpr uint8_t kTxParkDepth = 2U;
    uint32_t    tx_parked[kTxParkDepth]{};
    uint8_t     tx_park_head{0U};
    uint32_t    tx_subs{0U};         // frames handed to the line buffer
    uint32_t    tx_rejects{0U};      // line buffer full (dropped submits)
    uint32_t    tx_done_count{0U};   // TX-done interrupts observed
    uint32_t    tx_stale{0U};        // completions matching no parked frame
    uint32_t    stream_errs{0U};     // CSI stream-error interrupts observed
};

inline void check_tag(SinkCtx& ctx, const Layout& e)
{
    const Payload* p = reinterpret_cast<const Payload*>(&e.payload[0]);
    if (p->control[0] != static_cast<uint8_t>(ctx.expected_tag[0])
     || p->control[1] != static_cast<uint8_t>(ctx.expected_tag[1])
     || p->control[2] != static_cast<uint8_t>(ctx.expected_tag[2])) {
        ++ctx.tag_mismatch;
    }
}

// Data-plane verify: re-read the DDR region the frame lives in and confirm the
// pixels match the chain's deterministic transform from the DN seed.
//   PIC out   = 6/5 * fused, fused = (2*lg + 3*hg)/5
//   lg = 3/4*dn, hg = 5/4*dn
// We recompute the exact pipeline in uint32 to avoid rounding drift.
inline void verify_pic_bytes(SinkCtx& ctx, const Layout& e)
{
    uint8_t got[kSlotPayloadBytes];
    if (!ctx.ddr->read(DdrId::kDdrPicOut, e.meta.buffer_idx, e.meta.frame_id, got)) {
        ++ctx.frame_overrun;   // slot recycled before we read it
        return;
    }
    const uint32_t fid = e.meta.frame_id;
    for (uint16_t i = 0; i < kSlotPayloadBytes; ++i) {
        const uint32_t dn  = (fid * 7U + i) & 0xFFu;
        const uint32_t lg  = (dn * 3U) / 4U;
        const uint32_t hg  = ((dn * 5U) / 4U) & 0xFFu;   // chain truncates
        const uint32_t fus = ((lg * 2U) + (hg * 3U)) / 5U;
        const uint32_t exp = ((fus * 6U) / 5U) & 0xFFu;  // chain truncates
        if (got[i] != static_cast<uint8_t>(exp)) {
            ++ctx.byte_mismatch;
        }
    }
}
inline void verify_temp_bytes(SinkCtx& ctx, const Layout& e)
{
    uint8_t got[kSlotPayloadBytes];
    if (!ctx.ddr->read(DdrId::kDdrTemp, e.meta.buffer_idx, e.meta.frame_id, got)) {
        ++ctx.frame_overrun;   // slot recycled before we read it
        return;
    }
    const uint32_t fid = e.meta.frame_id;
    for (uint16_t i = 0; i < kSlotPayloadBytes; ++i) {
        const uint32_t dn  = (fid * 7U + i) & 0xFFu;
        const uint32_t lg  = (dn * 3U) / 4U;
        const uint32_t hg  = ((dn * 5U) / 4U) & 0xFFu;
        const uint32_t fus = ((lg * 2U) + (hg * 3U)) / 5U;
        const uint32_t exp = ((fus / 2U) + 32U) & 0xFFu;  // chain truncates
        if (got[i] != static_cast<uint8_t>(exp)) {
            ++ctx.byte_mismatch;
        }
    }
}

inline void onPicPacked(SinkCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    check_tag(ctx, e);
    verify_pic_bytes(ctx, e);   // data-plane byte check
    /* 阻塞记账（原真睡 800us）：USB UVC 帧提交耗时。latency 统计只依赖
       模拟值，无真实时间窗口断言，改记账不睡眠（T37 后 PIC 数据面走
       WRAPE，本 handler 已不在主数据路径上） */
    ctx.total_us += 800U;  // USB UVC submit (accounting only)
    if (ctx.first_frame_id == 0xFFFFFFFFU) { ctx.first_frame_id = e.meta.frame_id; }
    ctx.last_frame_id = e.meta.frame_id;
    ++ctx.frames_received;
    if (!ctx.banner_printed) {
        std::printf("[%s] first PIC frame: id=%u buf=%u kind=%u\n",
                    ctx.name, e.meta.frame_id, e.meta.buffer_idx, e.meta.payload_kind);
        g_log.record_from_task<LogLevel::kInfo, kEvtSinkFirst>(
            0U, e.meta.frame_id, e.meta.buffer_idx, e.meta.payload_kind);
        ctx.banner_printed = true;
    }
}

// Self-submitted TX-done (drop path): the frame never reached the lane; the
// synthetic completion walks the sink home through its normal arc.
inline void self_complete_tx(SinkCtx& ctx, uint32_t frame_id)
{
    Layout* e = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(
        static_cast<uint16_t>(Sig::kMipiTxDone));
    if (nullptr != e) {
        e->meta.frame_id = frame_id;
        ctx.rt->coordinator().submit_from_task(ctx.self_target, &e->event,
                                               {false, false});
    }
}

inline void onTempPacked(SinkCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    check_tag(ctx, e);
    verify_temp_bytes(ctx, e);   // data-plane byte check

    // Data-plane pass is DONE; the frame goes onto the MIPI TX line buffer.
    // The submit latency is paid by the IRQ worker (kMipiTxLatencyUs), not
    // here — the sink returns as soon as the lane accepts the descriptor.
    if (ctx.tx_channel != nullptr
        && ctx.tx_channel->submit(static_cast<uint16_t>(e.meta.frame_id))) {
        ++ctx.tx_subs;
        ctx.tx_parked[ctx.tx_park_head] = e.meta.frame_id;
        ctx.tx_park_head = static_cast<uint8_t>(
            (ctx.tx_park_head + 1U) % SinkCtx::kTxParkDepth);
    } else {
        // Busy line buffer: the frame is dropped; a self-submitted completion
        // for THIS frame walks the HSM home (counted, never a stall).
        ++ctx.tx_rejects;
        std::printf("[warn] mipi_irq submit rejected (busy), frame=%u\n",
                    static_cast<unsigned>(e.meta.frame_id));
        self_complete_tx(ctx, e.meta.frame_id);
    }
    if (ctx.first_frame_id == 0xFFFFFFFFU) { ctx.first_frame_id = e.meta.frame_id; }
    ctx.last_frame_id = e.meta.frame_id;
    if (!ctx.banner_printed) {
        std::printf("[%s] first TEMP frame: id=%u buf=%u\n",
                    ctx.name, e.meta.frame_id, e.meta.buffer_idx);
        g_log.record_from_task<LogLevel::kInfo, kEvtSinkFirst>(
            1U, e.meta.frame_id, e.meta.buffer_idx, e.meta.payload_kind);
        ctx.banner_printed = true;
    }
}

// TX-done interrupt: the frame is on the lane. Account completion here (the
// sink's observable "frame shipped" moment), exactly where the CSI ISR would
// fire in the real driver.
inline void onMipiTxDone(SinkCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    // Match: the completion must name a frame on the lane; a stale one is
    // counted and ignored (the parked frames keep waiting for their turn).
    bool hit = false;
    for (uint8_t i = 0U; i < SinkCtx::kTxParkDepth; ++i) {
        if (ctx.tx_parked[i] == e.meta.frame_id) {
            ctx.tx_parked[i] = 0xFFFFFFFFU;   // consume the parked id
            hit = true;
            break;
        }
    }
    if (!hit) {
        ++ctx.tx_stale;
        return;
    }
    // Latency paid on the worker; accounted here at the completion moment.
    ctx.total_us += kMipiTxLatencyUs;
    ++ctx.frames_received;
    ++ctx.tx_done_count;
    HsmTrace::transition("mipi", "TX_PENDING",
                         static_cast<uint16_t>(Sig::kMipiTxDone), "ACTIVE");
}

COACT_HSM_STATES(kSinkStates, SinkCtx, "Root", "Active");
COACT_HSM_TRANS(kUsbSinkTransitions, SinkCtx, Sig::kPicPacked, onPicPacked);

// CSI stream-error interrupt (RS500_MIPI图像输出原理.md §9): the AO counts
// the error — deliberately NO FSM transition (the manual's "interrupt does
// not auto-change the Stream FSM" rule); the frame on the lane stays parked
// until its own TX-done (or a stale accounting) resolves it. Injection-only.
inline void onMipiStreamErr(SinkCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    ++ctx.stream_errs;
    std::printf("[warn] mipi stream error interrupt: frame=%u "
                "(counted, FSM unchanged per §9 rule)\n",
                static_cast<unsigned>(e.meta.frame_id));
}

// MIPI sink HSM: ACTIVE --(kTempPacked)--> TX_PENDING (frame on the lane);
// TX_PENDING --(kMipiTxDone)--> ACTIVE (completion interrupt observed).
enum : int8_t { kMipiRoot = 0, kMipiActive = 1, kMipiTxPending = 2 };

inline const StateDef<SinkCtx> kMipiStates[] = {
    { -1,              nullptr, nullptr, "Root" },
    { kMipiRoot,       nullptr, nullptr, "ACTIVE" },
    { kMipiRoot,       nullptr, nullptr, "TX_PENDING" },
};

inline const TransitionDef<SinkCtx> kMipiSinkTransitions[] = {
    // Frame in: verify + hand to the lane, from any state.
    { kMipiActive,    static_cast<uint16_t>(Sig::kTempPacked), kMipiTxPending,
      TransitionKind::External, nullptr, onTempPacked },
    { kMipiTxPending, static_cast<uint16_t>(Sig::kTempPacked), kMipiTxPending,
      TransitionKind::Internal, nullptr, onTempPacked },
    // TX-done: match + release, from any state.
    { kMipiTxPending, static_cast<uint16_t>(Sig::kMipiTxDone), kMipiActive,
      TransitionKind::External, nullptr, onMipiTxDone },
    { kMipiActive,    static_cast<uint16_t>(Sig::kMipiTxDone), kMipiActive,
      TransitionKind::Internal, nullptr, onMipiTxDone },
    // kMipiStreamErr (§9): error interrupt — counted only, Internal from
    // either state (interrupt never auto-changes the Stream FSM).
    { kMipiTxPending, static_cast<uint16_t>(Sig::kMipiStreamErr), kMipiTxPending,
      TransitionKind::Internal, nullptr, onMipiStreamErr },
    { kMipiActive,    static_cast<uint16_t>(Sig::kMipiStreamErr), kMipiActive,
      TransitionKind::Internal, nullptr, onMipiStreamErr },
};


// ---------------------------------------------------------------------------
// UVC output chain (the T37 module set): the packed PIC stream leaves through
// Video Zoom (always active; X1 2x / X2 identity) -> Stream SEL (per-source
// valid count) -> USB TOP/WRAPE framing -> the Windows host observer.
//
// The T37 failure modeled here: with the X2 input fed through a WRAPE still
// configured for the X1 frame byte count, the frame ends at 655360 B with
// ERR+EOF — Windows sees the top-left quarter. The Identity-Zoom workaround
// keeps the downstream geometry (and WRAPE framing) fixed at 1280x1024 in
// BOTH modes, so the configuration can never lag the stream.
// =========================================================================
// Blackboard A: hardware-register snapshot domain (the T37 register groups).
//
// Design contract — every blackboard in this demo declares the same four
// elements (Writer / Reader / Sync / Invalidation):
//   W (writer): static init + main's T37 scenario, THROUGH THE NAMED SETTERS
//       below only (one field group = one auditable write site; direct field
//       assignment elsewhere is a structural violation, the same discipline
//       PeriphRegCache::write enforces).
//   R (reader): WrapeAo onPicPackedWrape (the framing verdict) + the final
//       checks in main — reads happen on the Dispatcher thread.
//   S (sync):   NO mutex. Writes happen ONLY at quiesce barriers: each T37
//       phase writes registers after t37_drain() returned, i.e. after the
//       host saw every in-flight frame EOF — the WRAPE has already consumed
//       all frames, so writer and reader never overlap. This is the real
//       driver discipline: reconfigure registers at stream boundaries, never
//       mid-frame. If a write ever moved into a concurrent window, the fix
//       is to submit it as a Dispatcher event (register writes belong on the
//       command channel), not to add a shared lock.
//   I (invalidation): X1<->X2 mode switches rewrite zoom.step ONLY;
//       sel.stream_vld_num is the constant downstream contract and is never
//       rewritten (Identity-Zoom depends on it); wrape.* is the T37 fault
//       surface, reset to aligned framing by phase 3.
//
// WHY THIS IS NOT THE BLACKBOARD ANTI-PATTERN: these fields model PHYSICAL
// REGISTERS (the Zoom/SSEL/WRAPE register groups of the RS500 manual) —
// hardware state that exists exactly once and is owned by the driver domain.
// Every field has a single writer (one sanctioned setter), reads happen on
// the same Dispatcher thread or behind quiesce barriers, and changes
// propagate to dependents via EVENTS (kModeSwitch), not by polling shared
// memory. THE BLACKBOARD ANTI-PATTERN would instead be: multiple independent
// producers writing one shared structure + consumers polling it + a
// controller coordinating them — the pattern the consistency docs show to be
// the source of U1-U9. Use that blackboard ONLY when (a) >=2 independent
// producers contribute partial results to one structure AND (b) consumers
// genuinely need the combined view (multi-sensor fusion); neither holds
// here, so the owner-AO discipline applies.
// =========================================================================
struct ZoomBlock {
    bool active{true};               // Identity rule: NEVER turned off
    uint16_t step{kZoomStep2x};      // 128 = 2x, 256 = identity
};
// Stream SEL (manual 7.5.5/7.5.6): selects the source stream entering the
// physical output interface. Each source stream carries its OWN valid-data
// count (streamVldNum = 1280x1024 = 1310720 valid pixels for the fixed UVC
// profile); the USB frame length is DERIVED from it (pixels x 2 B), never a
// separate constant. This register is the authority that keeps the downstream
// geometry constant across X1/X2 — exactly what Identity Zoom relies on.
struct StreamSelBlock {
    uint32_t stream_vld_num{kStreamVldNum};
};
struct WrapeBlock {
    uint32_t configured_frame_bytes{kOutFrameBytes};
    bool stale_x1_framing{false};    // the T37 bug switch
};

struct HwStateBlackboard {
    ZoomBlock      zoom;    // W: hw_bb_set_zoom_step / R: WRAPE geometry check
    StreamSelBlock sel;     // W: static init only / R: WRAPE frame-length derivation
    WrapeBlock     wrape;   // W: hw_bb_set_wrape_framing / R: WRAPE framing verdict
};
inline HwStateBlackboard g_hw_bb;

// Sanctioned write sites (the auditable register interface).
inline void hw_bb_set_zoom_step(uint16_t step) noexcept
{
    // Identity rule: only the coefficient changes; zoom.active stays true.
    g_hw_bb.zoom.step = step;
}
inline void hw_bb_set_wrape_framing(uint32_t frame_bytes, bool stale_x1) noexcept
{
    g_hw_bb.wrape.configured_frame_bytes = frame_bytes;
    g_hw_bb.wrape.stale_x1_framing = stale_x1;
}

// Framing verdict: how the WRAPE closes a frame vs its configured geometry.
struct FrameVerdict {
    uint32_t payload_bytes{0U};
    bool     err_eof{false};
};
// Compile-time framing policies (fault vs fix, same signature).
struct StaleFraming {
    [[nodiscard]] static constexpr FrameVerdict frame(uint32_t actual,
                                                      uint32_t configured) noexcept
    {
        return (actual > configured) ? FrameVerdict{configured, true}
                                     : FrameVerdict{actual, false};
    }
};
struct AlignedFraming {
    [[nodiscard]] static constexpr FrameVerdict frame(uint32_t actual,
                                                      uint32_t) noexcept
    {
        return FrameVerdict{actual, false};
    }
};


// ---------------------------------------------------------------------------
// WrapeAo: frames the packed PIC payload per its CONFIGURED geometry.
// ---------------------------------------------------------------------------
struct WrapeCtx {
    PoolT*   pool{nullptr};
    Rt*      rt{nullptr};
    DdrCtx*  ddr{nullptr};        // data-plane byte verification at the input
    UsbDmaWorker* dma_engine{nullptr};   // non-AO USB Bulk engine
    uint32_t frames_framed{0U};
    uint32_t err_eof_frames{0U};
    uint32_t complete_frames{0U};
    uint32_t byte_mismatch{0U};
    uint32_t tag_mismatch{0U};
    uint32_t zoom_geometry_mismatch{0U};
};

inline void onPicPackedWrape(WrapeCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);

    // Data-plane verification at the WRAPE input (was the USB sink's job):
    // re-read the DDR region and confirm the pipeline transform is intact.
    {
        const Payload* p = reinterpret_cast<const Payload*>(&e.payload[0]);
        if (p->control[0] != 'P' || p->control[1] != 'I' || p->control[2] != 'C') {
            ++ctx.tag_mismatch;
        }
        uint8_t got[kSlotPayloadBytes];
        if (!ctx.ddr->read(DdrId::kDdrPicOut, e.meta.buffer_idx,
                           e.meta.frame_id, got)) {
            ++ctx.byte_mismatch;   // slot overrun counts as corruption
        } else {
            const uint32_t fid = e.meta.frame_id;
            for (uint16_t i = 0; i < kSlotPayloadBytes; ++i) {
                const uint32_t dn  = (fid * 7U + i) & 0xFFu;
                const uint32_t lg  = (dn * 3U) / 4U;
                const uint32_t hg  = ((dn * 5U) / 4U) & 0xFFu;
                const uint32_t fus = ((lg * 2U) + (hg * 3U)) / 5U;
                const uint32_t exp = ((fus * 6U) / 5U) & 0xFFu;
                if (got[i] != static_cast<uint8_t>(exp)) {
                    ++ctx.byte_mismatch;
                }
            }
        }
    }

    // The stream delivered a full output-geometry frame; the WRAPE frames
    // per its OWN configured geometry (the T37 lag point).
    // Stream SEL (7.5.5/7.5.6): the selected source stream carries its OWN
    // valid pixel count; the USB frame length is DERIVED from it (pixels x 2B),
    // never a separate constant. This register is the module that keeps the
    // downstream geometry constant across X1/X2.
    const uint32_t actual = g_hw_bb.sel.stream_vld_num * 2U;
    static_assert(2U * kStreamVldNum == kOutFrameBytes, "SEL derivation == geometry");
    const uint32_t configured = g_hw_bb.wrape.configured_frame_bytes;

    // Video Zoom (7.4.1) geometry consistency: the zoom step and the frame
    // actually reaching the WRAPE must agree — a step applied while the frame
    // geometry expects the other coefficient is the coordinate-desync fault
    // class (U6 analog). X1 input needs the 2x step to reach the 1280x1024
    // output; X2 input is identity pass-through. The frame byte length seen
    // here IS the post-zoom output geometry, so both steps are legal only if
    // the output stays kOutFrameBytes — the Identity-Zoom contract. Detecting
    // a mismatch is the point; in the fixed scenario it never fires.
    if (actual != kOutFrameBytes
            || (g_hw_bb.zoom.step != kZoomStep2x
                    && g_hw_bb.zoom.step != kZoomStepIdentity)) {
        ++ctx.zoom_geometry_mismatch;
    }

    const FrameVerdict v =
        g_hw_bb.wrape.stale_x1_framing
            ? StaleFraming::frame(actual, configured)
            : AlignedFraming::frame(actual, configured);

    if (v.err_eof) {
        ++ctx.err_eof_frames;
        std::printf("[wrape] frame %u: actual %u B > configured %u B -> "
                    "ERR+EOF at %u B (%u%% of frame)\n",
                    e.meta.frame_id, actual, configured, v.payload_bytes,
                    v.payload_bytes * 100U / actual);
    } else {
        ++ctx.complete_frames;
    }

    // Hand the framed payload to the non-AO USB DMA engine; it ships the
    // bulk transactions and posts the EOF back (completion-IRQ pattern).
    // A busy engine drops the frame — the check is the "rejected submission
    // must not silently pass" contract (WARN, asserted-0 counter analog).
    if (ctx.dma_engine != nullptr) {
        if (!ctx.dma_engine->submit(
                UsbDmaWorker::Job{true, e.meta.frame_id, v.payload_bytes,
                                  v.err_eof})) {
            std::printf("[warn] usb_dma submit rejected (busy), frame=%u\n",
                        static_cast<unsigned>(e.meta.frame_id));
        }
    }
    ++ctx.frames_framed;
}

COACT_HSM_STATES(kWrapeStates, WrapeCtx, "Root", "Active");
COACT_HSM_TRANS(kWrapeTransitions, WrapeCtx, Sig::kPicPacked, onPicPackedWrape);

// ---------------------------------------------------------------------------
// WindowsHostAo: the four-point observer (stream config / valid count / USB
// payload / host image) the T37 doc demands — frame completeness + count
// continuity are the end-to-end process checks.
// ---------------------------------------------------------------------------
struct WinHostCtx {
    uint32_t frames_received{0U};
    uint32_t complete_frames{0U};
    uint32_t truncated_frames{0U};
    uint32_t min_payload{0xFFFFFFFFU};
    uint32_t max_payload{0U};
    uint32_t frame_gaps{0U};
    uint32_t last_frame_id{0xFFFFFFFFU};
    uint32_t error_interrupts{0U};   // driver-side USB error interrupts (T37 §2.2)
};

inline void onFrameEofHost(WinHostCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    const uint32_t payload =
        static_cast<uint32_t>(e.meta.result);   // EOF carries payload bytes
    ++ctx.frames_received;
    if (payload < ctx.min_payload) { ctx.min_payload = payload; }
    if (payload > ctx.max_payload) { ctx.max_payload = payload; }
    if (0U != (e.meta.flags & 0x1U)) { ++ctx.truncated_frames; }
    else { ++ctx.complete_frames; }
    if (ctx.last_frame_id != 0xFFFFFFFFU
        && e.meta.frame_id != ctx.last_frame_id + 1U) {
        ++ctx.frame_gaps;
    }
    ctx.last_frame_id = e.meta.frame_id;
}

// USB error interrupt (T37 §2.2 fault chain "error interrupt -> DRV"): a
// driver-side line DISTINCT from the EOF's err_eof data attribute. The host
// observer counts it; the USB controller driver-side retry/recovery policy
// is out of the demo's scope. Injection-only; asserted 0 in normal runs.
inline void onUsbErrIntHost(WinHostCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    ++ctx.error_interrupts;
    std::printf("[warn] usb error interrupt: frame=%u (driver counts, "
                "EOF data path unaffected)\n",
                static_cast<unsigned>(e.meta.frame_id));
}

COACT_HSM_STATES(kWinHostStates, WinHostCtx, "Root", "Active");
// One merged table: the EOF data path + the driver-side error-interrupt line
// (kUsbErrInt is an interrupt signal, not an EOF data attribute — see
// onUsbErrIntHost). Same single-state discipline as the orchestrator's table.
inline const TransitionDef<WinHostCtx> kWinHostTransitions[] = {
    { 1, static_cast<uint16_t>(Sig::kFrameEof), 1,
      TransitionKind::Internal, nullptr, onFrameEofHost },
    { 1, static_cast<uint16_t>(Sig::kUsbErrInt), 1,
      TransitionKind::Internal, nullptr, onUsbErrIntHost },
};

// ---------------------------------------------------------------------------
// OrchestratorAO: collects kIrscReady / kIspReady / kVideoReady acks. In
// RS500 this role is split between execute_config_phases_ex() and the
// rte_call_sync response — modeled here as a single in-process AO.
// ---------------------------------------------------------------------------
struct OrchCtx {
    uint32_t irsc_ready{0U};
    uint32_t isp_ready{0U};
    uint32_t pic_video_ready{0U};
    uint32_t temp_video_ready{0U};
    uint32_t last_irsc_step{0xFFU};
};

inline void onIrscReadyAck(OrchCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    ctx.last_irsc_step = e.meta.cmd_arg;
    ++ctx.irsc_ready;
}
inline void onIspReadyAck(OrchCtx& ctx, const Event&)
{
    ++ctx.isp_ready;
}
inline void onVideoReadyAck(OrchCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    // payload_kind carries the VideoStreamKind: 0=PIC, 1=TEMP.
    ++(e.meta.payload_kind ? ctx.temp_video_ready : ctx.pic_video_ready);
}

COACT_HSM_STATES(kOrchStates, OrchCtx, "Root", "Active");
inline const TransitionDef<OrchCtx> kOrchTransitions[] = {
    { 1, static_cast<uint16_t>(Sig::kIrscReady),  1, TransitionKind::Internal, nullptr, onIrscReadyAck },
    { 1, static_cast<uint16_t>(Sig::kIspReady),   1, TransitionKind::Internal, nullptr, onIspReadyAck },
    { 1, static_cast<uint16_t>(Sig::kVideoReady), 1, TransitionKind::Internal, nullptr, onVideoReadyAck },
};


// AO aliases: sinks (USB/MIPI), T37 WRAPE, Windows host, boot orchestrator.
using UsbSinkAo   = coact::Ao<SinkCtx, Hsm<SinkCtx>, AoTrait<40>>;
using MipiSinkAo  = coact::Ao<SinkCtx, Hsm<SinkCtx>, AoTrait<39>>;
using WrapeAo        = coact::Ao<WrapeCtx, Hsm<WrapeCtx>, AoTrait<38>>;
using WinHostAo      = coact::Ao<WinHostCtx, Hsm<WinHostCtx>, LowAoTrait>;
using OrchestratorAo = coact::Ao<OrchCtx, Hsm<OrchCtx>, HighAoTrait>;


}  // namespace isp_demo

