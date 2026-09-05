/*
 * ===========================================================================
 * isp_chain.hpp —— ISP 节点处理链（增益/融合/Enhance/TPD + 中断/DMA）
 * ===========================================================================
 * 功能描述
 *   - GainNodeBase<Policy>：low/high gain 节点（CRTP + stateless Policy），
 *     接收 kFrameIrscOut 后 DdrCtx 写像素 + 延迟模拟（kLat.low_gain_us /
 *     kLat.high_gain_us）→ 发 kLowGainDone / kHighGainDone。
 *   - HlFuseAO：fan-in，等匹配 frame_id 的 low+high 完成后发 kHlFused，并
 *     fan-out 同时扇到 enhance 节点（PIC 路径）和 tpd 链（TEMP 路径）。
 *   - EnhanceAO / TempChainAO（FusedNode CRTP）：ISP 节点 AO，请求
 *     IspIrqWorker 模拟 ISP 节点完成 IRQ（kIspNodeDone）后发
 *     kEnhanceDonePic / kEnhanceDoneTemp；FIFO_OVERFLOW 故障注入路径。
 *   - IspIrqWorker / SoutDmaWorker / MipiIrqWorker：非 AO 中断/DMA worker
 *     （深度 2/3/2）。IspIrqWorker 供 enhance/tpd 节点完成，SoutDmaWorker
 *     供 VideoPackAO SOUT 写回，MipiIrqWorker 供 MipiSinkAo CSI TX 完成 +
 *     stream error 中断。
 *   对应 RS500 module/isp（ispSW 节点调度 + 节点完成中断 / FIFO_OVERFLOW）与
 *   module/video 的 SOUT writeback / MIPI CSI TX 完成中断。
 *
 * 与其他文件的关系
 *   - 上游：IrscWorker(sensor_irsc) 的 kFrameIrscOut → gain AO；hl_fuse 等
 *     low/high done；enhance/tpd 等 hl_fuse done。include common.hpp 与
 *     sensor_irsc.hpp（IrscTrait/PoolT/Rt 共享）。
 *   - 下游：kEnhanceDonePic / kEnhanceDoneTemp 送 [video_stream] VideoPackAo；
 *     SoutDmaWorker 的 kSoutDone 送 VideoPackAo；MipiIrqWorker 的 kMipiTxDone
 *     送 [output_itf] MipiSinkAo；IspIrqWorker 的 kIspFifoOvf 回到 enhance/tpd
 *     本 AO（记数 + 退避，不改 FSM）。
 *   - 依赖：common.hpp（DdrCtx/Sig/g_pal）+ sensor_irsc.hpp（DemoWorkerBase）。
 *
 * 文字图（处理链视角）
 * ┌──────────────────────────────────────────────────────────────────┐
 * │ 例子系统数据流（→事件信号  ⇒DDR/黑板数据）                        │
 * │                                                                  │
 * │  [sensor_irsc]──kFrameIrscOut──▶[isp_chain]──kHlFused/Done──▶   │
 * │                              (本文件：增益/融合/Enhance/TPD      │
 * │                                + IspIrqWorker/SoutDma/MipiIrq)  │
 * │                                  [video_stream]──kPicPacked──▶  │
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
#include "sensor_irsc.hpp"

namespace isp_demo {

// ---------------------------------------------------------------------------
// IspIrqWorker: ISP node processing-done interrupt (RS500 isp module ispSW
// thread). The enhance/TPD node AOs complete the data-plane transform on the
// Dispatcher thread, then ask the ISP hardware for its node-done IRQ; the
// worker simulates the interrupt latency and raises kIspNodeDone back into
// the requesting AO, which only then hands the frame downstream — the real
// driver's "HW node complete -> callback -> release buffer" shape.
// ---------------------------------------------------------------------------
// Worker job payloads are (source id, frame id) pairs: the completion event
// carries BOTH back, so the receiving AO can match the notification against
// the frame it is actually waiting on — interleaving-tolerant by construction
// (a stale completion is detected and counted, never mis-applied).
struct NodeIrqJob {
    uint16_t node_id{0U};
    uint32_t frame_id{0U};
};
static_assert(std::is_trivially_copyable<NodeIrqJob>::value,
              "NodeIrqJob is placement-new'd into the worker ring slot");

// FIFO_OVERFLOW error job: same channel, one-shot fault injection (the
// ISP stream FIFO overflowed a node's input — RS500视频流处理与输出架构.md
// §4.3). Normal runs never enqueue one (the demo asserts fifo_ovf == 0).
static_assert(std::is_trivially_copyable<bool>::value, "trivial by definition");

struct IspIrqWorker : CompletionWorkerBase<IspIrqWorker, NodeIrqJob, 2U> {
    TargetId reply_to{};
    uint32_t completion_rejects{0U};
    static constexpr const char* name() noexcept { return "isp_irq"; }
    // Two instances share this type (enhance + TPD): instance_id overrides
    // the static id in start() so per-job trace records stay distinguishable.
    static constexpr uint16_t kWorkerId = 0U;
    uint16_t instance_id{kWorkerId};   // kEvtWorkerExec arg0 (per instance)

    bool start(PoolT* p, Rt* r, TargetId node_ao,
               uint16_t trace_id = kWorkerId)
    {
        reply_to = node_ao;
        instance_id = trace_id;
        return CompletionWorkerBase::start(p, r);
    }

    void execute_job(const NodeIrqJob& j)
    {
        /* 阻塞模拟（保留真睡）：ISP 节点处理完成中断的延迟，发生在
           IspIrqWorker 自己的线程，不阻塞 AO 事件循环；完成事件的
           真实异步到达被 irq_done_count/irq_stale 断言观察 */
        g_pal->sleep_us(50U);        // node-done interrupt latency
        Layout* done = allocate_completion(static_cast<uint16_t>(Sig::kIspNodeDone));
        if (nullptr != done) {
            done->meta.cmd_arg = j.node_id;
            done->meta.frame_id = j.frame_id;
            submit_completion(reply_to, done);
        }
    }

    // FIFO_OVERFLOW injection: a single error interrupt back into the node AO
    // (worker -> AO, the error-signal contract of §4.3). The AO counts the
    // error and drops the frame; nothing here changes any FSM.
    void inject_fifo_overflow(uint16_t node_id, uint32_t frame_id)
    {
        Layout* ovf = pool->alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(Sig::kIspFifoOvf));
        if (nullptr != ovf) {
            ovf->meta.cmd_arg = node_id;
            ovf->meta.frame_id = frame_id;
            rt->coordinator().submit_from_task(reply_to, &ovf->event,
                                               {false, false});
        }
    }
};

// ---------------------------------------------------------------------------
// SoutDmaWorker: stream_out_dma writeback (second-level DMA, SOUT->DDR, x3
// rings). The VideoPack AO completes repack and queues the SOUT writeback;
// the worker simulates the DMA latency, then raises kSoutDone so the AO
// releases the frame to its sink (USB WRAPE / MIPI).
// ---------------------------------------------------------------------------
// Depth 3 mirrors RS500's stream_out_dma x3 ring: one writeback in flight,
// two buffered — scheduler jitter (ms-scale on a host) must not drop frames
// on a 33ms-paced stream.
struct SoutJob {
    uint16_t path{0U};
    uint32_t frame_id{0U};
};
static_assert(std::is_trivially_copyable<SoutJob>::value,
              "SoutJob is placement-new'd into the worker ring slot");

struct SoutDmaWorker : CompletionWorkerBase<SoutDmaWorker, SoutJob, 3U> {
    TargetId reply_to{};
    uint32_t completion_rejects{0U};
    static constexpr const char* name() noexcept { return "sout_dma"; }
    static constexpr uint16_t kWorkerId = 1U;
    uint16_t instance_id{kWorkerId};

    bool start(PoolT* p, Rt* r, TargetId pack_ao)
    {
        reply_to = pack_ao;
        return CompletionWorkerBase::start(p, r);
    }

    void execute_job(const SoutJob& j)
    {
        /* 阻塞模拟（保留真睡）：SOUT 写回 DMA 引擎延迟，发生在
           SoutDmaWorker 自己的线程，不阻塞 AO 事件循环；x3 环深度
           吸收的调度抖动语义依赖真实时延 */
        g_pal->sleep_us(60U);        // SOUT writeback engine latency
        Layout* done = allocate_completion(static_cast<uint16_t>(Sig::kSoutDone));
        if (nullptr != done) {
            done->meta.cmd_arg = j.path;
            done->meta.frame_id = j.frame_id;
            submit_completion(reply_to, done);
        }
    }
};

// ---------------------------------------------------------------------------
// MipiIrqWorker: MIPI CSI TX completion interrupt (RS500 MIPI DSI/CSI
// outflow). The MIPI sink AO hands a frame to the TX line buffer, the worker
// simulates the line-transfer latency, then raises kMipiTxDone back into the
// sink AO — the "frame shipped over the lane" callback.
// ---------------------------------------------------------------------------
struct MipiIrqWorker : CompletionWorkerBase<MipiIrqWorker, uint16_t, 2U> {
    TargetId reply_to{};
    uint32_t completion_rejects{0U};
    static constexpr const char* name() noexcept { return "mipi_irq"; }
    static constexpr uint16_t kWorkerId = 2U;
    uint16_t instance_id{kWorkerId};

    bool start(PoolT* p, Rt* r, TargetId sink_ao)
    {
        reply_to = sink_ao;
        return CompletionWorkerBase::start(p, r);
    }

    void execute_job(const uint16_t& frame_id)
    {
        /* 阻塞模拟（保留真睡）：MIPI CSI 线缓冲传输时间，发生在 MipiIrqWorker
           自己的线程，不阻塞 AO 事件循环；kMipiTxLatencyUs 同时被
           onMipiTxDone 记账，tx_done_count 断言观察真实异步完成 */
        g_pal->sleep_us(kMipiTxLatencyUs);   // CSI line-buffer transfer
        Layout* done = allocate_completion(static_cast<uint16_t>(Sig::kMipiTxDone));
        if (nullptr != done) {
            done->meta.frame_id = frame_id;
            submit_completion(reply_to, done);
        }
    }

    // CSI stream-error injection (RS500_MIPI图像输出原理.md §9): the error
    // interrupt flows back to the sink AO, which counts it — deliberately
    // WITHOUT any FSM transition (the manual's "interrupt does not
    // auto-change the Stream FSM" constraint). Normal runs never fire it.
    void inject_stream_error(uint32_t frame_id)
    {
        Layout* err = pool->alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(Sig::kMipiStreamErr));
        if (nullptr != err) {
            err->meta.frame_id = frame_id;
            rt->coordinator().submit_from_task(reply_to, &err->event,
                                               {false, false});
        }
    }
};


// ---------------------------------------------------------------------------
// Gain-chain AOs.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// CRTP chain-node base + compile-time transform policies.
//
// ChainNodeBase factors the per-node skeleton (DN synthesis into DDR, latency
// accounting, region write, done-event forwarding) once; the Derived node
// supplies its policy via CRTP. Zero virtual dispatch: the transform resolves
// at compile time through static_cast<Derived*>(this).
//
// Policies are stateless structs with a static apply() — the classic
// compile-time strategy pattern, the same idiom std::allocator / std::hash
// use for customization.
// ---------------------------------------------------------------------------
struct LowGainPolicy {
    static constexpr uint8_t apply(uint8_t dn) noexcept
    {
        return static_cast<uint8_t>((dn * 3U / 4U) & 0xFFu);   // NUC-like low gain
    }
};
struct HighGainPolicy {
    static constexpr uint8_t apply(uint8_t dn) noexcept
    {
        return static_cast<uint8_t>((dn * 5U / 4U) & 0xFFu);   // brighter high gain
    }
};

struct GainNodeCtx {
    PoolT*    pool{nullptr};
    Rt*       rt{nullptr};
    DdrCtx*   ddr{nullptr};
    TargetId  owner_target{};
    DdrId     out_region{DdrId::kDdrLowGain};
    uint16_t  done_signal{0U};
    const char* label{""};
    uint32_t  simulated_us{0U};
    uint32_t  frames_handled{0U};
    uint32_t  total_us{0U};
};

// CRTP base: Derived must expose `static constexpr Policy policy()`-free
// access — we thread the policy as the second template parameter instead, so
// Derived itself stays a trivial context holder.
template <typename Policy>
struct GainNodeBase {
    // Common frame path: DN -> DDR -> policy transform -> DDR -> done event.
    // Returns false when the pool is exhausted (event dropped).
    [[nodiscard]] static bool process(GainNodeCtx& ctx, const Event& evt)
    {
        const Layout& e = *reinterpret_cast<const Layout*>(&evt);

        // Synthesize the DN frame into its deterministic DDR slot (both gain
        // chains write identical bytes: idempotent second write).
        std::array<uint8_t, kSlotPayloadBytes> dn{};
        fill_dn(dn.data(), e.meta.frame_id);
        static_cast<void>(
            ctx.ddr->write(DdrId::kDdrDn, e.meta.frame_id, dn.data()));

        std::array<uint8_t, kSlotPayloadBytes> in{};
        const bool dn_ok =
            ctx.ddr->read(DdrId::kDdrDn, dn_slot_of(e.meta.frame_id),
                          e.meta.frame_id, in.data());

        /* 阻塞记账（原真睡 simulated_us）：RS500 增益链驱动的逐帧同步处理
           耗时。latency 输出只需模拟值本身（usleep 实测还有调度抖动），
           无真实时间窗口断言依赖，故记 total_us 不真睡——真睡会占住
           Dispatcher 线程，拖慢所有 AO 的事件派发（30 帧全链路的放大点） */
        ctx.total_us += ctx.simulated_us;
        ++ctx.frames_handled;

        // Policy transform (compile-time dispatch; if constexpr keeps the
        // degraded path branchless when input is guaranteed present).
        std::array<uint8_t, kSlotPayloadBytes> out{};
        if constexpr (true) {
            for (uint16_t i = 0; i < kSlotPayloadBytes; ++i) {
                out[i] = Policy::apply(dn_ok ? in[i] : dn[i]);
            }
        }
        const uint16_t slot = ctx.ddr->write(ctx.out_region, e.meta.frame_id, out.data());

        Layout* done = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(ctx.done_signal);
        if (nullptr == done) { return false; }
        done->meta = e.meta;
        done->meta.buffer_idx = slot;
        Payload* p = reinterpret_cast<Payload*>(&done->payload[0]);
        p->ddr_slot = slot;
        p->ddr_id = static_cast<uint16_t>(ctx.out_region);
        ctx.rt->coordinator().submit_from_task(ctx.owner_target, &done->event, {false, false});
        return true;
    }
};

// Concrete nodes: policy-bound via type alias — the CRTP/strategy handshake.
using LowGainNode  = GainNodeBase<LowGainPolicy>;
using HighGainNode = GainNodeBase<HighGainPolicy>;

inline void onIrscOutLow(GainNodeCtx& ctx, const Event& evt)
{
    static_cast<void>(LowGainNode::process(ctx, evt));
}
inline void onIrscOutHigh(GainNodeCtx& ctx, const Event& evt)
{
    static_cast<void>(HighGainNode::process(ctx, evt));
}

COACT_HSM_STATES(kChainStates, GainNodeCtx, "Root", "Active");
COACT_HSM_TRANS(kLowTransitions, GainNodeCtx, Sig::kFrameIrscOut, onIrscOutLow);
COACT_HSM_TRANS(kHighTransitions, GainNodeCtx, Sig::kFrameIrscOut, onIrscOutHigh);

// ---------------------------------------------------------------------------
// HlFuseAO: fan-in. Waits for matching low+high frame_id, fuses, emits 2
// copies (PIC + TEMP paths).
// ---------------------------------------------------------------------------
struct HlCtx {
    PoolT*    pool{nullptr};
    Rt*       rt{nullptr};
    DdrCtx*   ddr{nullptr};       // shared DDR owner
    TargetId  enhance_target{};
    TargetId  tpd_target{};
    uint32_t  frames_fused{0U};
    uint32_t  total_us{0U};
    struct Slot { bool valid{false}; uint32_t frame_id{0U}; uint16_t ddr_slot{0U}; };
    Slot low{};
    Slot high{};
    void maybe_fuse();
};

inline void onLowGainDone(HlCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    ctx.low.valid = true;
    ctx.low.frame_id = e.meta.frame_id;
    ctx.low.ddr_slot = e.meta.buffer_idx;
    ctx.maybe_fuse();
}
inline void onHighGainDone(HlCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    ctx.high.valid = true;
    ctx.high.frame_id = e.meta.frame_id;
    ctx.high.ddr_slot = e.meta.buffer_idx;
    ctx.maybe_fuse();
}

inline void HlCtx::maybe_fuse()
{
    if (!low.valid || !high.valid) { return; }
    if (low.frame_id != high.frame_id) { return; }

    // Read both gain paths out of DDR, blend (weighted HL fusion). An overrun
    // on either side degrades to the last valid bytes of that slot.
    uint8_t lg[kSlotPayloadBytes], hg[kSlotPayloadBytes], fused[kSlotPayloadBytes];
    const bool lg_ok = ddr->read(DdrId::kDdrLowGain, low.ddr_slot, low.frame_id, lg);
    const bool hg_ok = ddr->read(DdrId::kDdrHighGain, high.ddr_slot, high.frame_id, hg);
    if (!lg_ok) { fill_dn(lg, low.frame_id); }
    if (!hg_ok) { fill_dn(hg, high.frame_id); }
    for (uint16_t i = 0; i < kSlotPayloadBytes; ++i) {
        // 40% low + 60% high (typical wide-dynamic-range fusion).
        fused[i] = static_cast<uint8_t>((2U * lg[i] + 3U * hg[i]) / 5U);
    }

    /* 阻塞记账（原真睡 hl_fuse_us）：HL 宽动态融合的硬件处理耗时。同增益
       链理由——latency 统计只依赖模拟值，无真实时间窗口断言，不真睡 */
    total_us += kLat.hl_fuse_us;
    ++frames_fused;
    if (frames_fused % 10U == 0U) {
        // Data-chain milestone (throttled): fan-in fused another 10 frames.
        std::printf("[hl_fuse] frames_fused=%u (last frame_id=%u)\n",
                    frames_fused, low.frame_id);
        g_log.record_from_task<LogLevel::kInfo, kEvtFrameDone>(
            0U, frames_fused, low.frame_id, 0U);
    }

    // Write fused into DDR once; both PIC (enhance) and TEMP (tpd) share it.
    const uint16_t fused_slot = ddr->write(DdrId::kDdrFused, low.frame_id, fused);

    // PIC path (enhance).
    Layout* e1 = pool->alloc_typed<Layout, Payload, kPayloadAlign>(
        static_cast<uint16_t>(Sig::kHlFused));
    if (nullptr != e1) {
        e1->meta.frame_id = low.frame_id;
        e1->meta.flags = 0U;
        e1->meta.buffer_idx = fused_slot;
        Payload* p = reinterpret_cast<Payload*>(&e1->payload[0]);
        p->control[0] = 'H'; p->control[1] = 'L'; p->control[2] = 'P';
        p->ddr_slot = fused_slot;
        p->ddr_id = static_cast<uint16_t>(DdrId::kDdrFused);
        rt->coordinator().submit_from_task(enhance_target, &e1->event,
                                           {false, false});
    }
    // TEMP path (tpd). Same fused pixels; tpd computes Y16 temp codes.
    Layout* e2 = pool->alloc_typed<Layout, Payload, kPayloadAlign>(
        static_cast<uint16_t>(Sig::kHlFused));
    if (nullptr != e2) {
        e2->meta.frame_id = low.frame_id;
        e2->meta.flags = 1U;
        e2->meta.buffer_idx = fused_slot;
        Payload* p = reinterpret_cast<Payload*>(&e2->payload[0]);
        p->control[0] = 'H'; p->control[1] = 'L'; p->control[2] = 'T';
        p->ddr_slot = fused_slot;
        p->ddr_id = static_cast<uint16_t>(DdrId::kDdrFused);
        rt->coordinator().submit_from_task(tpd_target, &e2->event,
                                           {false, false});
    }
    low = Slot{}; high = Slot{};
}

COACT_HSM_STATES(kHlStates, HlCtx, "Root", "Active");
inline const TransitionDef<HlCtx> kHlTransitions[] = {
    { 1, static_cast<uint16_t>(Sig::kLowGainDone),  1, TransitionKind::Internal, nullptr, onLowGainDone },
    { 1, static_cast<uint16_t>(Sig::kHighGainDone), 1, TransitionKind::Internal, nullptr, onHighGainDone },
};


// ---------------------------------------------------------------------------
// EnhanceAO + TempChainAO: the same CRTP/policy skeleton, this time over the
// fused region. The two nodes differ ONLY in policy + output region + done
// signal, so they collapse into FusedNodeBase<Policy> instantiations.
// ---------------------------------------------------------------------------
struct EnhancePolicy {
    static constexpr uint8_t apply(uint8_t fused) noexcept
    {
        return static_cast<uint8_t>((fused * 6U / 5U) & 0xFFu);  // gamma stretch
    }
};
struct TempPolicy {
    static constexpr uint8_t apply(uint8_t fused) noexcept
    {
        return static_cast<uint8_t>((fused / 2U + 32U) & 0xFFu); // Y16 temp code
    }
};

// Shared context for both fused-region consumers.
struct FusedNodeCtx {
    PoolT*    pool{nullptr};
    Rt*       rt{nullptr};
    DdrCtx*   ddr{nullptr};
    TargetId  owner_target{};
    DdrId     out_region{DdrId::kDdrPicOut};
    uint16_t  done_signal{0U};
    uint16_t  path_flag{0U};        // expected flags bit (0=PIC, 1=TEMP)
    uint32_t  payload_kind{1U};
    const char* label{""};
    uint32_t  simulated_us{0U};
    uint32_t  frames_handled{0U};
    uint32_t  total_us{0U};
    uint32_t  node_id{0U};          // identity for the ISP node-done channel
    uint32_t  irq_subs{0U};         // requests accepted by the channel
    uint32_t  irq_rejects{0U};      // requests dropped (queue full)
    uint32_t  irq_done_count{0U};   // node-done interrupts observed
    TargetId  downstream{};         // where the done event goes after irq
    IspIrqWorker* irq_channel{nullptr};  // async node-done interrupt source
    TargetId  self_target{};        // this AO (self-submitted drop completions)
    // Frame parking ring: the frame descriptors currently waiting for their
    // node-done interrupt. Depth 4 absorbs any dispatch interleaving (a
    // scheduler hiccup can pile frames closer than the 50us IRQ latency);
    // completions match by frame id, so overlap is tolerated, never lost.
    static constexpr uint8_t kParkDepth = 4U;
    IoMeta    parked[kParkDepth]{}; // circular parking ring
    uint8_t   park_head{0U};        // next free ring slot
    uint32_t  in_flight{0U};        // frames awaiting their interrupt
    uint32_t  irq_stale{0U};        // completions that matched no parked frame
    uint32_t  fifo_ovf{0U};         // ISP stream FIFO_OVERFLOW interrupts
    uint32_t  fifo_ovf_drops{0U};   // frames dropped by a FIFO overflow
};

template <typename Policy>
struct FusedNodeBase {
    // Phase 1 (Dispatcher thread): data-plane transform + DDR write. The
    // node work itself stays synchronous — only the completion notification
    // moves to the IRQ channel (minimal-change split: business logic stays in
    // the AO, the worker only simulates the hardware message).
    [[nodiscard]] static bool process(FusedNodeCtx& ctx, const Event& evt)
    {
        const Layout& e = *reinterpret_cast<const Layout*>(&evt);
        if ((e.meta.flags & 0x1U) != ctx.path_flag) { return true; }  // not ours

        std::array<uint8_t, kSlotPayloadBytes> fused{};
        if (!ctx.ddr->read(DdrId::kDdrFused, e.meta.buffer_idx, e.meta.frame_id,
                           fused.data())) {
            fill_dn(fused.data(), e.meta.frame_id);   // degraded seed
        }

        /* 阻塞记账（原真睡 simulated_us）：RS500 增强链（enhance/TPD 后级）
           驱动的逐帧同步处理耗时。同增益链理由，记 total_us 不真睡 */
        ctx.total_us += ctx.simulated_us;
        ++ctx.frames_handled;

        std::array<uint8_t, kSlotPayloadBytes> out{};
        for (uint16_t i = 0; i < kSlotPayloadBytes; ++i) {
            out[i] = Policy::apply(fused[i]);
        }
        const uint16_t slot = ctx.ddr->write(ctx.out_region, e.meta.frame_id, out.data());
        (void)slot;
        return true;
    }

    // Phase 1b: request the node-done IRQ. Called right after process(); the
    // AO parks the frame descriptor until the IRQ returns. The job carries
    // the frame id so the completion can be MATCHED (interleaving-tolerant).
    // A busy channel drops the frame: a self-submitted completion carries the
    // AO home without stalling (counted, never hidden — the interrupt analog
    // of a hardware FIFO overrun discarding one frame and continuing).
    static void request_irq(FusedNodeCtx& ctx, const Event& evt)
    {
        const Layout& e = *reinterpret_cast<const Layout*>(&evt);
        if ((e.meta.flags & 0x1U) != ctx.path_flag) { return; }  // not ours
        const NodeIrqJob job{static_cast<uint16_t>(ctx.node_id),
                             e.meta.frame_id};
        if (ctx.irq_channel->submit(job)) {
            ++ctx.irq_subs;
            // Park the descriptor in the ring (ownership -> IRQ window).
            ctx.parked[ctx.park_head] = e.meta;
            ctx.park_head = static_cast<uint8_t>((ctx.park_head + 1U)
                                                 % FusedNodeCtx::kParkDepth);
            ++ctx.in_flight;
        } else {
            ++ctx.irq_rejects;
            std::printf("[warn] isp_irq submit rejected (busy), frame=%u\n",
                        static_cast<unsigned>(e.meta.frame_id));
            self_complete(ctx, e.meta.frame_id);   // drop: async completion
        }
    }

    // Self-submitted completion for the drop path: the HSM is already in
    // IrqPending (the transition fired before the action ran); a synthetic
    // kIspNodeDone for THIS frame walks it home through the normal arc.
    static void self_complete(FusedNodeCtx& ctx, uint32_t frame_id)
    {
        Layout* e = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(Sig::kIspNodeDone));
        if (nullptr != e) {
            e->meta.cmd_arg = static_cast<uint16_t>(ctx.node_id);
            e->meta.frame_id = frame_id;
            ctx.rt->coordinator().submit_from_task(ctx.self_target, &e->event,
                                                   {false, false});
        }
    }

    // Phase 2 (IRQ completion, Dispatcher thread): the node-done interrupt
    // arrived — scan the parking ring for the frame it names, then release
    // that frame downstream (kEnhanceDone/kTempChainDone). Completions are
    // matched by frame id, so dispatch interleaving cannot mis-apply one; an
    // unknown id (already released / foreign) is counted as stale.
    static void complete_irq(FusedNodeCtx& ctx, const Event& evt)
    {
        const Layout& e = *reinterpret_cast<const Layout*>(&evt);
        IoMeta* hit = nullptr;
        for (uint8_t i = 0U; i < FusedNodeCtx::kParkDepth; ++i) {
            if (ctx.parked[i].frame_id == e.meta.frame_id) {
                hit = &ctx.parked[i];
                break;
            }
        }
        if (nullptr == hit) {
            ++ctx.irq_stale;              // no parked frame matches
            return;
        }
        Layout* done = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(ctx.done_signal);
        if (nullptr == done) {
            *hit = IoMeta{};
            if (ctx.in_flight > 0U) { --ctx.in_flight; }
            ++ctx.irq_rejects;
            return;
        }
        // Ownership transfer: the parked descriptor moves into the outgoing
        // event; the ring slot is reset in the same step (std::exchange).
        done->meta = std::exchange(*hit, IoMeta{});
        done->meta.payload_kind = ctx.payload_kind;
        Payload* p = reinterpret_cast<Payload*>(&done->payload[0]);
        p->ddr_slot = done->meta.buffer_idx;
        p->ddr_id = static_cast<uint16_t>(ctx.out_region);
        ctx.rt->coordinator().submit_from_task(ctx.downstream, &done->event,
                                               {false, false});
        if (ctx.in_flight > 0U) { --ctx.in_flight; }
        ++ctx.irq_done_count;
    }

    // Convenience wrapper for the transition actions (both phases per event).
    static void handle(FusedNodeCtx& ctx, const Event& evt)
    {
        static_cast<void>(process(ctx, evt));
        request_irq(ctx, evt);
    }

    // FIFO_OVERFLOW (RS500视频流处理与输出架构.md §4.3): the node's input
    // FIFO overflowed — the frame it names is DROPPED (removed from the
    // parking ring; it will never see its node-done interrupt), the error is
    // counted, and the AO keeps running. No crash, no FSM posture change
    // beyond the normal Internal arc; only an injection scenario ever calls it.
    static void handle_fifo_ovf(FusedNodeCtx& ctx, const Event& evt)
    {
        const Layout& e = *reinterpret_cast<const Layout*>(&evt);
        ++ctx.fifo_ovf;
        for (uint8_t i = 0U; i < FusedNodeCtx::kParkDepth; ++i) {
            if (ctx.parked[i].frame_id == e.meta.frame_id) {
                ctx.parked[i] = IoMeta{};
                if (ctx.in_flight > 0U) { --ctx.in_flight; }
                break;
            }
        }
        ++ctx.fifo_ovf_drops;
        std::printf("[warn] isp FIFO_OVERFLOW: node=%u frame=%u dropped "
                    "(error counted, pipeline continues)\n",
                    static_cast<unsigned>(e.meta.cmd_arg),
                    static_cast<unsigned>(e.meta.frame_id));
        g_log.record_from_task<LogLevel::kWarn, kEvtWorkerStat>(
            static_cast<uint32_t>(e.meta.cmd_arg), ctx.fifo_ovf,
            ctx.fifo_ovf_drops, 0U);
    }
};

using EnhanceNode = FusedNodeBase<EnhancePolicy>;
using TempNode    = FusedNodeBase<TempPolicy>;

// Two HSM states per node AO: ACTIVE (processing, IRQ window parked) and
// IRQ_PENDING (data done, waiting for the node-done interrupt). The state
// transition IS the documentation of the async split — the AO is literally
// in "waiting for hardware" between submit and complete.
inline constexpr int8_t kFusedRoot = 0;
inline constexpr int8_t kFusedActive = 1;
inline constexpr int8_t kFusedIrqPending = 2;

inline const StateDef<FusedNodeCtx> kFusedStates[] = {
    { -1,               nullptr, nullptr,              "Root" },
    { kFusedRoot,       nullptr, nullptr,              "Active" },
    { kFusedRoot,       nullptr, nullptr,              "IrqPending" },
};

inline void onHlFusedEnhance(FusedNodeCtx& ctx, const Event& evt)
{
    EnhanceNode::handle(ctx, evt);
}
inline void onHlFusedTpd(FusedNodeCtx& ctx, const Event& evt)
{
    TempNode::handle(ctx, evt);
}
inline void onIspNodeDoneEnhance(FusedNodeCtx& ctx, const Event& evt)
{
    EnhanceNode::complete_irq(ctx, evt);
    HsmTrace::transition("enhance", "IrqPending",
                         static_cast<uint16_t>(Sig::kIspNodeDone), "Active");
}
inline void onIspNodeDoneTpd(FusedNodeCtx& ctx, const Event& evt)
{
    TempNode::complete_irq(ctx, evt);
    HsmTrace::transition("tpd", "IrqPending",
                         static_cast<uint16_t>(Sig::kIspNodeDone), "Active");
}
// FIFO_OVERFLOW: error counted, frame dropped, NO state change (Internal
// arcs in both node tables below) — the pipeline degrades, never crashes.
inline void onIspFifoOvfEnhance(FusedNodeCtx& ctx, const Event& evt)
{
    EnhanceNode::handle_fifo_ovf(ctx, evt);
}
inline void onIspFifoOvfTpd(FusedNodeCtx& ctx, const Event& evt)
{
    TempNode::handle_fifo_ovf(ctx, evt);
}

// Guard: only the path's own kHlFused copy fires the split (each AO receives
// both copies; the flags bit discriminates — one shared table per path).
[[nodiscard]] inline bool is_pic_path(const FusedNodeCtx&, const Event& evt) noexcept
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    return 0U == (e.meta.flags & 0x1U);
}
[[nodiscard]] inline bool is_temp_path(const FusedNodeCtx&, const Event& evt) noexcept
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    return 0U != (e.meta.flags & 0x1U);
}

// Complete (state, signal) coverage — the HSM can never silently swallow an
// event: overlapping inputs (a frame arriving while one is parked) are
// processed the same way in either state; late completions are matched and
// released in either state. The state distinction remains an OBSERVABLE
// "interrupt window open" indicator (the common case: one frame in flight).
inline const TransitionDef<FusedNodeCtx> kEnhanceTransitions[] = {
    // kHlFused (ours): process + park + submit IRQ, from either state.
    { kFusedActive,    static_cast<uint16_t>(Sig::kHlFused), kFusedIrqPending,
      TransitionKind::External, is_pic_path, onHlFusedEnhance },
    { kFusedIrqPending, static_cast<uint16_t>(Sig::kHlFused), kFusedIrqPending,
      TransitionKind::Internal, is_pic_path, onHlFusedEnhance },
    // kHlFused (foreign copy): ignored, from either state.
    { kFusedActive,    static_cast<uint16_t>(Sig::kHlFused), kFusedActive,
      TransitionKind::Internal, is_temp_path, nullptr },
    { kFusedIrqPending, static_cast<uint16_t>(Sig::kHlFused), kFusedIrqPending,
      TransitionKind::Internal, is_temp_path, nullptr },
    // kIspNodeDone: match + release, from either state.
    { kFusedIrqPending, static_cast<uint16_t>(Sig::kIspNodeDone), kFusedActive,
      TransitionKind::External, nullptr, onIspNodeDoneEnhance },
    { kFusedActive,    static_cast<uint16_t>(Sig::kIspNodeDone), kFusedActive,
      TransitionKind::Internal, nullptr, onIspNodeDoneEnhance },
    // kIspFifoOvf (§4.3): error interrupt — counted + frame dropped, Internal
    // from either state (no posture change; error path only).
    { kFusedIrqPending, static_cast<uint16_t>(Sig::kIspFifoOvf), kFusedIrqPending,
      TransitionKind::Internal, nullptr, onIspFifoOvfEnhance },
    { kFusedActive,    static_cast<uint16_t>(Sig::kIspFifoOvf), kFusedActive,
      TransitionKind::Internal, nullptr, onIspFifoOvfEnhance },
};

inline const TransitionDef<FusedNodeCtx> kTempTransitions[] = {
    { kFusedActive,    static_cast<uint16_t>(Sig::kHlFused), kFusedIrqPending,
      TransitionKind::External, is_temp_path, onHlFusedTpd },
    { kFusedIrqPending, static_cast<uint16_t>(Sig::kHlFused), kFusedIrqPending,
      TransitionKind::Internal, is_temp_path, onHlFusedTpd },
    { kFusedActive,    static_cast<uint16_t>(Sig::kHlFused), kFusedActive,
      TransitionKind::Internal, is_pic_path, nullptr },
    { kFusedIrqPending, static_cast<uint16_t>(Sig::kHlFused), kFusedIrqPending,
      TransitionKind::Internal, is_pic_path, nullptr },
    { kFusedIrqPending, static_cast<uint16_t>(Sig::kIspNodeDone), kFusedActive,
      TransitionKind::External, nullptr, onIspNodeDoneTpd },
    { kFusedActive,    static_cast<uint16_t>(Sig::kIspNodeDone), kFusedActive,
      TransitionKind::Internal, nullptr, onIspNodeDoneTpd },
    // kIspFifoOvf (§4.3): error interrupt — counted + frame dropped, Internal
    // from either state (no posture change; error path only).
    { kFusedIrqPending, static_cast<uint16_t>(Sig::kIspFifoOvf), kFusedIrqPending,
      TransitionKind::Internal, nullptr, onIspFifoOvfTpd },
    { kFusedActive,    static_cast<uint16_t>(Sig::kIspFifoOvf), kFusedActive,
      TransitionKind::Internal, nullptr, onIspFifoOvfTpd },
};


// AO aliases: gain chains + HL fuse + enhance/TPD (isp_chain module).
using LowGainAo  = coact::Ao<GainNodeCtx, Hsm<GainNodeCtx>, AoTrait<60>>;
using HighGainAo = coact::Ao<GainNodeCtx, Hsm<GainNodeCtx>, AoTrait<59>>;
using HlFuseAo   = coact::Ao<HlCtx, Hsm<HlCtx>, AoTrait<55>>;
using EnhanceAo  = coact::Ao<FusedNodeCtx, Hsm<FusedNodeCtx>, AoTrait<50>>;
using TempChainAo = coact::Ao<FusedNodeCtx, Hsm<FusedNodeCtx>, AoTrait<49>>;


}  // namespace isp_demo
