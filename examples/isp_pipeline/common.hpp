/*
 * ===========================================================================
 * common.hpp —— 例子系统全局词汇与共享基础设施（12 文件结构的地基）
 * ===========================================================================
 * 功能描述
 *   - 事件词汇 Sig（39 个统一信号 id，全部跨模块事件在这张表里"词典化"）
 *   - PAL 别名 DemoPal（host 默认 Posix / ISP_DEMO_USE_RTT 时 RtThread /
 *     ISP_DEMO_CORO 时 CoroPal 单 pthread executor 共用）与全局单例 g_pal
 *     （worker/sleep_us/monotonic_ns 全走它，无裸 pthread）
 *   - DDR 数据面 DdrCtx（7 区域 x 8 槽三缓冲 + FrameStamp + 槽所有权协议）
 *   - 会话门控平面：g_session 原子相位 + SessionEventComposite 广播
 *   - coact::diag 日志通道 g_log、模式表 kProductModes、AO trait 优先级布局
 *   对应 RS500 module/common 与 module/rte 的共享层（app_fill_preview_cmd_by_
 *   mode 的模式表、rte_register_service 的优先级分区均在此镜像）。
 *
 * 与其他文件的关系
 *   - 被 11 个文件 include（sensor_irsc/isp_chain/video_stream/output_itf/
 *     recfg_session 五对 .hpp/.cpp + main.cpp）；本文件不 include 任何例子
 *     业务文件，只 include coact 框架与 PAL 头（ao/hsm/pool/runtime/event）。
 *   - 上游：main.cpp 构造 g_pal / g_session_pool / g_session_rt 并武装会话平面。
 *   - 下游：各模块 AO 的弧 action 读 DdrCtx 写读像素字节；各 worker 线程经
 *     g_pal->thread_create/sleep_us 运行；子 AO guard 直接读原子 g_session。
 *   - 依赖：DemoPal（SemOps/MutexOps/CondOps/ThreadOps/sleep_us/monotonic_ns）
 *     的全部操作面；DdrCtx 的 claim/read/write 是唯一数据面入口。
 *
 * 文字图（全局视角）
 * ┌──────────────────────────────────────────────────────────────────┐
 * │ 例子系统数据流（→事件信号  ⇒DDR/黑板数据）                        │
 * │                                                                  │
 * │  [sensor_irsc]──kFrameIrscOut──▶[isp_chain]──kHlFused/Done──▶   │
 * │  (本文件)                        [video_stream]──kPicPacked──▶  │
 * │                                  [output_itf]──kFrameEof──▶     │
 * │                                  [winhost 观察者]                │
 * │                                                                  │
 * │  编排: [main.cpp] ──kIrscCmd/kVideoCmd──▶ 各模块 AO             │
 * │  重配: [recfg_session] ◀──kRecfgReq── main；门控 g_session      │
 * │  共享: [common.hpp] 词汇+DdrCtx+黑板 / PAL: g_pal (SemOps 等)    │
 * │                （本文件 = 全图的地基：词汇/数据面/PAL/会话）      │
 * └──────────────────────────────────────────────────────────────────┘
 * ===========================================================================
 */
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <pthread.h>
#include <signal.h>

// PAL alias switch (see the DemoPal alias below): pull the RT-Thread PAL
// header BEFORE the coact framework headers (pal_rtthread.hpp needs the
// stub/rtthread.h include order of its own).
#ifdef ISP_DEMO_USE_RTT
#include "coact/pal_rtthread.hpp"
#else
#include "coact/pal_posix.hpp"
#endif

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/diag/log_rtthread.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/pool.hpp"
#include "coact/runtime.hpp"

#include "isp_pipeline/coro_pal.hpp"

namespace isp_demo {
using coact::Event;
using coact::EventQos;
using coact::Hsm;
using coact::LogicalPrio;
using coact::PriorityClass;
using coact::StateDef;
using coact::TargetId;
using coact::TransitionDef;
using coact::TransitionKind;

// ---------------------------------------------------------------------------
// Platform alias layer. The demo's PAL type is decided HERE, at compile time
// (ISP_DEMO_USE_RTT): the default host build binds coact::pal::Posix; an
// RT-Thread build (or the host RTT-stub compile gate) binds coact::pal::RtThread.
// Every worker / thread / sleep / clock dependency below flows through this
// alias, so no business file contains a bare pthread or usleep.
// ---------------------------------------------------------------------------
#ifdef ISP_DEMO_USE_RTT
using DemoPal = coact::pal::RtThread;
#elif defined(ISP_DEMO_CORO)
// Coro mode: the coro PAL shim routes thread_create/sleep_us into the
// coact::coro executor (one pthread for all workers - MCU-fair single-core).
using DemoPal = isp_demo_coro::CoroPal;
#else
using DemoPal = coact::pal::Posix;
#endif

// The single PAL instance main() constructs; late-bound (workers and helpers
// reach sleep_us / monotonic_ns through it without carrying a reference).
inline DemoPal* g_pal{nullptr};

// ---------------------------------------------------------------------------
// SoftIrq completion gate (SINGLE point, task: USB DMA completion -> soft
// IRQ -> consumer pthread -> kFrameEof). Non-RTT builds route the UsbDmaWorker
// completion through the SoftIrqOps family; the RT-Thread build keeps the
// direct submit (board-level rt_signal semantics unverified). Workers and
// consumers branch on this constant with if constexpr — no scattered #ifdefs.
// ---------------------------------------------------------------------------
#ifdef ISP_DEMO_USE_RTT
inline constexpr bool kUseSoftIrqCompletion = false;
inline void softirq_block_completion_signal() noexcept {}
#else
inline constexpr bool kUseSoftIrqCompletion = true;
// Process-wide prerequisite for the SoftIrq path: block the completion signal
// in the CALLING thread BEFORE any other thread is created. Every pthread
// spawned later (pump / dispatcher / workers / log writer / softirq consumer)
// inherits the blocked mask, so a raise can never be delivered to an unblocked
// thread with its default (terminate) disposition — the queued signal can only
// surface through the consumer's signalfd. Must be the first thread-related
// act of main(). No-op under the RTT build (path disabled).
inline void softirq_block_completion_signal() noexcept
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, coact::pal::Posix::SoftIrqSignal);
    (void)pthread_sigmask(SIG_BLOCK, &mask, nullptr);
}
#endif

// ---------------------------------------------------------------------------
// coact::diag log channel. Uses the RT-Thread static log adapter with the
// host stub (COACT_RTT_STUB) so it renders through rt_kprintf on stdout.
// Producers call g_log.record_from_task<Level, kEvt*>(args...) from AO
// actions; a static writer thread drains both lanes and emits the formatted
// line asynchronously (no producer-side formatting or allocation).
// ---------------------------------------------------------------------------
using LogLevel = coact::diag::LogLevel;
inline coact::diag::LogRtThread<> g_log;

enum LogEvt : uint16_t {
    kEvtBoot         = 1U,
    kEvtPhase1       = 2U,
    kEvtPhase3       = 3U,
    kEvtPhase4       = 4U,
    kEvtIrscStep     = 5U,
    kEvtIspNode      = 6U,
    kEvtVideoCmd     = 7U,
    kEvtFrameEmitted = 8U,
    kEvtFrameDone    = 9U,
    kEvtSinkFirst    = 10U,
    kEvtStop         = 11U,
    // Runtime-migration events (args are raw uint32_t; the writer renders
    // raw-hex "[cnt] e=... a0..a3"). String context (AO/state names) is not
    // carried by the record; the printf/hsm trace remains the readable view.
    kEvtHsmTrace    = 12U,  // a0=ao_id a1=src a2=sig a3=dst (0x100|idx = reject)
    kEvtRecfg       = 13U,  // a0=stage a1..a3 stage-specific
    kEvtWorkerStat  = 14U,  // a0=worker_id a1=executed a2=rejected a3=extra
    kEvtSession     = 15U,  // a0=prev a1=next (SessionState raw values)
    kEvtVideoFsm    = 16U,  // a0=stream a1=cmd a2=fsm
    // Framework trace reserve block (design_coact_trace_zh §3.1): event ids
    // 0x0100+ keep core TraceOps records out of the 1..16 business catalog.
    kEvtTraceSubmit   = 0x0100U,  // a0=dst a1=signal a2=disposition a3=reason
    kEvtTraceDispatch = 0x0101U,  // a0=elapsed_lo a1=elapsed_hi a2=path a3=timeout
    kEvtTraceLease    = 0x0102U,  // a0=kind a1=elapsed_lo a2=elapsed_hi a3=0
};

// ---------------------------------------------------------------------------
// Mode table (mirror of RS500 app_fill_preview_cmd_by_mode).
// ---------------------------------------------------------------------------
enum class SoutMode : uint8_t { kNoSout = 0U, kYuv420spNv12 = 2U };
enum class StreamMode : uint8_t { k3Loop = 0U, kDmo = 1U };
enum class OutFmt : uint8_t { kYuv422 = 0U, kY16 = 5U };
enum class Itf : uint8_t { kUsbS0 = 13U, kMipiTx0CsiS0 = 3U };
enum class ArchStream : uint8_t { kPicTemp = 0U, kPicOnly = 1U };

struct PreviewCmdInfo {
    ArchStream arch{ArchStream::kPicTemp};
    Itf        itf{Itf::kUsbS0};
    uint16_t   width{640U};
    uint16_t   height{512U};
    uint8_t    fps{30U};
    SoutMode   sout{SoutMode::kNoSout};
    StreamMode mode{StreamMode::kDmo};
    OutFmt     fmt{OutFmt::kYuv422};
    bool       stream_out_direct{true};
};

// Mirror of app_fill_preview_cmd_by_mode's product branch (non-RCALI).
// Mode 0 -> USB S0 640x514@30 PIC_TEMP, no SOUT, direct output.
// Mode 1 -> MIPI 640x514@30 PIC_TEMP, NV12 SOUT, direct output.
// Mode 4 -> MIPI 1920x1442@30 PIC_TEMP, NV12 SOUT, direct output.
// Mode 5 -> USB  1920x1442@3  PIC_TEMP, NV12 SOUT, direct output (USB2 low-fps).
inline constexpr PreviewCmdInfo kProductModes[] = {
    { ArchStream::kPicTemp, Itf::kUsbS0,          640U,  514U,  30U, SoutMode::kNoSout,       StreamMode::kDmo, OutFmt::kYuv422, true  }, // 0
    { ArchStream::kPicTemp, Itf::kMipiTx0CsiS0,   640U,  514U,  30U, SoutMode::kYuv420spNv12, StreamMode::kDmo, OutFmt::kYuv422, true  }, // 1
    { ArchStream::kPicTemp, Itf::kUsbS0,          640U,  514U,  30U, SoutMode::kNoSout,       StreamMode::kDmo, OutFmt::kYuv422, false }, // 2
    { ArchStream::kPicTemp, Itf::kMipiTx0CsiS0,   640U,  514U,  30U, SoutMode::kYuv420spNv12, StreamMode::kDmo, OutFmt::kYuv422, false }, // 3
    { ArchStream::kPicTemp, Itf::kMipiTx0CsiS0,   1920U, 1442U, 30U, SoutMode::kYuv420spNv12, StreamMode::kDmo, OutFmt::kYuv422, true  }, // 4
    { ArchStream::kPicTemp, Itf::kUsbS0,          1920U, 1442U, 3U,  SoutMode::kYuv420spNv12, StreamMode::kDmo, OutFmt::kYuv422, true  }, // 5
};

// Mirror of RS500_CALI branch (all PIC_ONLY Y16 3LOOP).
inline constexpr PreviewCmdInfo kCaliModes[] = {
    { ArchStream::kPicOnly, Itf::kUsbS0,          640U,  512U,  30U, SoutMode::kNoSout, StreamMode::k3Loop, OutFmt::kY16, true }, // 0
    { ArchStream::kPicOnly, Itf::kMipiTx0CsiS0,   1920U, 1440U, 30U, SoutMode::kNoSout, StreamMode::k3Loop, OutFmt::kY16, true }, // 1
    { ArchStream::kPicOnly, Itf::kUsbS0,          1920U, 1440U, 5U,  SoutMode::kNoSout, StreamMode::k3Loop, OutFmt::kY16, true }, // 2
};

inline const PreviewCmdInfo& select_cmd(bool cali, uint8_t mode_sel)
{
    const auto& tbl = cali ? kCaliModes : kProductModes;
    const uint8_t n = cali ? static_cast<uint8_t>(std::size(kCaliModes))
                           : static_cast<uint8_t>(std::size(kProductModes));
    return (mode_sel < n) ? tbl[mode_sel] : tbl[n - 1U];
}

// ---------------------------------------------------------------------------
// Single-authority frame geometry (the fix for the "X2 premature EOF" class
// of drift: one formula, every layer reads the same value).
//
// In the failure this models, AI already produced X2 geometry while SOUT
// still framed one X1-sized frame (655360B = 1/4 of a full X2 frame), so the
// sink observed a truncated transfer. The rule: NO layer re-derives geometry
// from raw config — they all ask the authority.
// ---------------------------------------------------------------------------
struct FrameGeometry {
    uint16_t width{0U};
    uint16_t height{0U};
    uint16_t bytes_per_pixel{2U};

    [[nodiscard]] constexpr uint32_t bytes_per_frame() const noexcept
    {
        return static_cast<uint32_t>(width) * height * bytes_per_pixel;
    }
    [[nodiscard]] constexpr bool operator==(const FrameGeometry& o) const noexcept
    {
        return width == o.width && height == o.height
            && bytes_per_pixel == o.bytes_per_pixel;
    }
};

// AI super-resolution modes. kX4 EXISTS in the request vocabulary but the
// capability matrix rejects it at every layer — modeling the doc's finding
// that a dirty-NVM X4 passes sanitize but fails runtime reconfig.
enum class SrMagx : uint8_t { kOff = 0U, kX1 = 1U, kX2 = 2U, kX4 = 4U };

[[nodiscard]] constexpr FrameGeometry apply_magx(FrameGeometry in, SrMagx m) noexcept
{
    switch (m) {
        case SrMagx::kX2:
            return { static_cast<uint16_t>(in.width * 2U),
                     static_cast<uint16_t>(in.height * 2U), in.bytes_per_pixel };
        case SrMagx::kX1:
        case SrMagx::kX4:      // geometry math exists; the MATRIX still rejects it
        case SrMagx::kOff:
        default:
            return in;
    }
}

// The capability matrix: the single authority for which magx values may run.
[[nodiscard]] constexpr bool magx_supported(SrMagx m) noexcept
{
    return (SrMagx::kOff == m) || (SrMagx::kX1 == m) || (SrMagx::kX2 == m);
}

// ---------------------------------------------------------------------------
// Runtime reconfiguration: stage enum replaces three booleans (the doc's §5.2
// "阶段枚举替代布尔组合"). Every stage is enumerable; recovery is a switch.
// ---------------------------------------------------------------------------
enum class RecfgStage : uint8_t {
    kIdle = 0U,       // no reconfig in flight
    kPrecheck,        // read-only validation, zero hardware touch
    kQuiescing,       // SOUT_STOP_FRAME issued, waiting for frame-boundary IDLE
    kApplying,        // dependency-ordered register writes (AI -> DMA -> SOUT)
    kSyncing,         // FS_SYNC
    kResuming,        // SOUT_START + first-frame confirmation
    // Terminal outcomes (commit/recover) go straight to kIdle: the mirror
    // never stores a "terminal posture" value, so no enum entry for them.
};

// Reconfig context: old/target snapshots + stage. The ONLY input to recovery.
// Commit happens last: std::exchange(active_cfg, target) only after success.
struct RecfgCtx {
    RecfgCtx*   next_pending{nullptr};   // intrusive queue (static, no heap)
    RecfgStage  stage{RecfgStage::kIdle};
    SrMagx      target_magx{SrMagx::kX1};
    FrameGeometry old_geom{};
    FrameGeometry target_geom{};
    uint32_t    layout_version{0U};      // bump on geometry change; stale
                                         // address records die on first query
    bool        inject_stale_sout{false}; // fault injection: SOUT frames with
                                          // the OLD geometry after apply
};

// ---------------------------------------------------------------------------
// Tunables (host-side frame period & per-node latency).
// ---------------------------------------------------------------------------
constexpr uint32_t kFrameCount    = 30U;
constexpr uint8_t  kSubscriberCount = 2U;

struct NodeLatency {
    uint32_t irsc_setup_us;
    uint32_t low_gain_us;
    uint32_t high_gain_us;
    uint32_t hl_fuse_us;
    uint32_t enhance_us;
    uint32_t tpd_us;
    uint32_t pic_video_us;
    uint32_t temp_video_us;
};
inline constexpr NodeLatency kLat{300, 400, 600, 200, 300, 500, 400, 300};

// Async-channel latencies (modeled by the non-AO workers; see WorkerBase).
constexpr uint32_t kIrscCmdLatencyUs = 500U;   // vdcmd rt_device_control
                                             // register-write path per step
constexpr uint32_t kMipiTxLatencyUs  = 600U;   // MIPI CSI TX line-buffer transfer

// ---------------------------------------------------------------------------
// Event vocabulary (single unified layout).
// ---------------------------------------------------------------------------
enum class Sig : uint16_t {
    kBoot         = 1U,    // orchestrator -> boot path (auto_preview_start)
    kInitPreview  = 2U,    // App -> StreamConfigService (RTE-style)
    kStopPreview  = 3U,    // App -> StreamConfigService (deinit reverse)
    kPhaseEnter   = 4U,    // orchestrator -> driver AOs (Phase enter marker)
    kIrscCmd      = 5U,    // App -> IrscDriver (init/start/ctrl/output_enable)
    kIspCmd       = 6U,    // App -> IspPipelineAO (init)
    kVideoCmd     = 7U,    // App -> VideoStreamFSM (init/start/stream_enable)
    kIrscReady    = 8U,    // IrscDriver -> orchestrator
    kIspReady     = 9U,    // IspPipelineAO -> orchestrator
    kVideoReady   = 10U,   // VideoStreamFSM -> orchestrator
    kFrameIrscOut = 11U,   // IRSC producer -> gain chains
    kLowGainDone  = 12U,
    kHighGainDone = 13U,
    kHlFused      = 14U,
    kEnhanceDone  = 15U,
    kTempChainDone = 16U,
    kPicPacked    = 17U,
    kTempPacked   = 18U,
    kRecfgReq     = 19U,   // App -> RecfgOrchestrator (SR X1->X2 request)
    kRecfgStage   = 20U,   // RecfgOrchestrator stage advance (self-driving)
    kSoutIdle     = 21U,   // SOUT frame-boundary idle ack
    kFirstFrame   = 22U,   // first frame ready after resume (commit boundary)
    kRecfgDone    = 23U,   // RecfgOrchestrator -> orchestrator (result)
    kFmtIdle      = 24U,   // Format888 idle ack (event-driven fix path)
    kFrameEof     = 25U,   // WRAPE -> Windows host: frame end (good or ERR+EOF)
    kIrscDmaDone  = 26U,   // CmdDmaWorker -> IrscDriver: register write done
    kIspNodeDone  = 27U,   // IspIrqWorker -> enhance/TPD AO: node-done irq
    kSoutDone     = 28U,   // SoutDmaWorker -> VideoPack AO: writeback-done irq
    kMipiTxDone   = 29U,   // MipiIrqWorker -> MIPI sink AO: TX-done irq
    kSessionState = 30U,   // session composite broadcast: phase gating
    // Per-path duplicates of the fan-out signals (the double-instance fix):
    // HlFuseAO emits ONE fused frame to BOTH downstream instances, so the
    // single shared kEnhanceDone/kTempChainDone ids collide in the merged
    // product HSMs — the pack pair-state table cannot tell a PIC input from a
    // TEMP input with one id. The distinct ids are the same discipline the
    // event tags ('HLP' vs 'HLT') already use; signal ids are part of the
    // transition-table contract, not just a wire label.
    kEnhanceDonePic  = 31U,   // EnhanceAO -> VideoPack (PIC input)
    kEnhanceDoneTemp = 32U,   // TempChainAO -> VideoPack (TEMP input)
    kSoutDonePic     = 33U,   // pack AO: PIC writeback completion (re-stamped)
    kSoutDoneTemp    = 34U,   // pack AO: TEMP writeback completion (re-stamped)
    // Error/interrupt signals from the RS500 manual's three chains. Normal
    // runs observe ZERO of these (asserted); they exist so the receiving AO
    // counts the error and degrades (drops the frame), never crashes and
    // never auto-changes a Stream FSM (the "interrupt does not move the FSM"
    // constraint of RS500_MIPI图像输出原理.md §9).
    kIspFifoOvf    = 35U,   // IspIrqWorker -> node AO: ISP stream FIFO_OVERFLOW
                            // interrupt (RS500视频流处理与输出架构.md §4.3;
                            // DMA_WR_DONE is covered by kIspNodeDone, ERR is
                            // covered by kFrameEof's err_eof flag)
    kMipiStreamErr = 36U,   // MipiIrqWorker -> MIPI sink AO: CSI stream error
                            // interrupt (RS500_MIPI图像输出原理.md §9); the AO
                            // counts it, no FSM transition (RS500 rule)
    kUsbErrInt     = 37U,   // UsbDmaWorker -> WinHostAO/driver: USB error
                            // interrupt as its OWN signal path (T37 §2.2 fault
                            // chain "error interrupt -> DRV"); the frame-EOF
                            // err_eof flag stays a DATA attribute, this is the
                            // driver-side interrupt line
    kStreamDone    = 38U,   // passive-mode SOUT STREAM_DONE interrupt
                            // (hal_stream_out.h; DMO quiesce step 2 of the
                            // two-step fix, referenced by DmoQuiesce below)
    kPassiveDataLoss = 39U, // passive-mode PASSIVE_DATA_LOSS interrupt
                            // (hal_stream_out.h); the DMO quiesce fallback of
                            // the second step — referenced by DmoQuiesce below
};

struct IoMeta {
    TargetId reply_to{};
    uint32_t frame_id{0U};
    uint16_t width{640U};
    uint16_t height{512U};
    uint16_t cmd_arg{0U};     // sub-cmd id (e.g. irsc_cmd sub-step)
    uint16_t flags{0U};        // bit0 high_gain, bit1 temp path, bit2 3loop
    uint32_t timestamp_ns{0U};
    uint32_t payload_kind{0U}; // 0=DN, 1=PIC yuv, 2=TEMP y16, 3=meta
    uint32_t buffer_idx{0U};
    int32_t  result{0};
};

// Payload surrogate for control-plane events. The DATA plane (actual pixel
// buffers in DDR) is NOT carried in events — RS500's video_isp_stream_rx_ind
// explicitly does not move pixels over the event channel. Instead the payload
// carries only the buffer descriptor (slot id + stage) so the DdrOwnerAO and
// downstream nodes know WHICH DDR region a frame lives in.
struct Payload {
    std::array<uint8_t, 32> control{};   // e.g. route tag
    uint16_t ddr_slot{0xFFU};            // which triple-buffer slot
    uint16_t ddr_id{0xFFU};              // which DDR region (0=DN,1=LG,2=HG,
                                         // 3=fused,4=PIC out,5=TEMP out)
};

constexpr size_t kPayloadAlign = 64U;
using Layout = coact::EventBlockLayout<IoMeta, sizeof(Payload), kPayloadAlign>;

// Platform profile (design_isp_pipeline_optimization P1): the RT-Thread build
// uses the single-core profile end to end - plain irq-mask pool backend,
// ImmediateReclaimer, SingleCoreCriticalRing staging - while the host/coro
// builds keep HostSmpProfile (tagged-CAS pool, batched reclaim). PoolT, Rt
// and the Dispatcher must never mix profiles.
#ifdef ISP_DEMO_USE_RTT
using DemoProfile = coact::RttSingleCoreProfile;
#else
using DemoProfile = coact::HostSmpProfile;
#endif

using PoolT = coact::EventPool<static_cast<uint16_t>(sizeof(Layout)),
                               128U, DemoProfile, kPayloadAlign>;
using Rt    = coact::Runtime<coact::DefaultConfig, DemoPal, DemoProfile>;

// ---------------------------------------------------------------------------
// DDR image region. Mirrors isp_stream_dma (ISP domain) / stream_out_dma
// (Video/SOUT domain): a fixed set of DMA triple-buffers in DDR. We model a
// small 8x8 Y16-ish frame (128 bytes) so the data plane actually holds bytes,
// and per-node transforms are verifiable downstream.
// ---------------------------------------------------------------------------
constexpr uint16_t kFramePixels = 8U * 8U;         // 64 pixels
constexpr uint16_t kDdrFrameBytes = kFramePixels;  // Y16 proxy = 1 byte/px here
constexpr uint16_t kTripleBufSize = 3U;   // RS500's real DMA ring depth
constexpr uint16_t kDdrSlots = 8U;        // demo depth: > max in-flight span

// Slot ownership state — the protocol-level mirror of RS500's DMA descriptor
// ownership bit (own by DMA / own by CPU in the 3-frame circular descriptor
// ring: whoever owns the slot may touch it; violating the ownership raises a
// FIFO_OVERFLOW / PASSIVE_DATA_LOSS interrupt). Here the "writer" is the
// producing node's DdrCtx::write (the DMA's counterpart) and the "reader" is
// a downstream node's DdrCtx::read (the CPU's counterpart).
//   kWriterOwned   : free for the writer — a fresh write may take it.
//   kReaderClaimed : a reader is inside the claim window (between claim and
//                    release); the writer MUST NOT overwrite — it skips the
//                    rotation and counts an ownership skip (the honest-drop
//                    counterpart of FIFO_OVERFLOW: the writer never blocks,
//                    DMA does not wait for anyone).
//   kFree          : slot unused or already consumed — writable.
enum class SlotOwner : uint8_t {
    kFree         = 0U,
    kWriterOwned  = 1U,
    kReaderClaimed= 2U,
};

// A fixed DDR region: N triple-buffered frames, byte-addressable via slot.
// Each slot carries the frame_id it currently holds so a late reader can
// detect overrun (the slot was recycled by a newer frame) — the same
// discipline a real 3-frame DMA ring needs when the consumer falls behind.
// The per-slot `owner` array is the SIDE-BAND ownership tracker (kept out of
// the raw slot bytes, like the descriptor-table ownership bits live in
// descriptor RAM, not in the frame payload).
struct DdrRegion {
    std::array<std::array<uint8_t, kDdrFrameBytes>, kDdrSlots> slots{};
    std::array<uint32_t, kDdrSlots> slot_frame{kDdrSlots, 0xFFFFFFFFU};
    // Value-initialized: every owner starts kFree (kFree == 0).
    std::array<SlotOwner, kDdrSlots> owner{};
    std::atomic<uint16_t> write_idx{0U};   // claimed-slot count (stats only)

    uint16_t claim() { return write_idx.fetch_add(1U, std::memory_order_acq_rel) % kTripleBufSize; }
    uint16_t last() const
    {
        const uint16_t w = write_idx.load(std::memory_order_acquire);
        return (w == 0U) ? static_cast<uint16_t>(kTripleBufSize - 1U)
                         : static_cast<uint16_t>(w - 1U);
    }
    const uint8_t* read(uint16_t slot) const { return slots[slot].data(); }
    uint8_t* write(uint16_t slot) { return slots[slot].data(); }
};

enum DdrId : uint16_t {
    kDdrDn      = 0U,   // IRSC DN raw (ISP input)
    kDdrLowGain = 1U,   // low-gain chain output (pre-HL)
    kDdrHighGain= 2U,   // high-gain chain output (pre-HL)
    kDdrFused   = 3U,   // HL fused (post-HL, PIC path)
    kDdrTemp    = 4U,   // TPD chain output (TEMP path)
    kDdrPicOut  = 5U,   // PIC stream_out (post Video)
    kDdrTempOut = 6U,   // TEMP stream_out (post Video)
    kDdrCount   = 7U,
};

static_assert(kDdrCount <= 7U, "DdrId range fits Payload::ddr_id");

// Frame stamp: a DMA-descriptor-style header constructed IN PLACE at the head
// of each DDR slot via placement new (no temporary, no copy — the object
// begins its lifetime directly in the slot's memory, exactly like the event
// pool's ::new (block) Layout{}).
struct FrameStamp {
    uint32_t frame_id;
    uint16_t region;
    uint16_t seq;      // write sequence within the slot
};

constexpr uint16_t kStampBytes = sizeof(FrameStamp);
static_assert(kDdrFrameBytes > kStampBytes,
              "each DDR slot must fit a stamp header plus payload bytes");
constexpr uint16_t kSlotPayloadBytes =
    static_cast<uint16_t>(kDdrFrameBytes - kStampBytes);

// ---------------------------------------------------------------------------
// Compile-time ABI / layout / move-behavior contracts ("编译期确定" principle).
// Every struct that crosses a module boundary or lives in pool/DDR memory is
// constrained HERE, at its definition — violations fail the build, not the
// field.
// ---------------------------------------------------------------------------
static_assert(std::is_standard_layout<FrameStamp>::value,
              "FrameStamp is written into raw DDR slot memory: it must be "
              "standard-layout so its address arithmetic is defined");
static_assert(std::is_trivially_copyable<FrameStamp>::value,
              "FrameStamp is memcpy-able DMA-descriptor-like data");
static_assert(std::is_nothrow_move_constructible<FrameStamp>::value,
              "FrameStamp moves must never throw (no-exceptions build)");
static_assert(std::is_standard_layout<FrameGeometry>::value,
              "FrameGeometry crosses the AO boundary in event payloads");
static_assert(std::is_trivially_copyable<FrameGeometry>::value,
              "FrameGeometry is snapshot/exchange'd in reconfig transactions");
static_assert(std::is_nothrow_move_constructible<FrameGeometry>::value,
              "FrameGeometry moves must never throw");
static_assert(std::atomic<uint16_t>::is_always_lock_free,
              "DdrRegion::write_idx must be lock-free (dispatcher + producer "
              "threads touch it; a libatomic fallback would be a silent "
              "heap/lock dependency)");
static_assert(std::atomic<bool>::is_always_lock_free,
              "IrscWorker::running is cross-thread: lock-free required");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "DdrCtx stat counters are cross-thread (Dispatcher + ISP IRQ "
              "worker): lock-free required");

// --- cross-boundary payload contracts (merged-AO era) ---------------------
static_assert(std::is_standard_layout<IoMeta>::value,
              "IoMeta is parked in ctx slots and moved out via exchange");
static_assert(std::is_trivially_copyable<IoMeta>::value,
              "IoMeta must be memcpy-able (DMA-descriptor-like)");
static_assert(std::is_trivially_copyable<uint16_t>::value,
              "WorkerBase job payload (uint16_t) is placement-new'd into the "
              "ring slot: trivially copyable required for the launder read");

// ---------------------------------------------------------------------------
// Blackboard B: frame-data domain (DdrCtx, the single home of every image
// region). Five-element contract (the hardware blackboard's four elements plus
// the ownership protocol):
//   W (writer): processing-node AO actions (fill_dn/enhance/pack write their
//       region's current slot) + the ISP IRQ worker for the DN region — each
//       region has ONE producing node, the owner-AO discipline.
//   R (reader): the next node in each chain (via region read), the sinks
//       (WRAPE byte-verify, MIPI verify), and the final checks (counters).
//   S (sync):   no lock on the region data — the coact Dispatcher is
//       single-threaded, so all node actions run serially; the ONLY
//       cross-thread fields are write_idx (lock-free atomic) and the stat
//       counters (lock-free atomics, static_assert'ed above) touched by the
//       Dispatcher + the ISP IRQ worker. (RS500's isp_stream_dma /
//       stream_out_dma are likewise owned by the ISP/Video driver domains,
//       not shared via events.)
//   I (invalidation): a slot is invalidated by RECYCLING — a newer frame
//       overwrites the deterministic slot and stamps its own frame_id; a
//       late reader detects the stale slot via the frame-id guard, which
//       turns any deeper backlog into an honest overrun drop.
//   O (ownership): each slot carries an explicit SlotOwner state (side-band,
//       like the descriptor-table ownership bits in RS500's 3-frame circular
//       DMA ring: own by DMA / own by CPU). The reader CLAIMS the slot for
//       the narrow window of its payload copy and RELEASES it after; the
//       writer checks the slot's owner BEFORE writing and never overwrites a
//       kReaderClaimed slot — it skips the rotation and counts
//       ownership_skips (the honest-drop counterpart of the FIFO_OVERFLOW
//       interrupt; the writer never blocks, DMA does not wait for anyone).
//       This upgrades the old timing-only argument ("single-frame lifetime <
//       ring wrap-around, therefore safe") into a protocol guarantee that
//       would still hold if the read path ever went multi-threaded.
// ---------------------------------------------------------------------------
// Deterministic slot: frame N always lands in slot N % kDdrSlots. Every region
// is written at most once per frame, and the in-flight frame span (queue depth)
// stays well below kDdrSlots at the demo's 30fps/3ms-per-chain pacing, so a
// slot is only recycled by a frame kDdrSlots later — the frame-id guard turns
// any deeper backlog into an honest overrun drop, exactly like a real DMA ring.
// The ownership protocol on top of this proves the skip path never fires on
// the demo's pacing: the final check asserts ownership_skips == 0.
[[nodiscard]] inline uint16_t dn_slot_of(uint32_t frame_id)
{
    return static_cast<uint16_t>(frame_id % kDdrSlots);
}

struct DdrCtx {
    DdrRegion regions[kDdrCount]{};
    uint32_t write_ops{0U};
    uint32_t read_ops{0U};
    uint32_t overrun_drops{0U};
    uint32_t ownership_skips{0U};   // writer-side honest drop (FIFO_OVERFLOW
                                    // counterpart): reader-held slot skipped
    uint16_t stamp_seq{0U};

    // Write a frame into its deterministic slot (frame_id % kDdrSlots).
    // The slot header is a FrameStamp constructed via PLACEMENT NEW directly
    // in the slot's memory (DMA-descriptor write); the pixels land behind it.
    // std::exchange hands the previous stamp out so the slot's ownership
    // transfer is explicit.
    //
    // OWNERSHIP PROTOCOL (write side): before touching the slot the writer
    // checks its owner — a kReaderClaimed slot is NOT the writer's to touch
    // (the reader is inside its claim window). The writer then skips the
    // rotation WITHOUT overwriting the bytes under the reader's feet and
    // counts an ownership skip: the honest drop of the RS500 FIFO_OVERFLOW
    // interrupt. The writer never blocks or waits — DMA hardware does not
    // wait for the CPU, and neither does this path. The skipped frame's
    // would-be readers get an honest overrun (slot_frame still names the
    // older frame) instead of silently torn data.
    [[nodiscard]] uint16_t write(DdrId id, uint32_t frame_id, const uint8_t* src)
    {
        DdrRegion& r = regions[id];
        const uint16_t slot = dn_slot_of(frame_id);
        if (SlotOwner::kReaderClaimed == r.owner[slot]) {
            ++ownership_skips;   // reader holds the slot: skip, do not write
            return slot;         // caller stamps buffer_idx; the frame-id
                                 // guard downstream turns this into an
                                 // honest overrun drop
        }
        uint8_t* const slot_mem = r.write(slot);
        // Placement new: begin the FrameStamp lifetime in place.
        FrameStamp* const stamp =
            ::new (static_cast<void*>(slot_mem)) FrameStamp{
                frame_id, static_cast<uint16_t>(id), ++stamp_seq};
        static_cast<void>(std::exchange(r.slot_frame[slot], stamp->frame_id));
        r.owner[slot] = SlotOwner::kWriterOwned;
        std::memcpy(slot_mem + kStampBytes, src, kSlotPayloadBytes);
        ++write_ops;
        return slot;
    }

    // Read a frame only if the slot still holds the expected frame_id (the
    // stamp is the guard). Returns false on overrun (slot recycled by a newer
    // frame) — the honest behavior of a DMA ring when the consumer lags.
    //
    // The stamp was placement-new'd into raw slot memory; std::launder
    // re-establishes the pointer-to-object relationship so reading it is
    // defined behavior (C++17 object model: [ptr.launder]).
    //
    // OWNERSHIP PROTOCOL (read side): after both frame-id guards pass, the
    // reader CLAIMS the slot (kReaderClaimed) for exactly the payload memcpy
    // and RELEASES it (kFree) right after — the narrowest possible window,
    // the software mirror of the CPU taking ownership of a DMA descriptor to
    // drain it. An overrun miss never claims (nothing to protect: the bytes
    // are stale anyway). Under the single-threaded Dispatcher the window is
    // uncontended, but the protocol makes the safety explicit rather than
    // implied by timing: even a future multi-threaded reader could not have
    // its slot overwritten mid-copy.
    [[nodiscard]] bool read(DdrId id, uint16_t slot, uint32_t frame_id,
                            uint8_t* dst)
    {
        DdrRegion& r = regions[id];
        // Double guard: the stamp's frame_id AND the region's slot_frame map
        // must both name the expected frame. Short-circuit order matters:
        // the laundered stamp is only safe to READ once the slot has been
        // written at least once (slot_frame is initialized to 0xFFFFFFFF, so
        // reading it first makes an untouched-slot read safe on BOTH guards).
        if (r.slot_frame[slot] != frame_id) {
            ++overrun_drops;
            return false;   // overrun: caller counts a dropped frame
        }
        const FrameStamp* const stamp = std::launder(
            reinterpret_cast<const FrameStamp*>(r.read(slot)));
        if (stamp->frame_id != frame_id) {
            ++overrun_drops;
            return false;   // stamp/owner-map disagreement: treat as overrun
        }
        r.owner[slot] = SlotOwner::kReaderClaimed;   // claim: narrow window
        std::memcpy(dst, r.read(slot) + kStampBytes, kSlotPayloadBytes);
        r.owner[slot] = SlotOwner::kFree;            // release: back to writer
        ++read_ops;
        return true;
    }
};

// Synthetic DN frame: deterministic per-frame pattern so downstream can verify
// the data really flowed through DDR unchanged (modulo node transforms).
inline void fill_dn(uint8_t* dst, uint32_t frame_id)
{
    for (uint16_t i = 0; i < kSlotPayloadBytes; ++i) {
        dst[i] = static_cast<uint8_t>((frame_id * 7U + i) & 0xFFu);
    }
}

// ---------------------------------------------------------------------------
// Static HSM table macros (compile-time table generation).
//
// Every AO in this demo uses the same trivial Root/Active skeleton; the macros
// stamp out the static tables at compile time with a caller-chosen table name,
// keeping the HSM tables declarative and impossible to misorder.
// ---------------------------------------------------------------------------
#define COACT_HSM_STATES(Table, Ctx, RootLabel, ActiveLabel)      \
    inline const coact::StateDef<Ctx> Table[] = {                 \
        { -1, nullptr, nullptr, RootLabel },                      \
        { 0,  nullptr, nullptr, ActiveLabel },                    \
    }

#define COACT_HSM_TRANS(Table, Ctx, Signal, Action)               \
    inline const coact::TransitionDef<Ctx> Table[] = {            \
        { 1, static_cast<uint16_t>(Signal), 1,                    \
          coact::TransitionKind::Internal, nullptr, Action },     \
    }

// ---------------------------------------------------------------------------
// Time helper: PAL-backed monotonic clock (no bare clock_gettime — the demo
// must compile identically on POSIX host and RT-Thread target).
// ---------------------------------------------------------------------------
inline uint64_t monotonic_ns()
{
    return (nullptr != g_pal) ? g_pal->monotonic_ns() : 0U;
}

// ---------------------------------------------------------------------------
// Static HSM transition-trace channel: every guarded/exiting HSM transition
// prints a one-line `[ao] SRC --(sig)--> DST` record, mirroring a logic
// analyzer view onto the active objects. Single sink, no formatting buffers;
// the demo forces unbuffered stdout at boot so ordering is exact.
// ---------------------------------------------------------------------------
// Compiled AO id for the structured hsm trace (LogEvt::kEvtHsmTrace arg0).
// Values are stable ids, not indices into a runtime table.
enum class HsmAoId : uint32_t {
    kVideoFsm = 0U,
    kIrsc     = 1U,
    kEnhance  = 2U,
    kTpd      = 3U,
    kPack     = 4U,
    kMipi     = 5U,
    kRecfg    = 6U,
};

struct HsmTrace {
    // Every transition also lands in the coact::diag channel (kEvtHsmTrace):
    // a0=ao_id a1=src a2=sig a3=dst. src/dst are compact state codes so the
    // record stays uint32-only (the log writer renders raw hex); the printf
    // line above stays as the human-readable logic-analyzer view.
    static void transition(const char* ao, const char* src, uint16_t sig,
                           const char* dst)
    {
        std::printf("[hsm] %s %s --(%u)--> %s\n", ao, src,
                    static_cast<unsigned>(sig), dst);
        const uint32_t ao_id = ao_id_of(ao);
        const uint32_t code = (state_code_of(src) << 8U) | state_code_of(dst);
        g_log.record_from_task<LogLevel::kInfo, kEvtHsmTrace>(
            static_cast<uint16_t>(ao_id), sig, code, 0U);
    }
    // Rejections are abnormal control flow: kWarn carries them on the
    // critical lane. a3 bit0x100 flags "rejected" so the two records share
    // one event id; a3 low byte = reason code (0=guard, 1=session, 2=illegal).
    static void rejection(const char* ao, const char* src, uint16_t sig,
                          const char* why)
    {
        std::printf("[hsm] %s %s --(%u)--> REJECTED (%s)\n", ao, src,
                    static_cast<unsigned>(sig), why);
        const uint32_t ao_id = ao_id_of(ao);
        const uint32_t code = (state_code_of(src) << 8U) | state_code_of(why);
        g_log.record_from_task<LogLevel::kWarn, kEvtHsmTrace>(
            static_cast<uint16_t>(ao_id), sig, code, 0x100U);
    }

private:
    // ao name -> stable id (compile-time set; unknown -> 0xF).
    static uint32_t ao_id_of(const char* ao) noexcept
    {
        if (0 == std::strcmp(ao, "video_fsm")) { return static_cast<uint32_t>(HsmAoId::kVideoFsm); }
        if (0 == std::strcmp(ao, "irsc"))      { return static_cast<uint32_t>(HsmAoId::kIrsc); }
        if (0 == std::strcmp(ao, "enhance"))   { return static_cast<uint32_t>(HsmAoId::kEnhance); }
        if (0 == std::strcmp(ao, "tpd"))       { return static_cast<uint32_t>(HsmAoId::kTpd); }
        if (0 == std::strcmp(ao, "pack"))      { return static_cast<uint32_t>(HsmAoId::kPack); }
        if (0 == std::strcmp(ao, "mipi"))      { return static_cast<uint32_t>(HsmAoId::kMipi); }
        if (0 == std::strcmp(ao, "recfg"))     { return static_cast<uint32_t>(HsmAoId::kRecfg); }
        return 0xFU;
    }
    // State/reason name -> compact code (first char + length: collisions are
    // acceptable — the record is a filterable hint, the printf is the truth).
    static uint32_t state_code_of(const char* s) noexcept
    {
        return static_cast<uint32_t>(static_cast<uint8_t>(s[0]) & 0xFFU);
    }
};

// ---------------------------------------------------------------------------
// DiagTrace: binds core TraceOps to the coact::diag channel (design_trace
// §3.1). Submit keeps the core source_id (0 = unknown for the current submit
// API); Dispatch/LeaseContention carry the target id in source_id because
// those core callbacks have no source AO parameter. Elapsed ns splits into
// lo/hi uint32_t args so the 24-byte LogRecord keeps its fixed shape.
// ---------------------------------------------------------------------------
struct DiagTrace {
    // from_isr routes to the ISR-safe record entry (review P0-1): an ISR
    // context submit must never wake the writer through the task hook.
    static void on_submit(void* ctx, uint16_t source_id, coact::TargetId target,
                          uint16_t signal, uint8_t disposition, uint32_t reason,
                          bool from_isr) noexcept
    {
        (void)ctx;
        if (from_isr) {
            g_log.record_from_isr<LogLevel::kInfo, kEvtTraceSubmit>(
                source_id, static_cast<uint32_t>(target.raw()),
                static_cast<uint32_t>(signal), static_cast<uint32_t>(disposition),
                reason);
        }
        else {
            g_log.record_from_task<LogLevel::kInfo, kEvtTraceSubmit>(
                source_id, static_cast<uint32_t>(target.raw()),
                static_cast<uint32_t>(signal), static_cast<uint32_t>(disposition),
                reason);
        }
    }

    static void on_dispatch(void* ctx, coact::TargetId target, uint64_t elapsed_ns,
                            uint8_t path, uint8_t timeout) noexcept
    {
        (void)ctx;
        g_log.record_from_task<LogLevel::kInfo, kEvtTraceDispatch>(
            static_cast<uint16_t>(target.raw()),
            static_cast<uint32_t>(elapsed_ns & 0xFFFFFFFFU),
            static_cast<uint32_t>(elapsed_ns >> 32U),
            static_cast<uint32_t>(path), static_cast<uint32_t>(timeout));
    }

    static void on_lease_contention(void* ctx, coact::TargetId target, uint8_t kind,
                                    uint64_t elapsed_ns) noexcept
    {
        (void)ctx;
        g_log.record_from_task<LogLevel::kInfo, kEvtTraceLease>(
            static_cast<uint16_t>(target.raw()), static_cast<uint32_t>(kind),
            static_cast<uint32_t>(elapsed_ns & 0xFFFFFFFFU),
            static_cast<uint32_t>(elapsed_ns >> 32U), 0U);
    }

    static coact::TraceOps ops() noexcept
    {
        return coact::TraceOps{ &DiagTrace::on_submit, &DiagTrace::on_dispatch,
                                &DiagTrace::on_lease_contention, nullptr };
    }
};

// Compiled enum name tables: value -> label at compile time.
inline constexpr const char* kSigNames[] = {
    "?0", "kBoot", "kInitPreview", "kStopPreview", "kPhaseEnter", "kIrscCmd",
    "kIspCmd", "kVideoCmd", "kIrscReady", "kIspReady", "kVideoReady",
    "kFrameIrscOut", "kLowGainDone", "kHighGainDone", "kHlFused",
    "kEnhanceDone", "kTempChainDone", "kPicPacked", "kTempPacked", "kRecfgReq",
    "kRecfgStage", "kSoutIdle", "kFirstFrame", "kRecfgDone", "kFmtIdle",
    "kFrameEof", "kIrscDmaDone", "kIspNodeDone", "kSoutDone", "kMipiTxDone",
    "kSessionState", "?31", "?32", "?33", "?34", "kIspFifoOvf",
    "kMipiStreamErr", "kUsbErrInt", "kStreamDone", "kPassiveDataLoss",
};
inline constexpr const char* sig_name(uint16_t s) noexcept
{
    return (s < std::size(kSigNames)) ? kSigNames[s] : "sig?";
}

// ---------------------------------------------------------------------------
// Traits.
// ---------------------------------------------------------------------------
template <uint8_t Prio>
struct AoTrait {
    static LogicalPrio logical_prio() { return static_cast<LogicalPrio>(Prio); }
    static PriorityClass priority_class() { return PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

// Forward-declared IrscTrait in IrscDriverAo. Provide full definition now.
struct IrscTrait : AoTrait<62> {};

// PriorityClass layout (RTE model, rte_register_service): the ORCHESTRATOR
// rides the High partition (control acks must not age behind frame bursts);
// every pipeline AO stays Normal (frame data, aging-tolerant); the passive
// Windows-host observer rides Low (pure telemetry). Logical priorities stay
// unique per AO (bind-time check).
struct HighAoTrait {
    static LogicalPrio logical_prio() { return static_cast<LogicalPrio>(63); }
    static PriorityClass priority_class() { return PriorityClass::High; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
struct LowAoTrait {
    static LogicalPrio logical_prio() { return static_cast<LogicalPrio>(37); }
    static PriorityClass priority_class() { return PriorityClass::Low; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

enum class SessionState : uint8_t {
    kBoot     = 0U,
    kInit     = 1U,
    kRunning  = 2U,
    kRecfgTxn = 3U,   // nested inside RUNNING: quiesce->apply->sync->resume
    kDeinit   = 4U,
    kStopped  = 5U,
};

[[nodiscard]] inline const char* session_state_name(SessionState s) noexcept
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

// Atomic: the master advances it from the main thread while child-AO guards
// read it on the Dispatcher thread — lock-free, single byte, no torn reads.
inline std::atomic<SessionState> g_session{SessionState::kBoot};
static_assert(std::atomic<SessionState>::is_always_lock_free,
              "the session state is read by Dispatcher-thread guards while the"
              " main thread advances it: lock-free required");

// Late-bound plane pointers (assigned once in main before the runtime
// starts; read-only afterwards — no shared MUTABLE state crosses AOs).
inline PoolT* g_session_pool{nullptr};
inline Rt*    g_session_rt{nullptr};

// ---------------------------------------------------------------------------
// SessionEventComposite: the COMPOSITE of all AOs that must observe session
// transitions. Leaves are TargetIds; the composite is a fixed array filled
// once at boot — the same shape RS500's recursive init/deinit uses, flattened
// to plain iteration (no recursion; the tree has exactly two levels).
// ---------------------------------------------------------------------------
struct SessionEventComposite {
    static constexpr uint8_t kMaxTargets = 14U;
    TargetId targets[kMaxTargets]{};
    uint8_t  count{0U};

    void add(TargetId t)
    {
        if (count < kMaxTargets) {
            targets[count] = t;
            ++count;
        }
    }

    // Broadcast: iterate the fixed array, submit one kSessionState per leaf.
    // Every observer is serialized through its own AO queue, so guards read
    // the phase without any lock.
    void publish() const
    {
        if ((nullptr == g_session_pool) || (nullptr == g_session_rt)) {
            return;   // plane not armed yet (before main wires it)
        }
        for (uint8_t i = 0U; i < count; ++i) {
            Layout* e = g_session_pool->alloc_typed<Layout, Payload,
                                                    kPayloadAlign>(
                static_cast<uint16_t>(Sig::kSessionState));
            if (nullptr != e) {
                e->meta.cmd_arg = static_cast<uint16_t>(
                    g_session.load(std::memory_order_relaxed));
                g_session_rt->coordinator().submit_from_task(targets[i],
                                                             &e->event,
                                                             {false, false});
            }
        }
    }
};

inline SessionEventComposite g_session_events;

// Master-flow advance: print the (leveled) trace and fan out the gating
// event. Session states change ONLY through this single entry point. The
// exchange carries the previous phase out (printed), the new phase in.
inline void session_advance(SessionState s, const char* why)
{
    const SessionState prev = g_session.exchange(s);
    if (s == prev) { return; }
    std::printf("[session] %s --(%s)--> %s\n",
                session_state_name(prev), why, session_state_name(s));
    // Structured session trace: a0=prev a1=next (SessionState raw values).
    g_log.record_from_task<LogLevel::kInfo, kEvtSession>(
        0U, static_cast<uint32_t>(prev), static_cast<uint32_t>(s));
    // NOTE: child guards read the ATOMIC g_session directly, so no fan-out
    // event is needed for gating. The composite below remains the documented
    // composite-pattern mechanism (for AOs that need event-plane notice);
    // publishing per transition queued ~20 no-op events that only lengthened
    // the stop drain, so the broadcast is disabled in the pure-guard design.
}

// ---------------------------------------------------------------------------
// DMO (passive-mode) quiesce — the SECOND step of the two-step fix
// (嵌入式显示链路原子重配设计.md §5.3). The FIRST step (already live, see
// rcEnterPrecheck's stream-mode guard) conservatively REJECTS any partial
// RECFG request while the stream runs in DMO mode: the active-mode quiesce
// sequence (SOUT_STOP_FRAME -> frame-boundary IDLE) is not reliable under
// passive mode (Change 16105 shipped a DMO+SOUT combination that slipped
// through), so until the hardware path below is VERIFIED, DMO requests take
// the full rebuild path instead of a partial RECFG.
//
// The second step, expressed here as a compile-time policy skeleton (the same
// QuiescePolicy<HasIdleIrq> if-constexpr idiom above, NOT a runtime path):
//   1. stop the upstream source feeding the passive SOUT;
//   2. wait for the STREAM_DONE interrupt (kStreamDone, hal_stream_out.h);
//   3. if STREAM_DONE never arrives (interrupt loss), the PASSIVE_DATA_LOSS
//      interrupt (kPassiveDataLoss) is the fall-back quiesce evidence — a
//      bounded-loss guarantee, not a clean stop.
// kDmoQuiesceVerified=false selects the reject-first behavior; flipping it to
// true (ONLY after hardware validation) instantiates the passive quiesce.
// ---------------------------------------------------------------------------
inline constexpr bool kDmoQuiesceVerified = false;

struct DmoStreamDoneQuiesce {
    // Passive quiesce: stop upstream + wait STREAM_DONE; PASSIVE_DATA_LOSS is
    // the interrupt-loss fallback. Returns the observed interrupt signal id.
    [[nodiscard]] static uint16_t wait(bool stream_done_arrived) noexcept
    {
        return stream_done_arrived
                   ? static_cast<uint16_t>(Sig::kStreamDone)
                   : static_cast<uint16_t>(Sig::kPassiveDataLoss);
    }
};

template <bool PassiveQuiesceVerified>
struct DmoQuiescePolicy {
    [[nodiscard]] static bool allow_partial_recfg() noexcept
    {
        if constexpr (PassiveQuiesceVerified) {
            // Step 2 (hardware-validated): DMO may use the passive quiesce —
            // upstream stop + STREAM_DONE (kStreamDone), PASSIVE_DATA_LOSS
            // (kPassiveDataLoss) as the interrupt-loss fallback.
            (void)DmoStreamDoneQuiesce::wait(true);
            return true;
        } else {
            // Step 1 (today): passive quiesce unverified -> full rebuild.
            return false;
        }
    }
};

using DmoQuiesce = DmoQuiescePolicy<kDmoQuiesceVerified>;

}  // namespace isp_demo
