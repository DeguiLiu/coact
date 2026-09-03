/*
 * ===========================================================================
 * coro_mode.hpp —— ISP demo 的 coro 模式 worker 后端（StackfulExecutor）
 * ===========================================================================
 * 功能描述
 *   - 提供 coact::coro::StackfulExecutor 的 ISP demo 实例化（16 协程槽位，
 *     128KB 单栈），把 7 个非 AO worker（IrscWorker / IspIrqWorker /
 *     SoutDmaWorker / MipiIrqWorker / CmdDmaWorker / UsbDmaWorker）的
 *     execute() 逻辑搬到一组 stackful 协程上。
 *   - 编译期门控 ISP_DEMO_CORO：定义时全部 worker 协程化，run 在单 pthread
 *     上的 pump()（可选 CPU 亲和钉），呈现 MCU 公平单核语义（与 RT-Thread
 *     PAL 行为对齐）；未定义时退回到 per-worker pthread（与原行为一致）。
 *   - 提供 yield-point 表：sleep_us / 锁等待 / 条件变量等待都映射到
 *     coact::coro::yield，外部事件到达后由 Runtime 的 pump 循环 resume。
 *   对应 RS500 module/common 服务层在单核 MCU 下的执行公平性建模目标
 *   （高优先级事件不应被低优先级 worker 长 sleep 饿死）。
 *
 * 与其他文件的关系
 *   - 上游：main.cpp 在 ISP_DEMO_CORO 分支构造 StackfulExecutor、设全局
 *     g_exec、调用 start_executor；stop 阶段调 stop_executor 排空协程。
 *   - 下游：[isp_chain] 等模块的 worker 调用 DemoPal::thread_create 时，
 *     经 coro_pal.hpp 的 PAL shim 路由到本文件注册的协程 trampoline；
 *     worker 的 sleep_us / cond_wait 同样通过 shim 进入 pump 循环。
 *   - 依赖：coact::coro::StackfulExecutor / yield/resume 原语；coact::pal
 *     PAL 接口；common.hpp 的 Sig/PoolT/Rt 等词汇（通过 coro_pal.hpp 间接）。
 *
 * 文字图（coro 后端视角）
 * ┌──────────────────────────────────────────────────────────────────┐
 * │ 例子系统数据流（→事件信号  ⇒DDR/黑板数据）                        │
 * │                                                                  │
 * │  [sensor_irsc]──kFrameIrscOut──▶[isp_chain]──kHlFused/Done──▶   │
 * │   ▲ PAL shim → 本文件 StackfulExecutor (单 pthread pump)         │
 * │   │ 全 7 worker execute() 跑在协程里, MCU 公平调度              │
 * │                                  [video_stream]──kPicPacked──▶  │
 * │                                  [output_itf]──kFrameEof──▶     │
 * │                                  [winhost 观察者]                │
 * │                                                                  │
 * │  编排: [main.cpp] ──kIrscCmd/kVideoCmd──▶ 各模块 AO             │
 * │  重配: [recfg_session] ◀──kRecfgReq── main；门控 g_session      │
 * │  共享: [common.hpp] 词汇+DdrCtx+黑板 / PAL: g_pal (CoroPal)      │
 * └──────────────────────────────────────────────────────────────────┘
 * ===========================================================================
 */
// ISP demo coro-mode worker layer: coact::coro StackfulExecutor back-end
// replacing the 7 per-worker pthreads (公平性验证, coro fairness mode).
//
// Mode selection (ISP_DEMO_CORO): compile-time. With the macro defined, all
// 7 workers' execute logic runs as stackful coroutines on ONE pthread
// (optionally pinned) driven by coact_coro_exec::pump() — the MCU-fair
// single-core semantics that match the RT-Thread PAL behavior. Without the
// macro the demo compiles exactly as before (per-worker pthreads).
//
// What maps to what (yield-point table, Lua yield/resume model):
//   pthread mode                          coro mode
//   -----------------------------------------------------------------
//   WorkerBase thread_create/cond_wait    one Coroutine slot per worker
//   execute() g_pal->sleep_us(L)          co.yield(kSleep, now_ns()+L*1000)
//   UsbDmaWorker per-txn 50us sleeps      one yield per transaction
//   IrscWorker period pacing sleeps       one yield per frame period
//   stop(): drain + join                  retire flag; pump() drains
//
// The submit/reject counters are unchanged: a coroutine "busy" in its sleep
// yield behaves exactly like a pthread busy in usleep — the same single-slot
// / fixed-depth contract, the same honest rejections. Coroutine yields are
// cooperative (no preemption), which is precisely the single-core MCU model.
//
// All coroutines run on the DISPATCHER-side pump thread; the main thread
// drives scenario phases exactly as in pthread mode (its conditional-poll
// pacing sleeps remain real sleeps on the main thread, outside the coro
// executor — they pace the TEST DRIVER, not the workload).
// SPDX-License-Identifier: MIT
#pragma once

#ifdef ISP_DEMO_CORO

#include <pthread.h>
#include <time.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <utility>

#include "coact/coro/posix.hpp"

namespace isp_demo_coro {

// The single executor instance (defined in main.cpp).
inline coact::coro::posix::StackfulExecutor<16U, 256U * 1024U>* g_exec =
    nullptr;
inline std::atomic<bool> g_stop{false};
inline pthread_t g_thread{};
inline bool g_thread_valid = false;

inline uint64_t now_us() noexcept
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
           static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

inline void sleep_us_impl(uint64_t us) noexcept
{
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(us / 1000000ULL);
    ts.tv_nsec = static_cast<long>((us % 1000000ULL) * 1000ULL);
    clock_nanosleep(CLOCK_MONOTONIC, 0U, &ts, nullptr);
}

// Yield with a relative microsecond deadline (the worker-facing API; every
// worker sleep becomes this call).
inline void coro_sleep_us(coact::coro::posix::Coroutine& co,
                          uint32_t us) noexcept
{
    (void)co.yield(coact::coro::posix::YieldRequest{
        coact::coro::posix::WaitReason::kSleep,
        now_us() + static_cast<uint64_t>(us), 0U});
}

// Executor pump thread: round-robin cooperative passes until stop + drain.
inline void* pump_trampoline(void* /*arg*/) noexcept
{
    while (!g_stop.load(std::memory_order_acquire)) {
        if (0U == g_exec->run_once()) {
            /* All coroutines parked or finished: brief idle nap (no busy
               spin; the workload is latency-paced, not throughput-paced). */
            sleep_us_impl(200U);
        }
    }
    /* Drain: retire every remaining coroutine after stop was requested. */
    for (int pass = 0; pass < 20000; ++pass) {
        if (0U == g_exec->run_once()) {
            break;
        }
    }
    return nullptr;
}

inline bool start_executor() noexcept
{
    if (g_thread_valid) {
        return false;
    }
    /* Optional pinning: best effort (a container may refuse; a 2-thread
       process still has no second core preempting task state). */
    (void)coact::coro::posix::pin_current_thread_to_core(0U);
    if (0 != pthread_create(&g_thread, nullptr, &pump_trampoline,
                            nullptr)) {
        return false;
    }
    g_thread_valid = true;
    return true;
}

inline void stop_executor() noexcept
{
    if (g_thread_valid) {
        g_stop.store(true, std::memory_order_release);
        pthread_join(g_thread, nullptr);
        g_thread_valid = false;
    }
}

}  // namespace isp_demo_coro

#endif  // ISP_DEMO_CORO
