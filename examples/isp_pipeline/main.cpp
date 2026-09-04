/*
 * ===========================================================================
 * main.cpp —— 例子系统场景编排入口（14 AO + 7 worker + 三大异常仿真）
 * ===========================================================================
 * 功能描述
 *   - 构造 14 个 AO 实例并按 bind 顺序定 TargetId：Orchestrator(1) / IRSC(2)
 *     / low_gain(3) / high_gain(4) / hl_fuse(5) / enhance(6) / tpd(7) /
 *     video_fsm(8) / pack_vid(9) / mipi_sink(11) / recfg(12) / wrape(13)
 *     / winhost(14)（合并后从 16 减到 14；TargetId 10 留给 USB sink）。
 *   - 启动 7 个非 AO worker：CmdDmaWorker / IspIrqWorker(enh) /
 *     IspIrqWorker(tpd) / SoutDmaWorker / MipiIrqWorker / UsbDmaWorker；
 *     全部经 g_pal->thread_create，stop 时 drain-on-stop。
 *   - 构造 DdrCtx 单一拥有者（所有像素经过它）、武装 SessionEventComposite、
 *     提交 kBoot 启动 PreviewStart 全链路，再发起重配 kRecfgReq 验证 X1→X2。
 *   - 三类显示异常仿真：花屏（width 假设不一致）/ 丢帧（latency spike 突破
 *     窗口）/ 闪屏（restart raced quiesce），每个有 fault 注入 + 现象 + 修
 *     复断言；另含重配 / DMO 拒绝 / 旁路 / 镜像 / 同步等场景。
 *   - 末端 check() 断言覆盖 0 个失败时打印 "ALL PASS (fails=0)"，exit 0。
 *   对应 RS500 app_start_preview_sync → camera_stream_config_service 的
 *   应用层入口（app_fill_preview_cmd_by_mode 模式分支在此 dispatch）。
 *
 * 与其他文件的关系
 *   - 上游：本文件无上游（唯一入口）；include 全部 5 个 .hpp（按
 *     common → sensor_irsc → isp_chain → video_stream → output_itf →
 *     recfg_session 顺序，保证依赖闭合）。
 *   - 下游：直接绑定 14 AO 与 7 worker 的 Runtime，向它们提交 kBoot/
 *     kIrscCmd/kVideoCmd/kRecfgReq 等事件；读取 winhost.context().frames_
 *     received 等计数做断言；stop 时逆序 drain worker。
 *   - 依赖：common.hpp 的 PreviewCmdInfo/kProductModes 模式表选择；
 *     g_session_pool / g_session_rt 在 bind 前被武装；session_advance
 *     推进 BOOT→INIT→RUNNING→...→STOPPED。
 *
 * 文字图（编排视角）
 * ┌──────────────────────────────────────────────────────────────────┐
 * │ 例子系统数据流（→事件信号  ⇒DDR/黑板数据）                        │
 * │                                                                  │
 * │  [sensor_irsc]──kFrameIrscOut──▶[isp_chain]──kHlFused/Done──▶   │
 * │   ▲ (本文件启动)                  [video_stream]──kPicPacked──▶  │
 * │   │ kIrscCmd                      [output_itf]──kFrameEof──▶     │
 * │   │                               [winhost 观察者]                │
 * │                                                                  │
 * │  编排: [main.cpp] ──kIrscCmd/kVideoCmd──▶ 各模块 AO             │
 * │  重配: [recfg_session] ◀──kRecfgReq── main；门控 g_session      │
 * │  共享: [common.hpp] 词汇+DdrCtx+黑板 / PAL: g_pal (SemOps 等)    │
 * └──────────────────────────────────────────────────────────────────┘
 * ===========================================================================
 */
#include <cstdint>
#include <cstdio>

#include "isp_pipeline/common.hpp"
#include "isp_pipeline/sensor_irsc.hpp"
#include "isp_pipeline/isp_chain.hpp"
#include "isp_pipeline/video_stream.hpp"
#include "isp_pipeline/output_itf.hpp"
#include "isp_pipeline/recfg_session.hpp"

namespace isp_demo {
// ---------------------------------------------------------------------------
// Three display-anomaly simulations (花屏/丢帧/闪屏), each with a fault
// injection, an observable symptom, and the fix that eliminates it.
//
//   花屏 (garbled):   width-assumption broken. Data is 8-bit but the DMA
//                     reads 16-bit words: two pixels merge into one. The
//                     byte-level sink verifier catches it immediately.
//                     Fix: a single bit-width authority shared by producer
//                     and consumer (no per-layer assumption).
//   丢帧 (dropped):   rate balance broken. Tolerance window = buffer depth
//                     x frame interval; a latency SPIKE (not the average)
//                     that exceeds the window drops frames. Fix: deeper
//                     ring or bounded peak latency.
//   闪屏 (flicker):   restart raced the quiesce. Skipping the SOUT idle
//                     confirmation restarts a half-stopped block: the first
//                     frames carry the OLD configuration (mismatched clock /
//                     pointer), failing the first-frame check. Fix: wait for
//                     the idle ack (the RecfgHSM quiesce arc).
// ---------------------------------------------------------------------------

// ---- 花屏: bit-width mismatch ------------------------------------------------
// A tiny deterministic 8-bit frame producer/consumer pair over a "DMA".
struct WidthDemo {
    static constexpr uint16_t kPixels = 64U;

    // The single bit-width authority (the FIX): both sides must read this.
    uint16_t dma_bits_per_word{8U};
    bool authority_shared{true};

    std::array<uint8_t, kPixels> source{};   // 8-bit pixels
    std::array<uint8_t, kPixels> received{}; // what the consumer saw

    void produce(uint32_t seed) noexcept
    {
        for (uint16_t i = 0; i < kPixels; ++i) {
            source[i] = static_cast<uint8_t>((seed * 13U + i * 7U) & 0xFFu);
        }
    }

    // The DMA "transfer": when the consumer's width assumption differs from
    // the producer's, each 16-bit word slot carries TWO 8-bit pixels — the
    // consumer unpacks them as ONE pixel each, so half the frame's pixels
    // simply never arrive (two-pixels-merged garbling).
    [[nodiscard]] uint32_t transfer_mismatched(uint16_t consumer_bits) noexcept
    {
        uint32_t corrupted = 0U;
        if (consumer_bits == dma_bits_per_word) {
            received = source;                       // aligned: lossless
            return 0U;
        }
        // 8-bit data read as 16-bit words: pixel pairs merge; the frame the
        // consumer sees is only HALF as many pixels, each a blend of a pair.
        for (uint16_t i = 0; i < kPixels; i += 2U) {
            const uint8_t merged = static_cast<uint8_t>(
                (source[i] + source[i + 1U]) / 2U);   // pair averaged
            received[i / 2U] = merged;
            if (merged != source[i] && merged != source[i + 1U]) {
                ++corrupted;                          // neither pixel survived
            }
        }
        for (uint16_t i = kPixels / 2U; i < kPixels; ++i) {
            received[i] = 0U;                         // tail never transferred
        }
        return corrupted;
    }

    // The fix path: both sides take the width from the authority.
    [[nodiscard]] uint32_t transfer_authority() noexcept
    {
        return transfer_mismatched(dma_bits_per_word);
    }
};

// ---- 丢帧: peak latency vs the tolerance window ------------------------------
// Window = buffer_depth * frame_interval. A consumer with a latency SPIKE
// beyond the window overruns the ring (the DdrCtx overrun guard is exactly
// this detector at runtime; here it is modeled analytically).
struct RateDemo {
    uint32_t frame_interval_us{33333U};   // 30 fps
    uint16_t buffer_depth{3U};

    [[nodiscard]] uint32_t tolerance_us() const noexcept
    {
        return static_cast<uint32_t>(buffer_depth) * frame_interval_us;
    }

    // Does a consumer with these per-visit latencies (peak included) keep up?
    // Returns the number of DROPPED frames over a simulated window.
    [[nodiscard]] uint32_t drops_with(const std::array<uint32_t, 6>& latencies) noexcept
    {
        uint32_t behind_us = 0U;
        uint32_t drops = 0U;
        for (uint32_t lat : latencies) {
            behind_us = (behind_us + lat > frame_interval_us)
                            ? behind_us + lat - frame_interval_us
                            : 0U;
            if (behind_us > tolerance_us()) {
                ++drops;                 // a frame got recycled unread
                behind_us -= frame_interval_us;   // resync approximation
            }
        }
        return drops;
    }
};

// ---- 闪屏: restart without waiting for quiesce --------------------------------
// Models the "SOUT half-stopped, pulled back up" failure: the first frames
// after restart carry the OLD configuration. The first-frame check (the
// RecfgHSM commit boundary) is the detector.
struct FlickerDemo {
    bool sout_idle_confirmed{false};
    FrameGeometry old_geom{640U, 512U, 2U};
    FrameGeometry new_geom{1280U, 1024U, 2U};

    // Restart WITHOUT the idle confirmation: the block still holds the old
    // geometry for the first frames (pointer/clk not realigned).
    [[nodiscard]] uint32_t first_frame_bytes_raced() const noexcept
    {
        return sout_idle_confirmed ? new_geom.bytes_per_frame()
                                   : old_geom.bytes_per_frame();
    }
    // After a proper quiesce (idle ack) the restart is clean.
    void confirm_idle() noexcept { sout_idle_confirmed = true; }
};

}  // namespace isp_demo

int main()
{
    using namespace isp_demo;

    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // SoftIrq prerequisite, FIRST thread-related act of main(): block the
    // completion signal in this thread so every pthread spawned later (pump /
    // dispatcher / workers / log writer / softirq consumer) inherits the
    // blocked mask — a raise can then never hit an unblocked thread's default
    // disposition. No-op on the RT-Thread build (SoftIrq path off).
    softirq_block_completion_signal();

    // Dual-platform PAL: DemoPal is coact::pal::Posix on the host build and
    // coact::pal::RtThread under ISP_DEMO_USE_RTT (the RT-Thread compile
    // path); the RT-Thread variant references board-provided static
    // resources (worker thread table). Workers reach the instance through
    // the late-bound g_pal pointer.
#ifdef ISP_DEMO_USE_RTT
    // Static RT-Thread resources: Dispatcher stack 16 KiB, 8 producer
    // context slots, 8 worker-thread slots x 4 KiB stacks (7 workers live
    // at once; the 8th is headroom). All storage is static — zero heap.
    static coact::pal::RtThreadResources<16384U, 8U, 8U, 4096U> rtt_res;
    static DemoPal pal(rtt_res);
#elif defined(ISP_DEMO_CORO)
    static coact::coro::posix::StackfulExecutor<16U, 256U * 1024U> coro_exec;
    isp_demo_coro::g_exec = &coro_exec;
    static DemoPal pal;
    isp_demo_coro::install_pump_hook();
    isp_demo_coro::start_executor();
#else
    static DemoPal pal;
#endif
    g_pal = &pal;

    // SMP pool discipline (pal.hpp): the POSIX PAL's irq_save() is a no-op, so
    // a pool shared by MULTIPLE allocating/reclaiming threads (Dispatcher +
    // 7 worker pthreads here) must inject the spin critical section — the
    // no-op CS lets the batched reclaim's next-field writes race a concurrent
    // alloc's load_next on the same free block, silently corrupting the
    // free list (lost/duplicated events under stress). The spinlock is held
    // only across alloc/reclaim/splice — a few stores, never user code.
    // Pool critical section, platform-routed (design_isp_pipeline_optimization
    // P1). RT-Thread single-core: the plain irq-mask section (O(1), and alloc
    // vs reclaim are never concurrent on one core). Host/coro (SMP): the
    // POSIX PAL's irq_save() is a no-op, so a pool shared by MULTIPLE
    // allocating/reclaiming threads (Dispatcher + 7 worker pthreads) must
    // inject the spin critical section - the no-op CS lets the batched
    // reclaim's next-field writes race a concurrent alloc's load_next on the
    // same free block, silently corrupting the free list. The spinlock is
    // held only across alloc/reclaim/splice - a few stores, never user code.
    // Pool storage is STATIC (review P1/P4): ~16 KiB of Layout blocks must
    // not sit on the board main-task stack (RT-Thread main stacks are KBs);
    // it is a fixed resource and belongs in .bss with the rest of the
    // static-resource budget table (common.hpp).
    alignas(kPayloadAlign) static std::array<uint8_t, sizeof(Layout) * 128U + kPayloadAlign> storage{};
#ifdef ISP_DEMO_USE_RTT
    PoolT pool;
    pool.init(storage.data(), storage.size(),
              coact::make_critical_section(pal));
#else
    static coact::SpinCriticalSection pool_cs;
    static PoolT pool;
    pool.init(storage.data(), storage.size(),
              coact::make_spin_critical_section(pool_cs));
#endif

    static Rt rt(pal);

    // Bind the core TraceOps to the diag channel before the first submit so
    // every framework submit/dispatch/lease-contention lands in g_log as
    // kEvtTraceSubmit/kEvtTraceDispatch/kEvtTraceLease (design_trace T2).
    // The callbacks only store pointers now; records begin once g_log starts.
    rt.monitor().bind_trace(DiagTrace::ops());

    // Monitor watermark crossing faults (design P3.1): the Dispatcher-side
    // single-point sampler reports 80% threshold crossings through this
    // boundary (review P1: previously bound on no demo sink, so crossings
    // were silently dropped). High-priority crossings use the critical lane;
    // lower-priority recoveries stay on normal and retain the priority arg.
    static const coact::FaultReporter monitor_fault{
        [](uint16_t partition, uint32_t detail,
           coact::FaultPriority priority, void*) noexcept {
            const uint32_t pct = (detail >> 8U) & 0xFFU;
            if (coact::FaultPriority::kHigh <= priority) {
                g_log.record_from_task<LogLevel::kError, kEvtWatermarkFault>(
                    0U, partition, pct, static_cast<uint32_t>(priority));
            }
            else {
                g_log.record_from_task<LogLevel::kWarn, kEvtWatermarkFault>(
                    0U, partition, pct, static_cast<uint32_t>(priority));
            }
        }, nullptr};
    rt.monitor().bind_fault(monitor_fault);

    // TargetIds (1-based, in bind order):
    //   1=Orchestrator, 2=IRSC driver, 3=low_gain, 4=high_gain, 5=hl_fuse,
    //   6=enhance, 7=tpd_chain, 8=video FSM (merged PIC+TEMP), 9=video pack
    //   (merged PIC+TEMP), 10=USB sink, 11=MIPI sink, 12=recfg, 13=WRAPE,
    //   14=Windows host. (16 AOs before the merge; 14 after.)
    static OrchestratorAo orch(kOrchStates, static_cast<uint16_t>(std::size(kOrchStates)),
                        kOrchTransitions, static_cast<uint16_t>(std::size(kOrchTransitions)),
                        1, 2U);
    static IrscDriverAo irsc_drv(kIrscStates, static_cast<uint16_t>(std::size(kIrscStates)),
                          kIrscTransitions, static_cast<uint16_t>(std::size(kIrscTransitions)),
                          1, 2U);
    static LowGainAo low(kChainStates, static_cast<uint16_t>(std::size(kChainStates)),
                  kLowTransitions, static_cast<uint16_t>(std::size(kLowTransitions)),
                  1, 2U);
    static HighGainAo high(kChainStates, static_cast<uint16_t>(std::size(kChainStates)),
                    kHighTransitions, static_cast<uint16_t>(std::size(kHighTransitions)),
                    1, 2U);
    static HlFuseAo hl(kHlStates, static_cast<uint16_t>(std::size(kHlStates)),
                kHlTransitions, static_cast<uint16_t>(std::size(kHlTransitions)),
                1, 2U);
    static EnhanceAo enhance(kFusedStates, static_cast<uint16_t>(std::size(kFusedStates)),
                      kEnhanceTransitions, static_cast<uint16_t>(std::size(kEnhanceTransitions)),
                      1, 2U);           // initial state: Active (index 1)
    static TempChainAo tpd(kFusedStates, static_cast<uint16_t>(std::size(kFusedStates)),
                    kTempTransitions, static_cast<uint16_t>(std::size(kTempTransitions)),
                    1, 2U);           // initial state: Active (index 1)
    static VideoFsmAo videofsm(kVideoStates, static_cast<uint16_t>(std::size(kVideoStates)),
                        kVideoTransitions, static_cast<uint16_t>(std::size(kVideoTransitions)),
                        1, 2U);         // initial state: PIC_IDLE (index 1)
    static VideoPackAo packvid(kPackStates, static_cast<uint16_t>(std::size(kPackStates)),
                        kPackTransitions, static_cast<uint16_t>(std::size(kPackTransitions)),
                        1, 2U);         // initial state: PIC_ACTIVE (index 1)
    static UsbSinkAo usb(kSinkStates, static_cast<uint16_t>(std::size(kSinkStates)),
                  kUsbSinkTransitions, static_cast<uint16_t>(std::size(kUsbSinkTransitions)),
                  1, 2U);
    static MipiSinkAo mipi(kMipiStates, static_cast<uint16_t>(std::size(kMipiStates)),
                    kMipiSinkTransitions, static_cast<uint16_t>(std::size(kMipiSinkTransitions)),
                    1, 2U);
    static RecfgOrchAo recfg(kRecfgStates, static_cast<uint16_t>(std::size(kRecfgStates)),
                      kRecfgTransitions, static_cast<uint16_t>(std::size(kRecfgTransitions)),
                      1, 2U);
    static WrapeAo wrape(kWrapeStates, static_cast<uint16_t>(std::size(kWrapeStates)),
                  kWrapeTransitions, static_cast<uint16_t>(std::size(kWrapeTransitions)),
                  1, 2U);
    static WinHostAo winhost(kWinHostStates, static_cast<uint16_t>(std::size(kWinHostStates)),
                      kWinHostTransitions, static_cast<uint16_t>(std::size(kWinHostTransitions)),
                      1, 2U);

    rt.bind(&orch);
    rt.bind(&irsc_drv);
    rt.bind(&low);
    rt.bind(&high);
    rt.bind(&hl);
    rt.bind(&enhance);
    rt.bind(&tpd);
    rt.bind(&videofsm);
    rt.bind(&packvid);
    rt.bind(&usb);
    rt.bind(&mipi);
    rt.bind(&recfg);
    rt.bind(&wrape);
    rt.bind(&winhost);

    // Wire up contexts.
    const TargetId kOrchId    = TargetId(1U);
    const TargetId kIrscId    = TargetId(2U);
    const TargetId kLowId     = TargetId(3U);
    const TargetId kHighId    = TargetId(4U);
    const TargetId kHlId      = TargetId(5U);
    const TargetId kEnId      = TargetId(6U);
    const TargetId kTpdId     = TargetId(7U);
    const TargetId kVideoFsmId = TargetId(8U);
    const TargetId kPackVidId = TargetId(9U);
    const TargetId kMipiId    = TargetId(11U);
    const TargetId kRecfgId   = TargetId(12U);
    const TargetId kWrapeId   = TargetId(13U);
    const TargetId kWinHostId = TargetId(14U);

    // Single DDR owner: the one home of every pixel region. All node actions
    // hit it on the dispatcher thread (serialized), so no lock, no blackboard.
    static DdrCtx ddr;

    // ---- arm the session plane BEFORE any submission can happen ----------
    g_session_pool = &pool;
    g_session_rt = &rt;
    g_session_events.add(kIrscId);
    g_session_events.add(kVideoFsmId);
    g_session_events.add(kPackVidId);
    g_session_events.add(kEnId);
    g_session_events.add(kTpdId);

    // ---- non-AO workers: the hardware behavior plane ----------------------
    // (Started before the AOs accept events; stopped in reverse below, each
    //  drained of in-flight jobs — see the stop section near the end.)
    static CmdDmaWorker cmd_dma;
    static IspIrqWorker isp_irq_enh;    // enhance node-done line (ISP hw IRQ 0)
    static IspIrqWorker isp_irq_tpd;    // TPD node-done line (ISP hw IRQ 1)
    static SoutDmaWorker sout_dma;      // SOUT writeback (path id in the job)
    static MipiIrqWorker mipi_irq;      // MIPI CSI TX completion

    // Worker fault sink (design_static_aop A4): a completion-reject fault is
    // reported through each worker's FaultReporter boundary as
    // kEvtWorkerFault (a0=worker_id a1=result a2=rejects a3=priority).
    // Severity routing (review P2): kCritical/kHigh faults log at kError ->
    // the diag CRITICAL
    // lane (an error burst never consumes normal capacity); kMedium/kLow
    // stay on normal. Null-bound would be zero cost but silent; the demo
    // binds all five so the path is observable. Reports run on worker
    // threads -> record_from_task.
    static const coact::FaultReporter worker_fault{
        [](uint16_t worker_id, uint32_t detail,
           coact::FaultPriority priority, void*) noexcept {
            if (coact::FaultPriority::kHigh <= priority) {
                g_log.record_from_task<LogLevel::kError, kEvtWorkerFault>(
                    0U, worker_id, (detail >> 16U) & 0xFFFFU,
                    detail & 0xFFFFU, static_cast<uint32_t>(priority));
            }
            else {
                g_log.record_from_task<LogLevel::kWarn, kEvtWorkerFault>(
                    0U, worker_id, (detail >> 16U) & 0xFFFFU,
                    detail & 0xFFFFU, static_cast<uint32_t>(priority));
            }
        }, nullptr};
    cmd_dma.bind_fault(worker_fault);
    isp_irq_enh.bind_fault(worker_fault);
    isp_irq_tpd.bind_fault(worker_fault);
    sout_dma.bind_fault(worker_fault);
    mipi_irq.bind_fault(worker_fault);

    // Worker bring-up failures are fatal (review P1): a half-started
    // channel would reject every submit and the scenario would misreport.
    if (!cmd_dma.start(&pool, &rt, kIrscId)
        || !isp_irq_enh.start(&pool, &rt, kEnId, 0U)      // trace id 0
        || !isp_irq_tpd.start(&pool, &rt, kTpdId, 5U)     // trace id 5 (type 0)
        || !sout_dma.start(&pool, &rt, kPackVidId)
        || !mipi_irq.start(&pool, &rt, kMipiId)) {
        return 1;
    }

    irsc_drv.context().pool = &pool;
    irsc_drv.context().rt = &rt;
    irsc_drv.context().orchestrator = kOrchId;
    irsc_drv.context().cmd_channel = &cmd_dma;

    low.context().label = "low_gain";
    low.context().simulated_us = kLat.low_gain_us;
    low.context().pool = &pool;
    low.context().rt = &rt;
    low.context().ddr = &ddr;
    low.context().owner_target = kHlId;
    low.context().out_region = DdrId::kDdrLowGain;
    low.context().done_signal = static_cast<uint16_t>(Sig::kLowGainDone);

    high.context().label = "high_gain";
    high.context().simulated_us = kLat.high_gain_us;
    high.context().pool = &pool;
    high.context().rt = &rt;
    high.context().ddr = &ddr;
    high.context().owner_target = kHlId;
    high.context().out_region = DdrId::kDdrHighGain;
    high.context().done_signal = static_cast<uint16_t>(Sig::kHighGainDone);

    hl.context().pool = &pool;
    hl.context().rt = &rt;
    hl.context().ddr = &ddr;
    hl.context().enhance_target = kEnId;
    hl.context().tpd_target = kTpdId;

    // enhance: node id 0 on the shared ISP IRQ channel
    enhance.context().label = "enhance";
    enhance.context().simulated_us = kLat.enhance_us;
    enhance.context().pool = &pool;
    enhance.context().rt = &rt;
    enhance.context().ddr = &ddr;
    enhance.context().out_region = DdrId::kDdrPicOut;
    // Per-path completion id: the pack pair-state table discriminates the
    // two stream inputs BY SIGNAL (not by a flags bit) — the same id from
    // both instances would be ambiguous in the merged product HSM.
    enhance.context().done_signal =
        static_cast<uint16_t>(Sig::kEnhanceDonePic);
    enhance.context().path_flag = 0U;       // PIC path
    enhance.context().payload_kind = 1U;
    enhance.context().node_id = 0U;
    enhance.context().downstream = kPackVidId;
    enhance.context().self_target = kEnId;
    enhance.context().irq_channel = &isp_irq_enh;

    // tpd: node id 1, its own IRQ line (the two ISP nodes are independent
    // completion sources — two channels, two workers, no shared routing).
    tpd.context().label = "tpd";
    tpd.context().simulated_us = kLat.tpd_us;
    tpd.context().pool = &pool;
    tpd.context().rt = &rt;
    tpd.context().ddr = &ddr;
    tpd.context().out_region = DdrId::kDdrTemp;
    // Per-path completion id (see the enhance wiring above).
    tpd.context().done_signal =
        static_cast<uint16_t>(Sig::kEnhanceDoneTemp);
    tpd.context().path_flag = 1U;           // TEMP path
    tpd.context().payload_kind = 2U;
    tpd.context().node_id = 1U;
    tpd.context().downstream = kPackVidId;
    tpd.context().self_target = kTpdId;
    tpd.context().irq_channel = &isp_irq_tpd;

    // Merged video FSM: one ctx, two mirrors.
    videofsm.context().pool = &pool;
    videofsm.context().rt = &rt;
    videofsm.context().orchestrator = kOrchId;
    videofsm.context().node_names[0] = "ISP_CUT_ZOOM";
    videofsm.context().node_names[1] = "VIDEO_CUT_ZOOM";
    videofsm.context().node_names[2] = "PSD";
    videofsm.context().node_names[3] = "OSD_0";
    videofsm.context().node_names[4] = "OSD_1";
    videofsm.context().node_names[5] = "SOUT";
    videofsm.context().node_names[6] = "OUT";
    videofsm.context().pic.kind = kPicStream;
    videofsm.context().pic.node_count = 7U;
    videofsm.context().temp.kind = kTempStream;
    videofsm.context().temp.node_count = 4U;

    // Merged video pack: one ctx, two sinks, one SOUT channel.
    packvid.context().pool = &pool;
    packvid.context().rt = &rt;
    packvid.context().ddr = &ddr;
    packvid.context().pic_sink = kWrapeId;   // T37: PIC leaves via WRAPE
    packvid.context().temp_sink = kMipiId;
    packvid.context().self_target = kPackVidId;
    packvid.context().sout_channel = &sout_dma;

    usb.context().name = "usb(uvc)";
    usb.context().expected_tag = "PIC";
    usb.context().ddr = &ddr;
    mipi.context().name = "mipi(csi)";
    mipi.context().expected_tag = "TEMP";
    mipi.context().ddr = &ddr;
    mipi.context().pool = &pool;
    mipi.context().rt = &rt;
    mipi.context().self_target = kMipiId;
    mipi.context().tx_channel = &mipi_irq;

    // T37 UVC chain: WRAPE frames the packed PIC; the non-AO USB DMA engine
    // ships the bulk transactions and posts EOF to the Windows host AO.
    static UsbDmaWorker usb_dma;
    if (!usb_dma.start(&pool, &rt, kWinHostId)) {
        mipi_irq.stop();
        sout_dma.stop();
        isp_irq_tpd.stop();
        isp_irq_enh.stop();
        cmd_dma.stop();
        return 1;
    }
    wrape.context().pool = &pool;
    wrape.context().rt = &rt;
    wrape.context().ddr = &ddr;
    wrape.context().dma_engine = &usb_dma;

    // Recfg AO plumbing (geometry set after `cmd` is selected, below).
    recfg.context().pool = &pool;
    recfg.context().rt = &rt;
    recfg.context().self_target = kRecfgId;
    recfg.context().report_to = kOrchId;
    recfg.context().active_magx = SrMagx::kX1;
    g_addr_cache.store(0xA0000000U);   // a live address record under v1

    Event init_e{0U, 0U, 0U};
    orch.init(init_e);
    irsc_drv.init(init_e);
    low.init(init_e); high.init(init_e); hl.init(init_e);
    enhance.init(init_e); tpd.init(init_e);
    videofsm.init(init_e); packvid.init(init_e);
    usb.init(init_e); mipi.init(init_e); recfg.init(init_e);
    wrape.init(init_e); winhost.init(init_e);

    rt.initialize();

    // Bring up the coact::diag log channel BEFORE rt.start() (design P3.2:
    // the sink must be live before the Dispatcher can produce trace
    // records). Startup order nuance (review): the 5 WorkerBase workers and
    // usb_dma are created earlier in scenario setup and only BEGIN
    // submitting once the runtime runs, so no record can precede this
    // initialization. A failed log bring-up FAILS the demo (review P3): a
    // silent dead sink would zero both accepted and drained and make the
    // conservation assertion vacuously true.
    const coact::diag::LogRtError log_err = g_log.initialize();
    bool diag_live = false;
    if (coact::diag::LogRtError::kOk == log_err) {
        diag_live =
            (coact::diag::LogRtError::kOk == g_log.start());
    }
    if (!diag_live) {
        std::printf("[FATAL] diag log failed to start (err=%u) - aborting\n",
                    static_cast<unsigned>(log_err));
        return 1;
    }
    g_log.record_from_task<LogLevel::kInfo, kEvtBoot>(0U);

    rt.start();
    bool isr_probe_accepted = false;
    if (Event* probe = pool.alloc(0xFFFFU)) {
        const coact::SubmitResult result =
            rt.coordinator().try_submit_from_isr(
                kOrchId, probe, coact::EventQos{false, false});
        isr_probe_accepted =
            (coact::SubmitDisposition::Queued == result.disposition);
    }

    // ============== Orchestrator ==============
    // Plays the role of app_start_preview_sync -> camera_stream_config_service.
    // Drives IRSC 4-step, ISP pipeline, PIC+TEMP video FSM, then runs.

    const bool cali = false;     // mirror product branch (RS500_CALI off)
    const uint8_t mode_sel = 4;  // 1920 MIPI NV12 PIC_TEMP (uses lower period)
    const PreviewCmdInfo cmd = select_cmd(cali, mode_sel);

    // Recfg AO starts with the pipeline's live geometry (X1 = cmd geometry).
    recfg.context().active_geom = FrameGeometry{cmd.width, cmd.height, 2U};

    std::printf("=== isp_pipeline_demo (Preview Start emulation) ===\n");
    std::printf("  mode=%u arch=%s itf=%s %ux%u@%u sout=%u mode=%u fmt=%u direct=%d\n",
                mode_sel,
                cmd.arch == ArchStream::kPicTemp ? "PIC_TEMP" : "PIC_ONLY",
                cmd.itf == Itf::kUsbS0 ? "USB_S0" : "MIPI_TX0_CSI_S0",
                cmd.width, cmd.height, cmd.fps,
                static_cast<unsigned>(cmd.sout),
                static_cast<unsigned>(cmd.mode),
                static_cast<unsigned>(cmd.fmt),
                cmd.stream_out_direct ? 1 : 0);

    // Skip stream_type=PIC_ONLY handling on demo path; arch==PIC_TEMP always.
    const bool need_temp = (cmd.arch == ArchStream::kPicTemp);

    // ---- Phase1: data preload (placeholder; bypassed like auto_preview path).
    std::printf("[orch] Phase1: data preload (bypass, pre_data_load_num=0)\n");
    g_log.record_from_task<LogLevel::kInfo, kEvtPhase1>(0U, 0U);

    // ---- Phase3: data load (placeholder; mirrors camera_preload_init_preview_data).
    std::printf("[orch] Phase3: data load (camera_preload_init_preview_data)\n");
    g_log.record_from_task<LogLevel::kInfo, kEvtPhase3>(0U, cmd.width, cmd.height);
    /* Phase3 预加载耗时原为固定 g_pal->sleep_us(1500)：此处前面未提交任何命令、
       无事件可轮询，placeholder 的"耗时"无验证价值，已删除 */

    // ---- Phase4: parameter application ----
    std::printf("[orch] Phase4: parameter application\n");
    g_log.record_from_task<LogLevel::kInfo, kEvtPhase4>(mode_sel);

    // IRSC 4 steps via the compile-time command table: order IS the contract.
    // Each command stamps its own identity into the event block (command
    // pattern; mirrors rt_device_control(DEV_IRSC_CTRL_*) dispatch).
    // Session: BOOT -> INIT opens the driver command window.
    session_advance(SessionState::kInit, "Phase4 parameter application");
    for (const IrscCmd& cmd : kIrscCmdSequence) {
        Layout* e = pool.alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(Sig::kIrscCmd));
        if (nullptr == e) { continue; }
        e->meta.reply_to = kOrchId;
        cmd.stamp(*e);
        // Control plane: critical (never dropped, see send_video note).
        rt.coordinator().submit_from_task(kIrscId, &e->event, {true, false});
    }
    std::printf("[orch] IRSC: init -> start -> ctrl -> output_enable (KBC then IRSC order)\n");

    // ISP pipeline init (mirrors isp_init_pipeline).
    {
        // Synthesize 8 internal kIspReady acks directly into the orchestrator
        // (modeling the nodes' init completion).
        for (uint8_t i = 0; i < 8U; ++i) {
            Layout* n = pool.alloc_typed<Layout, Payload, kPayloadAlign>(
                static_cast<uint16_t>(Sig::kIspReady));
            if (nullptr != n) {
                n->meta.cmd_arg = i;
                n->meta.reply_to = kOrchId;
                Payload* p = reinterpret_cast<Payload*>(&n->payload[0]);
                p->control[0] = 'I'; p->control[1] = 'S'; p->control[2] = 'P';
                p->control[3] = static_cast<uint8_t>(0xB0U + i);
                rt.coordinator().submit_from_task(kOrchId, &n->event, {true, false});
            }
        }
        std::printf("[orch] ISP pipeline: init %u nodes forward\n", 8U);
    }

    // Video stream FSM init (PIC always; TEMP if PIC_TEMP). The MERGED AO
    // receives both command streams; the stream identity rides in flags bit0.
    auto send_video = [&](VideoCmd c, bool temp, const char* who) {
        Layout* e = pool.alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(Sig::kVideoCmd));
        if (nullptr == e) { return; }
        e->meta.cmd_arg = static_cast<uint16_t>(c);
        e->meta.flags = temp ? 1U : 0U;   // stream discriminator (command stamp)
        e->meta.reply_to = kOrchId;
        // Control plane: critical=true — an L2-quarantined target drops
        // non-critical events; boot/stop commands must never be dropped.
        rt.coordinator().submit_from_task(kVideoFsmId, &e->event, {true, false});
        std::printf("[orch] video(%s) cmd=%u\n", who, static_cast<unsigned>(c));
    };

    // Guard-reject exercise: START while still IDLE is illegal — the merged
    // HSM must refuse it (rejection arc) before the session opens RUNNING.
    send_video(kVStart, false, "PIC");

    send_video(kVInit, false, "PIC");
    if (need_temp) { send_video(kVInit, true, "TEMP"); }

    // KBC_OUTPUT_ENABLE then IRSC_OUTPUT_ENABLE (mirrors the critical order).
    std::printf("[orch] KBC_OUTPUT_ENABLE -> IRSC_OUTPUT_ENABLE\n");

    // Video START (PIC stream enable, then TEMP if applicable). The session
    // enters RUNNING here — after this, the IrscDriver guard still accepts
    // (RUNNING is an open phase) but the video FSM rejects re-init.
    session_advance(SessionState::kRunning, "streams enabled");
    send_video(kVStart, false, "PIC");
    if (need_temp) { send_video(kVStart, true, "TEMP"); }

    // ---- Start the IRSC producer pthread ----
    IrscWorker irsc;
    if (!irsc.start(&pool, &rt, kLowId, kHighId, cmd.fps)) {
        usb_dma.stop();
        mipi_irq.stop();
        sout_dma.stop();
        isp_irq_tpd.stop();
        isp_irq_enh.stop();
        cmd_dma.stop();
        rt.stop();
        g_log.stop();
        return 1;
    }

    // ---- Drain ----
    coact::AoBase* aos[] = { &orch, &irsc_drv, &low, &high, &hl, &enhance, &tpd,
                             &videofsm, &packvid,
                             &usb, &mipi, &recfg, &wrape, &winhost };
    for (uint32_t w = 0U; w < 3000U; ++w) {
        bool drained = true;
        for (coact::AoBase* a : aos) {
            if (0U != a->pending().load()) { drained = false; }
        }
        if (drained && wrape.context().frames_framed >= kFrameCount
                   && winhost.context().frames_received >= kFrameCount
                   && mipi.context().frames_received >= kFrameCount) {
            break;
        }
        g_pal->sleep_us(5000U);
    }

    irsc.stop();

    // =====================================================================
    // Runtime atomic reconfiguration scenario (models the complex business
    // case: AI super-resolution X1 -> X2 switch while the stream is live).
    // Three passes:
    //   1. X4 request -> precheck REJECT (capability matrix, zero hw touch)
    //   2. X2 with fault injection -> SOUT frames with stale X1 geometry
    //      (the "655360B premature EOF" bug reproduced: truncated frame)
    //   3. X2 clean -> single-authority geometry, full-frame commit
    // =====================================================================
    std::printf("\n=== runtime reconfiguration (AI SR X1 -> X2) ===\n");
    auto request_recfg = [&](SrMagx magx, bool inject_stale) {
        // Session window opens from the MAIN thread: the broadcast must never
        // be submitted from the Dispatcher thread (a dispatcher-context
        // submit races the wakeup latch — the exact lost-wakeup the stress
        // runs exposed once in ~15). The recfg AO only runs the transaction.
        session_advance(SessionState::kRecfgTxn, "kRecfgReq accepted");
        Layout* e = pool.alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(Sig::kRecfgReq));
        if (nullptr == e) { return; }
        e->meta.cmd_arg = static_cast<uint16_t>(magx)
                        | (inject_stale ? 0x100U : 0U);
        rt.coordinator().submit_from_task(kRecfgId, &e->event, {true, false});
    };

    // Wait for the recfg transaction's SELF-DRIVEN window close: rcGoHome
    // (the terminal arc action, on the Dispatcher thread) restores the
    // session to RUNNING when the transaction reaches its end state. Waiting
    // on the session — not the AO counters — is the semantically correct
    // condition: the driver may only resume issuing requests once the
    // transaction window it opened is observably closed. The terminal arc is
    // guaranteed reachable (the recfg AO's own stage chain self-submits to
    // it), so this converges without a hard timeout.
    auto wait_txn_closed = []() {
        for (uint32_t w = 0U; w < 500U; ++w) {
            if (SessionState::kRunning == g_session.load(std::memory_order_relaxed)) {
                return;
            }
            g_pal->sleep_us(2000U);
        }
        // Fallback: the window never closed — surface it as a txn failure.
        ++recfg.context().recfgs_failed;
    };

    // Pass 1: X4 — the capability matrix rejects it everywhere.
    request_recfg(SrMagx::kX4, false);
    wait_txn_closed();

    // Pass 1b: DMO mode gap (嵌入式显示链路原子重配设计.md §3.2 边界1 +
    // §5.3): switch the request's stream mode to DMO (passive) — the precheck
    // guard rejects the partial RECFG (step 1 of the two-step fix: the
    // active-mode stop-stable sequence is unreliable under DMO; Change 16105
    // shipped a DMO+SOUT combination that slipped validation). The session
    // window opens and self-drives closed on the same terminal arc as the X4
    // reject, then the mode is restored to Active (3LOOP stands in: the
    // demo's passes run the active-mode quiesce sequence).
    std::printf("[recfg] pass 1b: DMO mode request (full rebuild required)\n");
    for (uint32_t w = 0U; w < 200U; ++w) {
        if (0U == rt.monitor().ao(kRecfgId).pending.load()
            && RecfgStage::kIdle == recfg.context().stage) {
            break;
        }
        g_pal->sleep_us(1000U);
    }
    recfg.context().stream_mode = StreamMode::kDmo;
    request_recfg(SrMagx::kX2, false);
    wait_txn_closed();
    recfg.context().stream_mode = StreamMode::k3Loop;   // restore Active mode
    // Pass 2: X2 with the stale-SOUT fault injection.
    request_recfg(SrMagx::kX2, true);
    wait_txn_closed();
    // Wait for the faulty transaction to recover, then check the stale
    // address cache behavior (window close == transaction terminal).
    wait_txn_closed();
    {
        uint32_t addr = 0U;
        const bool hit = g_addr_cache.query(addr);
        std::printf("[cache] stale address query after reconfig: %s "
                    "(version guard killed the old record)\n",
                    hit ? "HIT" : "MISS (forced reload)");
    }
    // Pass 3: X2 clean — single-authority geometry fixes the truncation.
    // Reset to X1 first so the demo shows a full X1 -> X2 transition. The
    // reset waits for the recfg AO to be fully DRAINED (pending == 0): a
    // ctx write racing the AO's own commit exchange is the same class of
    // interleaving the arc guards below eliminate on the event plane.
    for (uint32_t w = 0U; w < 200U; ++w) {
        if (0U == rt.monitor().ao(kRecfgId).pending.load()
            && RecfgStage::kIdle == recfg.context().stage) {
            break;
        }
        g_pal->sleep_us(1000U);
    }
    recfg.context().active_magx = SrMagx::kX1;
    recfg.context().active_geom = FrameGeometry{cmd.width, cmd.height, 2U};
    request_recfg(SrMagx::kX2, false);
    wait_txn_closed();
    std::printf("[recfg] summary: committed=%u failed=%u layout_version=%u\n",
                recfg.context().recfgs_committed,
                recfg.context().recfgs_failed,
                recfg.context().layout_version);
    /* Structured summary: a0=7(summary) a1=committed a2=failed a3=version. */
    g_log.record_from_task<LogLevel::kInfo, kEvtRecfg>(
        0U, 7U, recfg.context().recfgs_committed,
        recfg.context().recfgs_failed, recfg.context().layout_version);

    // =====================================================================
    // T37 UVC X2 premature-EOF: the WRAPE framing lags the stream geometry.
    // The Identity-Zoom workaround keeps downstream geometry fixed.
    // =====================================================================
    std::printf("\n=== T37 UVC X2 premature-EOF + Identity Zoom ===\n");
    std::printf("  profile: fixed 1280x1024@30, streamVldNum=%u, bulkXSF=%u B\n",
                kStreamVldNum, kBulkXsfBytes);

    // At this point the pipeline already streamed 30 frames through the
    // WRAPE (X1 mode, aligned framing): those are the complete baseline.
    const uint32_t baseline_complete = wrape.context().complete_frames;
    const uint32_t baseline_framed = wrape.context().frames_framed;
    const uint32_t baseline_host = winhost.context().frames_received;

    // Drive the T37 scenario with real frames. The frames come from the same
    // pipeline transform as the streamed ones, so the WRAPE's DDR byte check
    // stays exact: synthesize the enhanced bytes and write them into the
    // kDdrPicOut slot the event will point at (dn -> lg/hg -> fused -> exp).
    uint32_t t37_fid = kFrameCount;   // continues the streamed id sequence
    auto t37_send_frame = [&]() {
        uint8_t expected[kSlotPayloadBytes];
        const uint32_t fid = t37_fid;
        for (uint16_t i = 0; i < kSlotPayloadBytes; ++i) {
            const uint32_t dn  = (fid * 7U + i) & 0xFFu;
            const uint32_t lg  = (dn * 3U) / 4U;
            const uint32_t hg  = ((dn * 5U) / 4U) & 0xFFu;
            const uint32_t fus = ((lg * 2U) + (hg * 3U)) / 5U;
            expected[i] = static_cast<uint8_t>(((fus * 6U) / 5U) & 0xFFu);
        }
        const uint16_t buf = ddr.write(DdrId::kDdrPicOut, fid, expected);
        Layout* e = pool.alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(Sig::kPicPacked));
        if (nullptr == e) { return; }
        e->meta.frame_id = fid;
        e->meta.buffer_idx = buf;
        e->meta.payload_kind = 1U;
        Payload* p = reinterpret_cast<Payload*>(&e->payload[0]);
        p->control[0] = 'P'; p->control[1] = 'I'; p->control[2] = 'C';
        p->ddr_slot = buf; p->ddr_id = DdrId::kDdrPicOut;
        rt.coordinator().submit_from_task(kWrapeId, &e->event, {false, false});
        ++t37_fid;
    };
    // Drain: the USB DMA engine ships one frame in bulk transactions, so wait
    // for the Windows host to see the frame EOF (engine is single-job).
    auto t37_drain = [&]() {
        for (uint32_t w = 0U; w < 5000U; ++w) {
            if (winhost.context().frames_received >= baseline_host + t37_fid - kFrameCount) {
                return;
            }
            g_pal->sleep_us(1000U);
        }
    };

    // Phase T2: THE BUG — X2 input, WRAPE keeps stale X1 framing.
    // Fault-mode note (manual 7.4.1): the REAL failure mode this demo models
    // is "Zoom is turned OFF across the switch" — the downstream geometry
    // desyncs from the configured framing. Here the fault surface is the
    // configured_frame_bytes lag instead; zoom stays ACTIVE (the Identity
    // principle: always on, only the step changes), so the fix in phase 3 is
    // precisely "keep it active with the right coefficient". All register
    // writes go through the blackboard setters (contract W above) and happen
    // behind the previous phase's quiesce barrier (contract S above).
    hw_bb_set_zoom_step(kZoomStepIdentity);        // X2 -> identity (step 256)
    hw_bb_set_wrape_framing(kX1FrameBytes, true);  // stale X1 framing = the bug
    std::printf("[t37] phase 2: X2 without workaround (WRAPE stale X1 framing)\n");
    for (uint32_t f = 0U; f < kT37Phase2Frames; ++f) {
        t37_send_frame();
        t37_drain();
    }

    // Phase T3: THE FIX — Identity Zoom: framing matches the fixed output.
    hw_bb_set_wrape_framing(kOutFrameBytes, false);
    std::printf("[t37] phase 3: X2 with Identity Zoom (framing fixed at %u B)\n",
                kOutFrameBytes);
    for (uint32_t f = 0U; f < kT37Phase3Frames; ++f) {
        t37_send_frame();
        t37_drain();
    }

    // Phase T4: X1 <-> X2 x10 switch rounds (workaround stays on). Each round
    // writes the zoom step behind the previous round's drain — the register
    // write never lands mid-frame (contract S), and sel.stream_vld_num is
    // never touched: the SEL count is the constant downstream contract.
    for (uint32_t round = 0U; round < kT37Phase4Rounds; ++round) {
        hw_bb_set_zoom_step(
            (0U == round % 2U) ? kZoomStep2x : kZoomStepIdentity);
        // Downstream geometry never changes — that is the whole point.
        t37_send_frame();
        t37_drain();
    }
    std::printf("[t37] phase 4: X1<->X2 x10 rounds, zoom always active, "
                "downstream 1280x1024 constant\n");
    std::printf("[t37] summary: framed=%u err_eof=%u complete=%u | winhost "
                "frames=%u truncated=%u gaps=%u\n",
                wrape.context().frames_framed - baseline_framed,
                wrape.context().err_eof_frames,
                wrape.context().complete_frames - baseline_complete,
                winhost.context().frames_received,
                winhost.context().truncated_frames,
                winhost.context().frame_gaps);

    // =====================================================================
    // Register-cache consistency scenarios (regmap-style protocol demo).
    // =====================================================================
    std::printf("\n=== register-cache consistency ===\n");
    PeriphRegCache regs;
    // Baseline: identical copies, some tuned values.
    for (uint16_t r = 0; r < kRegCount; ++r) {
        regs.st.shadow[r] = 0x1000U + r;
        regs.st.hardware[r] = 0x1000U + r;
    }

    // --- Scenario A: BARE bypass (the 2.1 failure) -----------------------
    // Factory test writes registers directly; nobody syncs the shadow.
    {
        regs.cache_bypass = true;              // raw flag, no guard
        regs.write(2U, 0xBEEFU);               // tuned value, hardware only
        regs.write(5U, 0xCAFEU);
        regs.cache_bypass = false;             // exit WITHOUT resync
    }
    regs.audit("after bare bypass");           // adds the bare-bypass drift
    // Baseline: drift observed so far is Scenario A evidence (environment).
    const uint32_t drift_before_fix = regs.drift_events;
    // Days later: a routine scene switch triggers a full parameter re-push,
    // silently replaying the STALE shadow over the tuned registers.
    const uint32_t hw_before_repush = regs.st.hardware[2];
    regs.full_repush();
    const uint32_t hw_after_repush = regs.st.hardware[2];
    std::printf("[cache] scenario A: full repush overwrote tuned hw 0x%x -> 0x%x "
                "(silent param backflow)\n",
                hw_before_repush, hw_after_repush);

    // --- Scenario B: PAIRED bypass (the fix) ------------------------------
    {
        BypassGuard guard(regs);               // RAII: resync on ANY exit
        regs.write(2U, 0xBEEFU);
        regs.write(5U, 0xCAFEU);
    }                                          // resync happens here
    regs.audit("after paired bypass");         // zero drift lines expected
    const uint32_t drift_after_fix = regs.drift_events;
    std::printf("[cache] scenario B: paired bypass resynced "
                "(total drift=%u, new drift=%u)\n",
                drift_after_fix, drift_after_fix - drift_before_fix);

    // --- Scenario C: cache_only freeze window + sync convergence ---------
    bool sync_refused = false;
    {
        regs.cache_only = true;                // freeze window opens
        regs.write(1U, 0x1111U);               // dirty accumulates in shadow
        regs.write(3U, 0x3333U);
        // sync() returns false when it refuses (still cache_only).
        const bool sync_succeeded = regs.sync();
        sync_refused = !sync_succeeded;        // freeze must reject the commit
        std::printf("[cache] scenario C: sync during freeze refused=%d "
                    "(precondition assert)\n", sync_refused ? 1 : 0);
        regs.cache_only = false;               // frame boundary reached
        const bool ok = regs.sync();           // commits the dirty set
        std::printf("[cache] scenario C: sync at boundary ok=%d "
                    "(frame-atomic commit)\n", ok ? 1 : 0);
    }

    // --- Scenario D: mirror desync (2.3) ----------------------------------
    {
        const DisplayMirror mirror{true, 640U};   // user enables horizontal mirror
        const DetRect box{100U, 50U, 200U, 150U}; // detection box in raw coords

        // The failure: display path mirrors, detection coordinates don't.
        const DetRect stale = box;              // nobody transformed it
        // The fix: route through the single transform authority.
        const DetRect fixed = transform(box, mirror);
        std::printf("[cache] scenario D: mirror box raw=[%u,%u..%u,%u] "
                    "stale(same)=[%u,%u..%u,%u] fixed=[%u,%u..%u,%u]\n",
                    box.x0, box.y0, box.x1, box.y1,
                    stale.x0, stale.y0, stale.x1, stale.y1,
                    fixed.x0, fixed.y0, fixed.x1, fixed.y1);
    }

    // --- Scenario E: bit-field RMW through the cache (field-level protocol) -
    // The register is a FIELD GROUP — writing the opcode must move only
    // opcode[4:7]; mode[8:9] and enable[0] are the FSM's live stream posture
    // and MUST survive. Same discipline, same three-flag protocol, just a
    // narrower lane than write()'s whole-word store.
    std::uint32_t bf_sout_mode_before = 0U;
    std::uint32_t bf_ai_bypass_before = 0U;
    std::uint32_t bf_sout_after = 0U;
    std::uint32_t bf_ai_after = 0U;
    {
        // Seed both toy registers through the field layer (write-through).
        regs.write_field<SoutCtrlEnable>(kRegSoutCtrl, kSoutEnableOn);
        regs.write_field<SoutCtrlMode>(kRegSoutCtrl, kSoutModeActive);
        regs.write_field<SoutCtrlOpcode>(kRegSoutCtrl, kSoutOpcodeFrame);
        regs.write_field<AiCtrlBypass>(kRegAiCtrl, 0U);
        regs.write_field<AiCtrlMagx>(kRegAiCtrl, 1U);

        // Snapshot the neighbor fields BEFORE the field-level RMWs.
        bf_sout_mode_before = regs.read_field<SoutCtrlMode>(kRegSoutCtrl);
        bf_ai_bypass_before = regs.read_field<AiCtrlBypass>(kRegAiCtrl);

        // The isolation RMWs: opcode and magx move ONLY their own lanes.
        regs.write_field<SoutCtrlOpcode>(kRegSoutCtrl, kSoutOpcodeRecfg);
        regs.write_field<AiCtrlMagx>(kRegAiCtrl, 2U);
        bf_sout_after = regs.st.shadow[kRegSoutCtrl];
        bf_ai_after = regs.st.shadow[kRegAiCtrl];
        std::printf("[cache] scenario E: SOUT_CTRL shadow=0x%x (opcode=%u "
                    "mode=%u enable=%u), AI_CTRL shadow=0x%x (magx=%u "
                    "bypass=%u); hw=0x%x/0x%x\n",
                    bf_sout_after, SoutCtrlOpcode::read(bf_sout_after),
                    SoutCtrlMode::read(bf_sout_after),
                    SoutCtrlEnable::read(bf_sout_after),
                    bf_ai_after, AiCtrlMagx::read(bf_ai_after),
                    AiCtrlBypass::read(bf_ai_after),
                    regs.st.hardware[kRegSoutCtrl],
                    regs.st.hardware[kRegAiCtrl]);
    }

    // =====================================================================
    // Error-signal injection scenario: one deliberate interrupt per chain —
    // the NEW signals (kIspFifoOvf / kMipiStreamErr / kUsbErrInt) each get
    // exactly one worker->AO delivery here, then the counters are asserted.
    // Normal runs (before this block) observe ZERO of them; the pipeline
    // counts the error and degrades — never crashes, never changes an FSM.
    // =====================================================================
    std::printf("\n=== error-signal injection (one interrupt per chain) ===\n");
    {
        // 1. ISP FIFO_OVERFLOW: frame 0 is long released; the AO counts the
        //    stale overflow and drops it (parking ring miss is tolerated).
        isp_irq_enh.inject_fifo_overflow(0U, 0U);
        // 2. MIPI CSI stream error: counted, FSM posture unchanged (§9 rule).
        mipi_irq.inject_stream_error(kFrameCount - 1U);
        // 3. USB error interrupt: driver-side count, EOF data path untouched.
        usb_dma.inject_error_interrupt(kFrameCount - 1U);
        // Drain: the three injected events must land before the counters are
        // read (pending-based, same contract as every other drain here).
        for (uint32_t w = 0U; w < 500U; ++w) {
            if (enhance.context().fifo_ovf >= 1U
                && mipi.context().stream_errs >= 1U
                && winhost.context().error_interrupts >= 1U) {
                break;
            }
            g_pal->sleep_us(1000U);
        }
    }

    // =====================================================================
    // Three display anomalies: 花屏 / 丢帧 / 闪屏.
    // =====================================================================
    std::printf("\n=== display anomalies (garbled / dropped / flicker) ===\n");

    // --- 花屏: bit-width mismatch -----------------------------------------
    uint32_t garbled_pairs = 0U;
    uint32_t garbled_fixed = 0U;
    {
        WidthDemo wd;
        wd.produce(42U);
        // The failure: data is 8-bit, consumer assumes 16-bit words.
        garbled_pairs = wd.transfer_mismatched(16U);
        std::printf("[anomaly] garbled: 8-bit data x 16-bit DMA -> %u/%u "
                    "pixel pairs corrupted (two-pixels-merged)\n",
                    garbled_pairs, WidthDemo::kPixels / 2U);
        // The fix: both sides read the shared width authority.
        garbled_fixed = wd.transfer_authority();
        std::printf("[anomaly] garbled (fixed): shared width authority -> "
                    "%u corrupted\n", garbled_fixed);
    }

    // --- 丢帧: peak latency breaks the tolerance window -------------------
    uint32_t drops_normal = 0U;
    uint32_t drops_spike = 0U;
    uint32_t drops_deep = 0U;
    uint32_t tolerance_us = 0U;
    {
        RateDemo rd;
        tolerance_us = rd.tolerance_us();
        // Normal traffic: per-visit latency well inside the interval.
        drops_normal = rd.drops_with({30000U, 31000U, 29000U, 33000U, 30000U, 32000U});
        // The spike: one 250ms stall (software conversion long-tail) blows
        // the 3-buffer x 33ms window; frames drop until the consumer resyncs.
        drops_spike = rd.drops_with({30000U, 250000U, 30000U, 31000U, 30000U, 32000U});
        std::printf("[anomaly] dropped: window=%u us (3buf x 33ms); "
                    "steady=%u drops, 250ms-spike=%u drops (peak, not average)\n",
                    tolerance_us, drops_normal, drops_spike);
        // Fix A: deeper ring absorbs the same spike.
        rd.buffer_depth = 8U;
        drops_deep = rd.drops_with({30000U, 250000U, 30000U, 31000U, 30000U, 32000U});
        std::printf("[anomaly] dropped (fix): 8-buffer window=%u us -> "
                    "%u drops\n", rd.tolerance_us(), drops_deep);
    }

    // --- 闪屏: restart raced the quiesce -----------------------------------
    uint32_t raced_bytes = 0U;
    uint32_t clean_bytes = 0U;
    uint32_t target_bytes = 0U;
    {
        FlickerDemo fd;
        // The failure: skip the SOUT idle confirmation and START immediately.
        raced_bytes = fd.first_frame_bytes_raced();
        target_bytes = fd.new_geom.bytes_per_frame();
        std::printf("[anomaly] flicker: restart without idle ack -> first "
                    "frame %u B (OLD geometry) vs expected %u B -> flash\n",
                    raced_bytes, target_bytes);
        // The fix: the quiesce arc (idle ack) realigns clock/pointers first.
        fd.confirm_idle();
        clean_bytes = fd.first_frame_bytes_raced();
        std::printf("[anomaly] flicker (fixed): quiesced restart -> first "
                    "frame %u B (new geometry, aligned)\n", clean_bytes);
    }

    // ---- STOP_PREVIEW (reverse order: video stream STOP, then DEINIT reverse) ----
    std::printf("\n[orch] STOP_PREVIEW: reverse order\n");
    g_log.record_from_task<LogLevel::kInfo, kEvtStop>(0U, need_temp ? 1U : 0U);
    // Session linkage: DEINIT closes the streaming window — the IrscDriver
    // guard now refuses further commands (its reject arc), proving the
    // master->child gating end to end.
    session_advance(SessionState::kDeinit, "STOP_PREVIEW");
    if (need_temp) { send_video(kVStop, true, "TEMP"); }
    send_video(kVStop, false, "PIC");
    if (need_temp) { send_video(kVDeinit, true, "TEMP"); }
    send_video(kVDeinit, false, "PIC");
    // Event-driven drain: wait until EVERY AO's queue is empty AND the video
    // FSMs reached IDLE — a fixed sleep races the Dispatcher under load
    // (observed once in stress: the stop cmds were still queued when the
    // counters were read). Pending-based drain is the correct contract.
    for (uint32_t w = 0U; w < 3000U; ++w) {
        bool drained = true;
        for (coact::AoBase* a : aos) {
            if (0U != a->pending().load()) { drained = false; }
        }
        if (drained && videofsm.context().pic.fsm == kVideoIdle
                   && videofsm.context().temp.fsm == kVideoIdle) {
            break;
        }
        g_pal->sleep_us(1000U);
    }
    // T37: the USB DMA engine still ships the last bulk transactions. Drain
    // every EOF into the Windows host BEFORE stopping the engine (stop would
    // discard an unfinished job), then park the worker thread.
    t37_drain();
    // Worker-drain guard: every async channel must have delivered ALL of its
    // completions (in-flight jobs are never dropped, only awaited).
    for (uint32_t w = 0U; w < 500U; ++w) {
        if (mipi.context().tx_done_count >= kFrameCount
            && packvid.context().pic_sout_done >= packvid.context().pic_frames
            && packvid.context().temp_sout_done >= packvid.context().temp_frames
            && enhance.context().irq_done_count >= enhance.context().irq_subs
            && tpd.context().irq_done_count >= tpd.context().irq_subs) {
            break;
        }
        g_pal->sleep_us(1000U);
    }
    // Session terminal state (every subscriber has reached quiescence).
    session_advance(SessionState::kStopped, "all segments deinit'd");
    // Worker stop order — in-flight jobs drain to their completion events and
    // those events must be processed by a LIVE runtime, so:
    //   1. mipi/isp/sout/cmd channels drain first (their completions feed AOs)
    //   2. the USB engine drains its last bulk transactions (feeds winhost)
    //   3. THEN the runtime stops (dispatcher processes everything accepted)
    mipi_irq.stop();
    isp_irq_enh.stop();
    isp_irq_tpd.stop();
    sout_dma.stop();
    cmd_dma.stop();
    usb_dma.stop();
    // Structured worker statistics (kEvtWorkerStat): a0=worker_id a1=executed
    // (or transactions/frames) a2=rejected. Emitted BEFORE g_log.stop() so
    // the writer drains them; the printf summary below stays as the readable
    // end-of-run report.
    g_log.record_from_task<LogLevel::kInfo, kEvtWorkerStat>(
        0U, usb_dma.transactions_done, 0U);
    g_log.record_from_task<LogLevel::kInfo, kEvtWorkerStat>(
        1U, cmd_dma.executed_count(), cmd_dma.rejected_count());
    g_log.record_from_task<LogLevel::kInfo, kEvtWorkerStat>(
        2U, isp_irq_enh.executed_count(), isp_irq_enh.rejected_count());
    g_log.record_from_task<LogLevel::kInfo, kEvtWorkerStat>(
        3U, isp_irq_tpd.executed_count(), isp_irq_tpd.rejected_count());
    g_log.record_from_task<LogLevel::kInfo, kEvtWorkerStat>(
        4U, sout_dma.executed_count(), sout_dma.rejected_count());
    g_log.record_from_task<LogLevel::kInfo, kEvtWorkerStat>(
        5U, mipi_irq.executed_count(), mipi_irq.rejected_count());
    g_log.record_from_task<LogLevel::kInfo, kEvtWorkerStat>(
        6U, kFrameCount, 0U);   // irsc producer
    rt.stop();
    // Structured diag statistics AFTER g_log.stop(): the stop join drains
    // both lanes, so the conservation identity is drained == accepted per
    // lane (admission-dropped records never entered the ring and are
    // reported separately as dropped). stdout line counts are NOT the
    // integrity proof (design P3.2).
    g_log.stop();
    bool diag_conservation_ok = false;
    {
        const coact::diag::LogStats st = g_log.logger().snapshot();
        std::printf("=== diag channel stats ===\n");
        std::printf("  normal  : accepted=%u drained=%u dropped=%u "
                    "(conservation %s) hwm=%u\n",
                    static_cast<unsigned>(st.accepted_normal),
                    static_cast<unsigned>(st.drained_normal),
                    static_cast<unsigned>(st.dropped_at_enqueue_normal),
                    (st.drained_normal == st.accepted_normal) ? "OK"
                                                              : "BROKEN",
                    static_cast<unsigned>(st.normal_high_watermark));
        std::printf("  critical: accepted=%u drained=%u dropped=%u "
                    "(conservation %s) hwm=%u\n",
                    static_cast<unsigned>(st.accepted_critical),
                    static_cast<unsigned>(st.drained_critical),
                    static_cast<unsigned>(st.dropped_at_enqueue_critical),
                    (st.drained_critical == st.accepted_critical) ? "OK"
                                                                  : "BROKEN",
                    static_cast<unsigned>(st.critical_high_watermark));
        std::printf("  faults  : catalog_miss=%u truncated=%u sink_failed=%u "
                    "wake_signals=%u\n",
                    static_cast<unsigned>(st.catalog_miss),
                    static_cast<unsigned>(st.formatter_truncated),
                    static_cast<unsigned>(st.sink_failed),
                    static_cast<unsigned>(st.wake_signal));
        // Static lane capacities alongside the run stats (design P3.1: the
        // sampling report carries capacity, occupancy and trace drops).
        std::printf("  capacity: normal=%u critical=%u "
                    "(normal watermark drops above)\n",
                    static_cast<unsigned>(
                        coact::diag::LogRtThreadBase::kNormalCapacity),
                    static_cast<unsigned>(
                        coact::diag::LogRtThreadBase::kCriticalCapacity));
        diag_conservation_ok = (st.drained_normal == st.accepted_normal)
                               && (st.drained_critical
                                   == st.accepted_critical);
    }
    const coact::GlobalCounters& monitor_stats = rt.monitor().global();
    bool watermark_observed = true;
    for (size_t i = 0U; i < 3U; ++i) {
        const uint32_t samples = monitor_stats.watermark_samples[i].load(
            std::memory_order_relaxed);
        const uint16_t used = monitor_stats.watermark_used[i].load(
            std::memory_order_relaxed);
        const uint16_t capacity = monitor_stats.watermark_capacity[i].load(
            std::memory_order_relaxed);
        std::printf("  watermark[%zu]: samples=%u used=%u capacity=%u\n", i,
                    static_cast<unsigned>(samples),
                    static_cast<unsigned>(used),
                    static_cast<unsigned>(capacity));
        watermark_observed = watermark_observed && (samples > 0U)
                              && (capacity > 0U);
    }
    // SoftIrq completion path: the winhost EOF drain above already awaited
    // every delivery; usb_dma.stop() (which joins the softirq consumer) has
    // meanwhile printed its own delivered count. Report the reconciliation
    // here so the ISR-path evidence sits next to the winhost counters.
    usb_dma.print_softirq_stat();
#ifdef ISP_DEMO_CORO
    isp_demo_coro::stop_executor();
    std::printf("[coro] executor stopped: single-thread cooperative mode "
                "(worker pthreads: 0)\n");
#endif

    std::printf("\n=== per-node latency ===\n");
    std::printf("  low_gain  : total=%u frames=%u avg=%u (sim=%u)\n",
                low.context().total_us, low.context().frames_handled,
                low.context().frames_handled ? low.context().total_us / low.context().frames_handled : 0U,
                low.context().simulated_us);
    std::printf("  high_gain : total=%u frames=%u avg=%u (sim=%u)\n",
                high.context().total_us, high.context().frames_handled,
                high.context().frames_handled ? high.context().total_us / high.context().frames_handled : 0U,
                high.context().simulated_us);
    std::printf("  hl_fuse   : total=%u frames=%u avg=%u\n",
                hl.context().total_us, hl.context().frames_fused,
                hl.context().frames_fused ? hl.context().total_us / hl.context().frames_fused : 0U);
    std::printf("  enhance   : total=%u frames=%u avg=%u (sim=%u)\n",
                enhance.context().total_us, enhance.context().frames_handled,
                enhance.context().frames_handled ? enhance.context().total_us / enhance.context().frames_handled : 0U,
                enhance.context().simulated_us);
    std::printf("  tpd_chain : total=%u frames=%u avg=%u (sim=%u)\n",
                tpd.context().total_us, tpd.context().frames_handled,
                tpd.context().frames_handled ? tpd.context().total_us / tpd.context().frames_handled : 0U,
                tpd.context().simulated_us);
    std::printf("  pic_pack  : frames=%u last_pack_us=%u sout_subs=%u done=%u\n",
                packvid.context().pic_frames, packvid.context().pic_last_pack_us,
                packvid.context().pic_sout_subs, packvid.context().pic_sout_done);
    std::printf("  temp_pack : frames=%u last_pack_us=%u sout_subs=%u done=%u\n",
                packvid.context().temp_frames, packvid.context().temp_last_pack_us,
                packvid.context().temp_sout_subs, packvid.context().temp_sout_done);
    std::printf("  wrape     : framed=%u complete=%u err_eof=%u byte_mismatch=%u tag_mismatch=%u\n",
                wrape.context().frames_framed, wrape.context().complete_frames,
                wrape.context().err_eof_frames, wrape.context().byte_mismatch,
                wrape.context().tag_mismatch);
    std::printf("  winhost   : frames=%u complete=%u truncated=%u min_payload=%u max_payload=%u gaps=%u\n",
                winhost.context().frames_received,
                winhost.context().complete_frames,
                winhost.context().truncated_frames,
                winhost.context().min_payload, winhost.context().max_payload,
                winhost.context().frame_gaps);
    std::printf("  usb_sink  : idle (T37 moved the PIC data plane to WRAPE)\n");
    std::printf("  mipi_sink : frames=%u first=%u last=%u total_us=%u tag_mismatch=%u byte_mismatch=%u overrun=%u\n",
                mipi.context().frames_received, mipi.context().first_frame_id,
                mipi.context().last_frame_id, mipi.context().total_us,
                mipi.context().tag_mismatch, mipi.context().byte_mismatch,
                mipi.context().frame_overrun);
    std::printf("  ddr       : write_ops=%u read_ops=%u overrun_drops=%u ownership_skips=%u (data plane through DDR)\n",
                ddr.write_ops, ddr.read_ops, ddr.overrun_drops, ddr.ownership_skips);
    std::printf("  irsc_drv  : step_count=%u ready=%d\n",
                irsc_drv.context().step_count, irsc_drv.context().ready ? 1 : 0);
    std::printf("  orch      : irsc_ready=%u isp_ready=%u pic_video_ready=%u temp_video_ready=%u last_irsc_step=%u\n",
                orch.context().irsc_ready, orch.context().isp_ready,
                orch.context().pic_video_ready, orch.context().temp_video_ready,
                orch.context().last_irsc_step);
    std::printf("  video_fsm : pic=%s temp=%s (merged AO, rejects=%u+%u)\n",
                video_state_name(videofsm.context().pic.fsm),
                video_state_name(videofsm.context().temp.fsm),
                videofsm.context().pic.rejected_cmds,
                videofsm.context().temp.rejected_cmds);
    std::printf("  quiesce   : sout(event) ioctls=%u us=%u | fmt888(poll) ioctls=%u us=%u\n",
                videofsm.context().sout_ioctls,
                videofsm.context().sout_quiesce_us,
                videofsm.context().fmt_ioctls,
                videofsm.context().fmt_quiesce_us);
    // ---- the four (seven-instance) non-AO workers: channel statistics ----
    std::printf("  --- hardware workers (non-AO pthreads) ---\n");
    std::printf("  usb_dma   : transactions=%u (WRAPE bulk engine)\n",
                usb_dma.transactions_done);
    std::printf("  cmd_dma   : executed=%u rejected=%u (vdcmd register channel)\n",
                cmd_dma.executed_count(), cmd_dma.rejected_count());
    std::printf("  isp_irq_enh: executed=%u rejected=%u (enhance node-done irq)\n",
                isp_irq_enh.executed_count(), isp_irq_enh.rejected_count());
    std::printf("  isp_irq_tpd: executed=%u rejected=%u (tpd node-done irq)\n",
                isp_irq_tpd.executed_count(), isp_irq_tpd.rejected_count());
    std::printf("  sout_dma  : executed=%u rejected=%u (SOUT writeback)\n",
                sout_dma.executed_count(), sout_dma.rejected_count());
    std::printf("  mipi_irq  : executed=%u rejected=%u (MIPI CSI TX done)\n",
                mipi_irq.executed_count(), mipi_irq.rejected_count());
    std::printf("  irsc      : emitted %u DN frames (producer pthread)\n",
                kFrameCount);
    std::printf("  session   : final=%s\n", session_state_name(g_session));
    std::printf("  pool.used=%u (expect 0), hwm=%u\n",
                static_cast<unsigned>(pool.used()),
                static_cast<unsigned>(pool.high_watermark()));

    // =====================================================================
    // Self-verification: every simulated scenario asserts its invariants.
    // Exit code carries the verdict so ctest can gate on it.
    // =====================================================================
    uint32_t fails = 0U;
    auto check = [&fails](bool ok, const char* what) {
        std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) { ++fails; }
    };

    std::printf("\n=== verification ===\n");
    // Data plane: every frame byte-verified through the whole DDR chain.
    // T37 moved the PIC data plane from the USB sink to WRAPE -> WinHost.
    check(wrape.context().frames_framed == kFrameCount + kT37Phase2Frames
                                    + kT37Phase3Frames + kT37Phase4Rounds,
          "PIC sink (WRAPE) received all frames");
    check(mipi.context().frames_received == kFrameCount, "TEMP sink received all frames");
    check(wrape.context().byte_mismatch == 0U, "PIC data plane byte-exact");
    check(mipi.context().byte_mismatch == 0U, "TEMP data plane byte-exact");
    check(wrape.context().tag_mismatch == 0U && mipi.context().tag_mismatch == 0U,
          "route tags correct");
    // DDR ring integrity: the data plane promises ZERO slot overruns — every
    // consumer read the real bytes it was pointed at (a degraded fill_dn seed
    // would replay the seed formula downstream and could still pass the byte
    // check, so overrun==0 is the only guard that the check ran on real data).
    check(ddr.overrun_drops == 0U, "DDR slot guard: zero overrun degradations");
    check(diag_conservation_ok,
          "diag: lane conservation identity (drained+dropped==accepted)");
    check(isr_probe_accepted &&
              g_isr_trace_submits.load(std::memory_order_relaxed) > 0U,
          "ISR submit: accepted and traced through ISR-safe sink");
    check(watermark_observed,
          "monitor: watermark samples include usage and capacity");
    // AOP behavior assertions (design_static_aop review #6): the aspect
    // chain must be observably ALIVE, not merely compiled - every
    // completion worker accumulated real execution time and the periodic
    // producer counted its frames.
    check(cmd_dma.execution_duration_ns() > 0ULL
              && isp_irq_enh.execution_duration_ns() > 0ULL
              && isp_irq_tpd.execution_duration_ns() > 0ULL
              && sout_dma.execution_duration_ns() > 0ULL
              && mipi_irq.execution_duration_ns() > 0ULL,
          "AOP: all five workers accumulated non-zero execution duration");
    check(irsc.frames_produced() == kFrameCount,
          "AOP: IRSC producer counted every frame");
    // Slot ownership protocol (the RS500 DMA descriptor ownership-bit mirror):
    // the writer must never have found a reader-claimed slot. On the demo's
    // pacing a single frame's lifetime is shorter than the ring wrap-around
    // (8 slots), so the skip path — the honest FIFO_OVERFLOW-style drop —
    // should NEVER fire. A non-zero skip here is a protocol misjudgement
    // (bug), not a real load scenario: the claim window is a single memcpy
    // inside a Dispatcher action.
    check(ddr.ownership_skips == 0U,
          "DDR ownership protocol: writer never hit a reader-claimed slot");
    // Sink overrun counters must agree: the WRAPE counts a read miss as
    // byte_mismatch; the MIPI sink counts it as frame_overrun. Both must be
    // zero, and no frame may exit the byte verify via the overrun early-out.
    check(wrape.context().byte_mismatch == 0U
              && mipi.context().frame_overrun == 0U
              && wrape.context().frames_framed == mipi.context().frames_received + kT37Phase2Frames + kT37Phase3Frames + kT37Phase4Rounds,
          "data plane: every frame byte-verified (no overrun early-outs)");
    // T37 UVC: premature-EOF reproduced in phase 2, fixed from phase 3 on.
    check(wrape.context().err_eof_frames == kT37Phase2Frames,
          "T37: phase 2 premature-EOF frames (ERR+EOF)");
    check(wrape.context().complete_frames == baseline_complete + kT37Phase3Frames
                                         + kT37Phase4Rounds,
          "T37: phase 3+4 frames complete at fixed geometry");
    check(winhost.context().truncated_frames == kT37Phase2Frames,
          "T37: Windows host saw the truncated payloads");
    check(winhost.context().frames_received
              == baseline_host + kT37Phase2Frames + kT37Phase3Frames
                               + kT37Phase4Rounds,
          "T37: Windows host received every frame EOF");
    check(winhost.context().frame_gaps == 0U, "T37: no frame gaps on the host");
    check(winhost.context().min_payload == kX1FrameBytes,
          "T37: truncated payload is the stale X1 length");
    check(winhost.context().max_payload == kOutFrameBytes,
          "T37: complete payload is the fixed output length");
    check(g_hw_bb.zoom.active, "T37: zoom block never turned off");
    check(g_hw_bb.sel.stream_vld_num == kStreamVldNum,
          "T37: Stream SEL valid-pixel count constant (frame-length authority)");
    check(wrape.context().zoom_geometry_mismatch == 0U,
          "T37: zoom step consistent with frame geometry throughout");
    check(g_hw_bb.wrape.configured_frame_bytes == kOutFrameBytes,
          "T37: downstream geometry constant across X1<->X2 rounds");
    // Event pool: full reclaim (zero leak).
    check(pool.used() == 0U, "event pool fully reclaimed");
    // Boot orchestration: all command acks collected.
    check(irsc_drv.context().step_count == 4U, "IRSC 4-step command sequence");
    check(orch.context().isp_ready == 8U, "ISP 8-node init acks");
    check(orch.context().pic_video_ready == 1U && orch.context().temp_video_ready == 1U,
          "video FSM init acks (PIC + TEMP)");
    // Video FSM: full IDLE -> READY -> RUNNING -> READY -> IDLE cycle.
    check(videofsm.context().pic.fsm == kVideoIdle
              && videofsm.context().temp.fsm == kVideoIdle,
          "video FSMs back to IDLE after reverse deinit");
    check(videofsm.context().pic.rejected_cmds == 1U
              && videofsm.context().temp.rejected_cmds == 0U,
          "video FSM: illegal START-in-IDLE rejected by guard arc");
    // Session gating: the master session reached its terminal state and the
    // driver refused the post-deinit command (child guard observed it).
    check(g_session == SessionState::kStopped, "session reached STOPPED");
    check(irsc_drv.context().channel_rejects == 0U
              && irsc_drv.context().dma_done_count == 4U,
          "IRSC async channel: 4 register writes, 4 completions, 0 rejects");
    // Non-AO worker invariants: channel counters vs AO-side observations.
    check(cmd_dma.executed_count() == irsc_drv.context().step_count
              && cmd_dma.executed_count() == 4U,
          "CmdDmaWorker: executed == IRSC step_count == 4 (vdcmd channel)");
    check(enhance.context().irq_done_count == enhance.context().frames_handled
              && enhance.context().irq_subs == enhance.context().irq_done_count,
          "IspIrqWorker(enhance): requests == completions == frames");
    check(tpd.context().irq_done_count == tpd.context().frames_handled
              && tpd.context().irq_subs == tpd.context().irq_done_count,
          "IspIrqWorker(tpd): requests == completions == frames");
    check(enhance.context().irq_rejects == 0U && tpd.context().irq_rejects == 0U,
          "IspIrqWorker: zero queue-full rejects");
    check(isp_irq_enh.completion_rejects == 0U
              && isp_irq_tpd.completion_rejects == 0U
              && sout_dma.completion_rejects == 0U
              && mipi_irq.completion_rejects == 0U
              && cmd_dma.completion_rejects == 0U
              && usb_dma.completion_rejects == 0U,
          "IRQ/DMA completion events: zero pool-allocation rejects");
    check(packvid.context().pic_sout_done == packvid.context().pic_frames
              && packvid.context().temp_sout_done == packvid.context().temp_frames,
          "SoutDmaWorker: writebacks == packed frames (PIC and TEMP)");
    check(packvid.context().pic_sout_rejects == 0U
              && packvid.context().temp_sout_rejects == 0U,
          "SoutDmaWorker: zero queue-full rejects");
    check(mipi.context().tx_done_count == mipi.context().frames_received
              && mipi_irq.executed_count() == kFrameCount,
          "MipiIrqWorker: TX completions == frames shipped");
    // Error-signal contracts: ZERO on the normal path (the 30 streamed frames
    // never tripped one); exactly the injected count after the injection
    // block (each error landed once, counted once, degraded not crashed).
    check(tpd.context().fifo_ovf == 0U,
          "IspIrqWorker: TPD chain never hit a FIFO_OVERFLOW (normal run)");
    check(enhance.context().fifo_ovf == 1U,
          "IspIrqWorker: injected FIFO_OVERFLOW counted once (frame dropped)");
    check(mipi.context().stream_errs == 1U
              && mipi.context().tx_done_count == mipi.context().frames_received,
          "MipiIrqWorker: injected stream error counted once, FSM unaffected");
    check(winhost.context().error_interrupts == 1U
              && usb_dma.error_interrupts == 1U,
          "UsbDmaWorker: injected error interrupt counted on both sides");
    // SoftIrq completion path (the real ISR->thread->AO shape): every raise
    // the engine made was taken by the consumer (SIGRTMIN queues per instance,
    // never coalesces), every take produced exactly one host-visible EOF, and
    // the delivered count reconciles with the WRAPE framing side.
#ifndef ISP_DEMO_USE_RTT
    check(usb_dma.softirq_delivered() == usb_dma.softirq_raises()
              && usb_dma.softirq_raises()
                     == wrape.context().frames_framed,
          "SoftIrq: raises == takes == framed frames (zero-loss ISR path)");
    check(usb_dma.softirq_delivered()
              == winhost.context().frames_received,
          "SoftIrq: every take delivered exactly one host EOF");
#else
    check(usb_dma.transactions_done > 0U,
          "USB DMA engine shipped its bulk transactions (direct path)");
#endif
    // Runtime reconfiguration: fault injection recovered, clean pass committed.
    check(recfg.context().recfgs_committed == 1U, "reconfig: exactly one commit");
    check(recfg.context().recfgs_failed == 3U,
          "reconfig: X4 precheck reject + DMO full-rebuild reject + X2 fault rollback");
    check(recfg.context().dmo_rejects == 1U,
          "reconfig: DMO partial-reconfig rejected at precheck (full rebuild)");
    check(recfg.context().observed_frame_bytes
              == recfg.context().active_geom.bytes_per_frame(),
          "reconfig: committed frame matches authority geometry");
    check(recfg.context().layout_version == 3U, "reconfig: layout_version advanced");
    // Quiesce mechanisms: event-driven constant vs poll scaling.
    check(videofsm.context().sout_ioctls == 3U, "SOUT quiesce: 3 ioctls (event)");
    check(videofsm.context().fmt_ioctls > videofsm.context().sout_ioctls,
          "FMT888 quiesce: poll costs more ioctls than event path");
    // Stale address cache: version guard killed the old record.
    {
        uint32_t addr = 0U;
        check(!g_addr_cache.query(addr), "stale address record invalidated");
    }
    // Register cache: repush replayed the STALE shadow over the tuned hw
    // (0xBEEF forked by bare bypass, then overwritten by the stale 0x1002).
    check(hw_before_repush == 0xBEEFU,
          "scenario A: bare bypass forked the tuned hw value");
    check(hw_after_repush == 0x1002U && hw_after_repush != hw_before_repush,
          "scenario A: repush backflow overwrote tuned hw");
    // Environment: cumulative drift exists (scenario A already forked it);
    // this asserts the bug was reproduced, not that it was fixed.
    check(drift_before_fix > 0U,
          "scenario B env: bare-bypass drift was observed (cumulative)");
    // Fix: the paired guard resynced, so NO new drift was introduced.
    check(drift_after_fix - drift_before_fix == 0U,
          "scenario B fix: paired bypass introduces zero new drift");
    // …and the paired guard restored agreement.
    {
        bool agree = true;
        for (uint16_t r = 0; r < kRegCount; ++r) {
            if (regs.st.shadow[r] != regs.st.hardware[r]) { agree = false; }
        }
        check(agree, "scenario B: paired bypass left zero drift");
    }
    // Freeze-window sync refused during cache_only, committed after.
    check(sync_refused, "scenario C: sync during freeze is refused");
    check(regs.syncs == 1U, "scenario C: exactly one sync commit");
    check(!regs.cache_dirty, "scenario C: dirty set fully flushed");
    // Mirror transform authority: corners swap and re-normalize.
    {
        const DisplayMirror mirror{true, 640U};
        const DetRect box{100U, 50U, 200U, 150U};
        const DetRect t = transform(box, mirror);
        check(t.x0 == 439U && t.x1 == 539U,
              "scenario D: mirror transform maps [100,200]->[439,539]");
    }
    // Bit-field layer: the field RMW touches ONLY the named field; the
    // adjacent lanes in the same register word survive untouched. Same
    // three-flag protocol as write() — same shadow/hardware coherency,
    // same cache_only dirty tracking, same bypass counter.
    check(SoutCtrlOpcode::read(bf_sout_after) == kSoutOpcodeRecfg
              && SoutCtrlMode::read(bf_sout_after) == bf_sout_mode_before
              && SoutCtrlEnable::read(bf_sout_after) == kSoutEnableOn,
          "scenario E: SOUT opcode RMW left mode/enable fields untouched");
    check(AiCtrlMagx::read(bf_ai_after) == 2U
              && AiCtrlBypass::read(bf_ai_after) == bf_ai_bypass_before
              && bf_sout_after == regs.st.hardware[kRegSoutCtrl]
              && bf_ai_after == regs.st.hardware[kRegAiCtrl],
          "scenario E: AI magx RMW left bypass untouched; shadow==hardware");
    // Recfg path: rcEnterApply re-issued the SOUT opcode through the field
    // view; mode[8:9] (the FSM-owned stream posture) must have survived
    // every transaction (pass-2 fault + pass-3 clean both run APPLY).
    check(SoutCtrlOpcode::read(g_sout_ctrl) == kSoutOpcodeRecfg
              && SoutCtrlMode::read(g_sout_ctrl) == kSoutModeActive
              && SoutCtrlEnable::read(g_sout_ctrl) == kSoutEnableOn,
          "recfg path: SOUT_CTRL opcode re-issued via BitFieldView, "
          "mode/enable lanes preserved across transactions");
    // Display anomalies: symptom reproduced AND fix verified.
    check(garbled_pairs > 0U, "garbled: width mismatch corrupts pixels");
    check(garbled_fixed == 0U, "garbled: shared width authority is lossless");
    check(drops_normal == 0U, "dropped: steady latency never drops");
    check(drops_spike > 0U, "dropped: latency spike breaks the window");
    check(drops_deep == 0U, "dropped (fix): deep window never drops");
    check(raced_bytes != target_bytes,
          "flicker: un-quiesced restart carries old geometry");
    check(clean_bytes == target_bytes,
          "flicker: quiesced restart aligns the first frame");

    std::printf("RESULT: %s (fails=%u)\n", (0U == fails) ? "ALL PASS" : "FAILURES",
                static_cast<unsigned>(fails));
    return (0U == fails) ? 0 : 1;
}
