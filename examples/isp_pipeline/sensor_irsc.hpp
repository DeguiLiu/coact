/*
 * ===========================================================================
 * sensor_irsc.hpp —— IRSC 传感器输入层（产帧 + 驱动命令 + worker 骨架）
 * ===========================================================================
 * 功能描述
 *   - IrscWorker：PAL 线程产帧器，按 period_us（默认 30fps, 33333us）发
 *     kFrameIrscOut，模拟 drv_irsc 帧中断时序；不占 Dispatcher 线程。
 *   - IrscDriverAo：传感器驱动命令 AO，接收 kIrscCmd（init/start/ctrl/
 *     output_enable 四步），经 CmdDmaWorker 模拟 vdcmd rt_device_control
 *     寄存器写路径，回 kIrscDmaDone 步进，完成后上报 kIrscReady。
 *   - WorkerBase / DemoWorkerBase：CRTP 非 AO worker 骨架（定深环形队列 +
 *     drain-on-stop + PAL Mutex/Cond），全部硬件行为 worker 的公共底座。
 *   - UsbDmaWorker：USB Bulk DMA 引擎（T37 §7.6.5），搬完帧后向 WinHostAo
 *     发 kFrameEof；故障时发 kUsbErrInt。
 *   对应 RS500 module/sensor_input（sensor_input_com_input_irsc.c、drv_irsc
 *   寄存器序列）与 vdcmd 寄存器写通道。
 *
 * 与其他文件的关系
 *   - 上游：main.cpp 发 kIrscCmd 启动 IrscDriverAo；IrscWorker 产帧节拍。
 *   - 下游：IrscWorker 的 kFrameIrscOut 扇出到 [isp_chain] 的 low/high
 *     gain AO；IrscDriver 的 kIrscReady 汇到 [output_itf] 的 OrchestratorAo；
 *     UsbDmaWorker 的 kFrameEof/kUsbErrInt 送 [output_itf] 的 WinHostAo。
 *   - 依赖：include common.hpp（Sig/DemoPal/g_pal/PoolT/Rt/IrscTrait）；worker
 *     经 g_pal->thread_create/join、sleep_us、monotonic_ns 运行；CmdDmaWorker
 *     单槽忙时 honest-reject（rejected 计数，不阻塞 AO 事件循环）。
 *
 * 文字图（产帧视角）
 * ┌──────────────────────────────────────────────────────────────────┐
 * │ 例子系统数据流（→事件信号  ⇒DDR/黑板数据）                        │
 * │                                                                  │
 * │  [sensor_irsc]──kFrameIrscOut──▶[isp_chain]──kHlFused/Done──▶   │
 * │  (本文件)                        [video_stream]──kPicPacked──▶  │
 * │    │                             [output_itf]──kFrameEof──▶     │
 * │    └─IrscDriver─kIrscReady─▶[orch] [winhost 观察者]             │
 * │     UsbDmaWorker(本文件) ──kFrameEof──▶ WinHostAo(output_itf)   │
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

namespace isp_demo {

// ---------------------------------------------------------------------------
// IRSC producer: PAL worker thread, mimics drv_irsc timing. Communicates via
// events. Thread lifecycle goes through DemoPal::thread_create/join (POSIX:
// pthread; RT-Thread: the PAL's fixed static thread table — zero heap).
// ---------------------------------------------------------------------------
struct IrscWorker {
    DemoPal::ThreadHandle thread_{};
    PoolT*          pool{nullptr};
    Rt*             rt{nullptr};
    TargetId        low_target{};
    TargetId        high_target{};
    std::atomic<bool> running{false};
    uint32_t        period_us{33333U};

    void start(PoolT* p, Rt* r, TargetId low, TargetId high, uint32_t fps)
    {
        pool = p;
        rt = r;
        low_target = low;
        high_target = high;
        period_us = (fps == 0U) ? 33333U : (1000000U / fps);
        running.store(true);
        std::printf("[worker] irsc started (frames=%u, period=%u us)\n",
                    kFrameCount, period_us);
        g_pal->thread_create(thread_, &IrscWorker::tramp, this);
    }
    void stop()
    {
        running.store(false);
        g_pal->thread_join(thread_);
    }
private:
    static void tramp(void* arg) { static_cast<IrscWorker*>(arg)->run(); }
    void run()
    {
        for (uint32_t f = 0; f < kFrameCount && running.load(); ++f) {
            // Relative pacing: sleep one full frame period per iteration. A
            // sensor produces frames on its own clock regardless of how far
            // behind the pipeline is — and keeps the in-flight span shallow
            // so the DMA ring never legitimately overruns.
            /* 阻塞模拟（保留真睡）：IRSC 传感器产帧节拍 + 增益模式判定的
               寄存器耗时，合并为一次睡眠。发生在 IrscWorker 自己的线程，
               不阻塞 AO 事件循环；且 30fps 节拍是封帧时序断言（frame pacing）
               的真实时间依赖，虚拟时钟无法推进真实线程，故必须真睡 */
            g_pal->sleep_us(period_us + (kLat.irsc_setup_us / 2U));
            const uint64_t emit_ns = monotonic_ns();

            for (uint8_t ch = 0; ch < 2U; ++ch) {
                Layout* e = pool->alloc_typed<Layout, Payload, kPayloadAlign>(
                    static_cast<uint16_t>(Sig::kFrameIrscOut));
                if (nullptr == e) { break; }
                e->meta.frame_id = f;
                e->meta.timestamp_ns = static_cast<uint32_t>(emit_ns & 0xFFFFFFFFu);
                e->meta.payload_kind = 0U;
                Payload* p = reinterpret_cast<Payload*>(&e->payload[0]);
                p->control[0] = 'D'; p->control[1] = 'N';
                p->control[2] = static_cast<uint8_t>((f >> 8U) & 0xFFu);
                p->control[3] = static_cast<uint8_t>(f & 0xFFu);
                p->ddr_id = DdrId::kDdrDn;      // writer fills actual pixels
                const TargetId dst = (ch == 0U) ? low_target : high_target;
                rt->coordinator().submit_from_task(dst, &e->event, {false, false});
            }
        }
        std::printf("[irsc] emitted %u DN frames @ %u us\n", kFrameCount, period_us);
        g_log.record_from_task<LogLevel::kInfo, kEvtFrameEmitted>(0U, kFrameCount, period_us);
    }
};

constexpr uint16_t kX1Width  = 640U;
constexpr uint16_t kX1Height = 512U;
constexpr uint16_t kOutWidth  = 1280U;   // the fixed external UVC profile
constexpr uint16_t kOutHeight = 1024U;
constexpr uint32_t kX1FrameBytes =
    static_cast<uint32_t>(kX1Width) * kX1Height * 2U;    // 655360 (T37 evidence)
constexpr uint32_t kOutFrameBytes =
    static_cast<uint32_t>(kOutWidth) * kOutHeight * 2U;  // 2621440
constexpr uint32_t kStreamVldNum = 1280U * 1024U;        // 1310720
constexpr uint16_t kZoomStepIdentity = 256U;             // 1:1 pass-through
constexpr uint16_t kZoomStep2x = 128U;                   // 2x upscale
constexpr uint32_t kBulkXsfBytes = 16384U;               // bulk transaction
                                                         // granularity (NOT
                                                         // the frame length)
static_assert(kX1FrameBytes == 655360U, "T37: premature-EOF boundary is 640x512x2B");
static_assert(kOutFrameBytes == 2621440U, "T37: full X2 frame is 1280x1024x2B");
static_assert(kStreamVldNum == 1310720U, "T37: per-source valid-pixel count");
// T37 scenario frame counts: phase 2 (the bug), phase 3 (the fix) and
// phase 4 (x10 X1<->X2 switch rounds, one frame each, workaround stays on).
constexpr uint32_t kT37Phase2Frames = 3U;
constexpr uint32_t kT37Phase3Frames = 2U;
constexpr uint32_t kT37Phase4Rounds = 10U;

// ---------------------------------------------------------------------------
// UsbDmaWorker: a NON-AO worker thread modeling the USB Bulk DMA engine (T37
// 7.6.5). The WRAPE AO hands it a frame to ship; it moves the payload in
// kBulkXsfBytes transactions (with per-transaction latency), then posts the
// frame EOF back into coact — the interrupt-callback pattern, mirroring how
// the real WRAPE completion IRQ reaches the driver.
//
// Relationship to the AOs: WrapeAo (producer, on the Dispatcher thread)
// -> UsbDmaWorker (consumer/executor, own thread) -> WinHostAo (observer,
// back on the Dispatcher thread). The only coupling is the event plane.
// Hand-off primitives are DemoPal MutexOps/CondOps/ThreadOps (dual platform).
// ---------------------------------------------------------------------------
struct UsbDmaWorker {
    struct Job {
        bool     valid{false};
        uint32_t frame_id{0U};
        uint32_t payload_bytes{0U};
        bool     err_eof{false};
    };

    DemoPal::MutexHandle mtx{};
    DemoPal::CondHandle  cond{};
    Job             job{};
    bool            running{false};

    PoolT*   pool{nullptr};
    Rt*      rt{nullptr};
    TargetId host_target{};

    uint32_t transactions_done{0U};
    uint32_t completion_rejects{0U};
    uint32_t error_interrupts{0U};   // driver-side USB error interrupt count

    void start(PoolT* p, Rt* r, TargetId host)
    {
        pool = p;
        rt = r;
        host_target = host;
        g_pal->mutex_init(mtx);
        g_pal->cond_init(cond);
        running = true;
        std::printf("[worker] usb_dma started (slot=1)\n");
        g_pal->thread_create(thread_, &UsbDmaWorker::tramp, this);
    }
    void stop()
    {
        g_pal->mutex_lock(mtx);
        running = false;
        g_pal->cond_signal(cond);
        g_pal->mutex_unlock(mtx);
        g_pal->thread_join(thread_);
    }
    // WRAPE AO calls this (Dispatcher thread); never blocks long.
    bool submit(Job j)
    {
        g_pal->mutex_lock(mtx);
        if (job.valid) {
            g_pal->mutex_unlock(mtx);
            return false;   // engine busy
        }
        job = j;
        job.valid = true;
        g_pal->cond_signal(cond);
        g_pal->mutex_unlock(mtx);
        return true;
    }

    // USB error interrupt as its OWN signal path (T37 §2.2 fault chain
    // "error interrupt -> DRV"): distinct from kFrameEof's err_eof DATA
    // attribute — this is the driver-side interrupt line. The error path
    // posts kUsbErrInt (counted by the host AO), and the data EOF still
    // follows with its err_eof flag set. Normal runs never fire it.
    void inject_error_interrupt(uint32_t frame_id)
    {
        ++error_interrupts;
        Layout* err = pool->alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(Sig::kUsbErrInt));
        if (nullptr != err) {
            err->meta.frame_id = frame_id;
            rt->coordinator().submit_from_task(host_target, &err->event,
                                               {false, false});
        }
    }

private:
    DemoPal::ThreadHandle thread_{};
    static void tramp(void* arg)
    {
        static_cast<UsbDmaWorker*>(arg)->run();
    }
    void run()
    {
        for (;;) {
            g_pal->mutex_lock(mtx);
            while (running && !job.valid) {
                g_pal->cond_wait(cond, mtx, 0U);
            }
            if (!running && !job.valid) {
                g_pal->mutex_unlock(mtx);
                break;
            }
            // Fetch-old-reset-slot in one step (the standard exchange idiom).
            Job j = std::exchange(job, Job{});
            g_pal->mutex_unlock(mtx);

            // Move the payload in bulk transactions (transaction granularity,
            // NOT the frame length — the doc's 禁止误解).
            const uint32_t txns =
                (j.payload_bytes + kBulkXsfBytes - 1U) / kBulkXsfBytes;
            for (uint32_t t = 0; t < txns; ++t) {
            /* 阻塞模拟（保留真睡）：USB Bulk DMA 引擎每事务的搬移耗时，
               发生在 UsbDmaWorker 自己的线程，不阻塞 AO 事件循环；每事务
               50us x 160 事务的封帧时序驱动 t37_drain 的真实等待窗口
               （WinHost 收帧计数是真异步的），虚拟时钟推不动真实线程 */
                g_pal->sleep_us(50U);   // per-transaction engine time
            }
            transactions_done += txns;

            // Completion "interrupt": post the frame EOF into coact.
            Layout* fe = pool->alloc_typed<Layout, Payload, kPayloadAlign>(
                static_cast<uint16_t>(Sig::kFrameEof));
            if (nullptr != fe) {
                fe->meta.frame_id = j.frame_id;
                fe->meta.result = static_cast<int32_t>(j.payload_bytes);
                fe->meta.flags = j.err_eof ? 1U : 0U;
                rt->coordinator().submit_from_task(host_target, &fe->event,
                                                   {false, false});
            } else {
                ++completion_rejects;
            }
        }
        std::printf("[worker] usb_dma exited (transactions=%u)\n",
                    transactions_done);
    }
};


// ===========================================================================
// WorkerBase<Derived, Job, Depth, PalT>: the CRTP skeleton shared by the
// queued hardware workers. Owns the mutex+cond hand-off ring, the drain-on-stop
// lifecycle and the executed/statistics counters; each concrete worker
// supplies only the execute-hook (job -> "hardware latency -> completion
// event") via CRTP, resolved at compile time through static_cast<Derived*>.
//
// PalT is the concrete coact PAL (DemoPal): every primitive call — mutex
// lock/unlock, cond wait/signal/broadcast, thread create/join, sleep — goes
// through the PAL instance (g_pal), so the same WorkerBase compiles and runs
// on POSIX host (pthread) and RT-Thread target (rt_mutex/rt_sem/static thread
// table) with zero runtime branching.
//
// Design points (deliberate):
//   - SINGLE-SLOT hand-off (depth 1): a busy channel REJECTS the submit and
//     the AO counts the drop — the non-blocking "hardware takes one request
//     at a time" contract. No request queuing, no waiting; frame loss is an
//     honest counter, never a hidden stall (same policy as UsbDmaWorker).
//   - The ring machinery degenerates to one slot but keeps the same code
//     path, so a future multi-entry channel needs only a depth bump.
//   - submit() runs on the Dispatcher thread (producer side): contended only
//     in the window while the worker holds the mutex popping, so the critical
//     section is a few stores — never blocks on hardware latency.
//   - stop() DRAINS: the worker first empties the queue, then exits, then the
//     caller joins. This is the off-by-zero stop contract a real ISR channel
//     needs — every accepted job produces its completion event (UsbDmaWorker
//     deliberately keeps its busier-discard semantics; both are documented).
//   - Job pop uses std::exchange (fetch-old-reset-slot in one step) and the
//     slot is re-read through std::launder after the placement-new writes —
//     the same object-lifetime discipline DdrCtx::write/read use for the DDR
//     FrameStamp.
// ===========================================================================
template <typename Derived, typename Job, uint8_t kDepthV, typename PalT>
class WorkerBase {
public:
    static constexpr uint8_t kDepth = kDepthV;
    static_assert(kDepthV >= 1U, "worker ring needs at least one slot");

    // CRTP downcast: the base reaches the derived worker's execute() hook
    // through this accessor — compile-time dispatch, no vtable.
    [[nodiscard]] Derived& derived() noexcept { return *static_cast<Derived*>(this); }
    [[nodiscard]] const Derived& derived() const noexcept
    { return *static_cast<const Derived*>(this); }

    void start(PoolT* p, Rt* r)
    {
        pool = p;
        rt = r;
        pal->mutex_init(mtx);
        pal->cond_init(cond);
        pal->cond_init(idle_cv);
        running = true;
        std::printf("[worker] %s started (depth=%u)\n",
                    derived().name(), static_cast<unsigned>(kDepth));
        pal->thread_create(thread_, &WorkerBase::tramp, this);
    }

    // Drain-on-stop: request halt, wake, wait for queue-empty + idle, join.
    // In-flight jobs are NOT discarded (unlike UsbDmaWorker): the completion
    // events of everything accepted before stop() are guaranteed delivered.
    void stop()
    {
        uint8_t queued = 0U;
        {
            pal->mutex_lock(mtx);
            queued = count;
            pal->mutex_unlock(mtx);
        }
        // Print AFTER unlocking: std::printf may block on the console; never
        // hold the hand-off mutex across it (3.3 "critical section only
        // stores").
        std::printf("[worker] %s draining (queued=%u)\n",
                    derived().name(), static_cast<unsigned>(queued));
        pal->mutex_lock(mtx);
        running = false;
        pal->cond_broadcast(cond);
        pal->cond_wait(idle_cv, mtx, 0U);     // until head == tail && !busy
        pal->mutex_unlock(mtx);
        pal->thread_join(thread_);
        std::printf("[worker] %s exited (executed=%u, rejected=%u)\n",
                    derived().name(),
                    static_cast<unsigned>(executed.load(std::memory_order_relaxed)),
                    static_cast<unsigned>(rejected.load(std::memory_order_relaxed)));
        pal->cond_deinit(idle_cv);
        pal->cond_deinit(cond);
        pal->mutex_deinit(mtx);
    }

    // Producer side (Dispatcher thread). Returns false when the queue is
    // full — the honest hardware condition of a DMA request slot being busy
    // (single-slot channels: one in flight, the next submit is rejected).
    bool submit(const Job& j)
    {
        pal->mutex_lock(mtx);
        bool ok = false;
        if (count < kDepth) {             // free capacity
            // Placement-new: begin the Job's lifetime in place in the slot
            // (multi-field descriptor construction — the same in-place
            // discipline as the pool payload and the DDR FrameStamp).
            ::new (static_cast<void*>(&slots[head])) Job(j);
            head = static_cast<uint8_t>((head + 1U) % kDepth);
            ++count;
            ok = true;
        } else {
            // Busy channel: count the drop. (Rejected submissions never
            // touched this counter before — the caller-side WARN and the
            // exit log both depend on it.)
            ++rejected;
        }
        pal->cond_signal(cond);
        pal->mutex_unlock(mtx);
        return ok;
    }

    [[nodiscard]] uint32_t executed_count() const noexcept
    {
        return executed.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint32_t rejected_count() const noexcept
    {
        return rejected.load(std::memory_order_relaxed);
    }

protected:
    PoolT* pool{nullptr};
    Rt*    rt{nullptr};
    std::atomic<uint32_t> executed{0U};
    std::atomic<uint32_t> rejected{0U};

private:
    static void tramp(void* arg)
    {
        static_cast<WorkerBase*>(arg)->run();
    }
    void run()
    {
        Job j{};
        bool busy = false;
        for (;;) {
            pal->mutex_lock(mtx);
            while (running.load() && 0U == count) {
                pal->cond_wait(cond, mtx, 0U);
            }
            if (0U != count) {
                // Pop via launder: the slot was placement-new'd; launder
                // re-establishes the pointer-to-object relationship before
                // the copy (C++17 [ptr.launder], same as DdrCtx::read).
                const Job& front = *std::launder(
                    reinterpret_cast<const Job*>(&slots[tail]));
                j = front;                                   // copy out
                tail = static_cast<uint8_t>((tail + 1U) % kDepth);
                --count;
                busy = true;
            } else {
                busy = false;
            }
            if (!busy && !running.load()) {
                pal->cond_signal(idle_cv);
                pal->mutex_unlock(mtx);
                return;                     // drained and halted
            }
            pal->mutex_unlock(mtx);

            derived().execute(j);                              // CRTP hook
            executed.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    PalT* pal{g_pal};                        // the demo's single PAL instance
    typename PalT::ThreadHandle thread_{};
    typename PalT::MutexHandle  mtx{};
    typename PalT::CondHandle   cond{};
    typename PalT::CondHandle   idle_cv{};
    Job slots[kDepth]{};
    uint8_t head{0U};
    uint8_t tail{0U};
    uint8_t count{0U};     // live jobs (kDepth == capacity, not capacity-1)
    std::atomic<bool> running{false};
};

// Demo shorthand: every concrete worker binds the demo's PAL alias.
template <typename Derived, typename Job, uint8_t kDepthV>
using DemoWorkerBase = WorkerBase<Derived, Job, kDepthV, DemoPal>;

// ---------------------------------------------------------------------------
// CmdDmaWorker: async vdcmd command channel. Maps the RS500 vdcmd service
// layer's rt_device_control(DEV_IRSC_CTRL_*) path: the driver AO accepts a
// sub-command, hands it to the channel, and the register-write engine drives
// the bus (latency), then raises the completion callback as an event
// (kIrscDmaDone) back into the driver AO. Pure message simulation: no SPI
// transactions, no protocol parsing — only the async round-trip shape.
// ---------------------------------------------------------------------------
// NOTE on depth: the frame-side channels (IspIrq/SoutDma/MipiIrq) are
// SINGLE-SLOT (depth 1): hardware takes one frame request at a time and a
// busy slot is an honest reject. The COMMAND channel is depth 4: vdcmd
// register-write transactions are a bounded burst (the 4-step IRSC sequence
// arrives back-to-back) and MUST all execute in order — dropping a control
// step would break the boot contract, so this channel carries a small
// request queue instead (bounded, compile-time, still no blocking).
struct CmdDmaWorker : DemoWorkerBase<CmdDmaWorker, uint16_t, 4U> {
    TargetId reply_to{};
    uint32_t completion_rejects{0U};
    static constexpr const char* name() noexcept { return "cmd_dma"; }

    void start(PoolT* p, Rt* r, TargetId driver)
    {
        reply_to = driver;
        WorkerBase::start(p, r);
    }

    void execute(const uint16_t& cmd_arg)
    {
        /* 阻塞模拟（保留真睡）：vdcmd 异步寄存器写通道的总线耗时，
           发生在 CmdDmaWorker 自己的线程，不阻塞 AO 事件循环；
           4 次完成事件的到达时序被 dma_done_count 断言真实观察 */
        g_pal->sleep_us(kIrscCmdLatencyUs);   // register-write bus time
        Layout* done = pool->alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(Sig::kIrscDmaDone));
        if (nullptr != done) {
            done->meta.cmd_arg = cmd_arg;
            done->meta.payload_kind = 3U;
            rt->coordinator().submit_from_task(reply_to, &done->event,
                                               {false, false});
        } else {
            ++completion_rejects;
        }
    }
};


// ---------------------------------------------------------------------------
// IrscDriver AO: models sensor_input_lsv_source + drv_irsc.
// Handles kIrscCmd in 4 steps (init / start / ctrl / output_enable).
// Each step allocates and submits a kIrscReady ack to the orchestrator.
// ---------------------------------------------------------------------------
enum IrscStep : uint8_t { kIrscInit = 0U, kIrscStart = 1U, kIrscCtrl = 2U, kIrscOutputEnable = 3U };

// ---------------------------------------------------------------------------
// Command pattern: IRSC driver sub-commands.
//
// Each IRSC step (init/start/ctrl/output_enable) is a self-contained command
// object carrying its own tag — the orchestrator builds the command and the
// driver executes it, mirroring how RS500's rt_device_control(DEV_IRSC_CTRL_*)
// dispatches into the driver.
// ---------------------------------------------------------------------------
struct IrscCmd {
    IrscStep    step{kIrscInit};
    const char* tag{""};

    // Stamp the command identity into the event block (command -> event meta).
    void stamp(Layout& e) const
    {
        e.meta.cmd_arg = static_cast<uint16_t>(step);
        e.meta.payload_kind = 3U;
        Payload* p = reinterpret_cast<Payload*>(&e.payload[0]);
        p->control[0] = static_cast<uint8_t>(tag[0]);
        p->control[1] = static_cast<uint8_t>(tag[1]);
        p->control[2] = static_cast<uint8_t>(tag[2]);
        p->control[3] = static_cast<uint8_t>(tag[3]);
        p->control[4] = static_cast<uint8_t>(step);
    }
};

// The RS500 boot sequence as a compile-time command table: order IS the
// contract (init -> start -> ctrl -> output_enable).
inline const char* irsc_step_name(IrscStep s)
{
    switch (s) {
        case kIrscInit: return "init";
        case kIrscStart: return "start";
        case kIrscCtrl: return "ctrl";
        case kIrscOutputEnable: return "output_enable";
    }
    return "?";
}

inline constexpr IrscCmd kIrscCmdSequence[] = {
    { kIrscInit,         "IRSCI" },
    { kIrscStart,        "IRSCS" },
    { kIrscCtrl,         "IRSCC" },
    { kIrscOutputEnable, "IRSCE" },
};

// ---------------------------------------------------------------------------
// Command-pattern execution table for the async IRSC channel: the compile-time
// sub-command table decides the completion posture. output_enable completes
// the IRSC bring-up; every other step merely acknowledges. The table IS the
// driver's command semantics — adding a step means adding one row.
// ---------------------------------------------------------------------------
enum IrscDoneAction : uint8_t { kAck = 0U, kAckAndReady = 1U };

struct IrscDoneStep {
    IrscStep       step;
    IrscDoneAction action;
};

inline constexpr IrscDoneStep kIrscDoneTable[] = {
    { kIrscInit,         kAck },
    { kIrscStart,        kAck },
    { kIrscCtrl,         kAck },
    { kIrscOutputEnable, kAckAndReady },
};

constexpr IrscDoneAction irsc_done_action_of(IrscStep s) noexcept
{
    for (const IrscDoneStep& row : kIrscDoneTable) {
        if (row.step == s) { return row.action; }
    }
    return kAck;
}

struct IrscCtx {
    PoolT*    pool{nullptr};
    Rt*       rt{nullptr};
    TargetId  producer_target{};   // where to spin up the IRSC pthread
    TargetId  orchestrator{};      // who gets the kIrscReady ack
    CmdDmaWorker* cmd_channel{nullptr};   // async vdcmd rt_device_control
    bool      ready{false};
    uint32_t  step_count{0U};
    uint32_t  channel_subs{0U};    // jobs accepted by the channel
    uint32_t  channel_rejects{0U}; // jobs the channel dropped (queue full)
    uint32_t  dma_done_count{0U};  // completion callbacks observed
};

// kIrscCmd: NOT executed inline. The driver hands the sub-command to the
// async channel (vdcmd rt_device_control) and returns immediately — the
// register-write latency lives on the worker thread, exactly like the real
// driver never busy-waits the bus in its dispatcher context.
inline void onIrscCmd(IrscCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    const IrscStep step = static_cast<IrscStep>(e.meta.cmd_arg);
    const uint16_t arg = static_cast<uint16_t>(step);

    if (ctx.cmd_channel->submit(arg)) {
        ++ctx.channel_subs;
    } else {
        // Busy vdcmd channel — drop prelude (asserted to stay 0 in a healthy
        // run, so any occurrence is an anomaly worth a visible WARN).
        ++ctx.channel_rejects;
        std::printf("[warn] cmd_dma submit rejected (busy), step=%u\n",
                    static_cast<unsigned>(arg));
    }
}

// kIrscDmaDone: the channel's completion callback. The COMPILE-TIME command
// table decides the posture (command pattern: the table, not an if-chain, is
// the driver's semantics). Ack is assembled here and only here.
inline void onIrscDmaDone(IrscCtx& ctx, const Event& evt)
{
    const Layout& e = *reinterpret_cast<const Layout*>(&evt);
    const IrscStep step = static_cast<IrscStep>(e.meta.cmd_arg);

    Layout* ack = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(
        static_cast<uint16_t>(Sig::kIrscReady));
    if (nullptr == ack) { return; }
    ack->meta.cmd_arg = static_cast<uint16_t>(step);
    ack->meta.result = 0;
    ack->meta.payload_kind = 3U;
    ack->meta.reply_to = ctx.orchestrator;
    Payload* p = reinterpret_cast<Payload*>(&ack->payload[0]);
    p->control[0] = static_cast<uint8_t>('I');
    p->control[1] = static_cast<uint8_t>('R');
    p->control[2] = static_cast<uint8_t>('S');
    p->control[3] = static_cast<uint8_t>('C');
    p->control[4] = static_cast<uint8_t>(step);
    ctx.rt->coordinator().submit_from_task(ctx.orchestrator, &ack->event, {true, false});
    ++ctx.step_count;
    ++ctx.dma_done_count;

    // Command-pattern posture: the table row decides readiness.
    if (kAckAndReady == irsc_done_action_of(step)) { ctx.ready = true; }
}

// Reject arc: a command while DEINIT/STOPPED must not reach the hardware —
// the session guard on the transition table already blocks it, this action
// is the observable trace for the covered case.
inline void onIrscCmdRejected(IrscCtx& ctx, const Event&)
{
    ++ctx.channel_rejects;
    HsmTrace::rejection("irsc", "ACTIVE", static_cast<uint16_t>(Sig::kIrscCmd),
                        "session not INIT/RUNNING");
}

// IrscDriver HSM: two real states. ACTIVE accepts commands while the session
// is in INIT/RUNNING; the guarded arc falls through to the self-transition
// that records the rejection — the HSM's own guard-ordering (first match
// wins, failed guard scans on) provides the reject path for free.
enum : int8_t { kIrscRoot = 0, kIrscActive = 1 };

inline const StateDef<IrscCtx> kIrscStates[] = {
    { -1, nullptr, nullptr, "Root" },
    { kIrscRoot, nullptr, nullptr, "Active" },
};

[[nodiscard]] inline bool irsc_session_open(const IrscCtx&, const Event&) noexcept
{
    const SessionState s = g_session.load(std::memory_order_relaxed);
    return (SessionState::kInit == s) || (SessionState::kRunning == s);
}

inline const TransitionDef<IrscCtx> kIrscTransitions[] = {
    // Guarded accept (Internal): command forwarded to the channel.
    { kIrscActive, static_cast<uint16_t>(Sig::kIrscCmd), kIrscActive,
      TransitionKind::Internal, irsc_session_open, onIrscCmd },
    // Guard-reject fallback (Self): trace the refusal.
    { kIrscActive, static_cast<uint16_t>(Sig::kIrscCmd), kIrscActive,
      TransitionKind::Self, nullptr, onIrscCmdRejected },
    // Completion callback from the channel.
    { kIrscActive, static_cast<uint16_t>(Sig::kIrscDmaDone), kIrscActive,
      TransitionKind::Internal, nullptr, onIrscDmaDone },
};

using IrscDriverAo = coact::Ao<IrscCtx, Hsm<IrscCtx>, IrscTrait>;

// Forward declare IrscTrait here so the IrscDriverAo typedef can resolve;
// the full definition is right after the AoTrait family.
struct IrscTrait;

}  // namespace isp_demo
