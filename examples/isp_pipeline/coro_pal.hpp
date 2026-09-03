/*
 * ===========================================================================
 * coro_pal.hpp —— ISP demo 的 coro 模式 PAL 垫片（Posix 子类路由到协程）
 * ===========================================================================
 * 功能描述
 *   - 本文件是 coro 模式下的 DemoPal 实现：把 coact::pal::Posix 的
 *     thread_create / sleep_us / mutex_* / cond_* 操作面重写为协程化版
 *     本，落到 coro_mode.hpp 的 StackfulExecutor 上。原本各自跑在 7 个
 *     pthread 上的 worker execute()，经此 shim 后共享单 pthread 的 pump。
 *   - 仅在编译期定义 ISP_DEMO_CORO 时被 common.hpp 的 DemoPal alias 选中
 *     （ISP_DEMO_USE_RTT 优先于 ISP_DEMO_CORO，host 默认走 Posix）。
 *   - 行为契约：对外接口与 Posix 一致（SoO/LoO 替换），内部把 pthread_
 *     create 改为 StackfulExecutor::spawn + trampoline，sleep_us 改为
 *     coact::coro::sleep_for 协程让出。
 *   对应 RS500 module/common 服务层在 MCU 单核公平调度模型下的 PAL
 *   替换路径，与 RT-Thread PAL（多线程静态表）形成第三种执行拓扑。
 *
 * 与其他文件的关系
 *   - 上游：common.hpp 在 ISP_DEMO_CORO 时把 DemoPal 绑到本文件 CoroPal；
 *     main.cpp 启动顺序为：构造 StackfulExecutor → 启动 pump → 实例化
 *     worker → worker.start() 经 g_pal->thread_create 落入本文件 shim。
 *   - 下游：全部 7 个非 AO worker（IrscWorker / IspIrqWorker / SoutDmaWorker
 *     / MipiIrqWorker / CmdDmaWorker / UsbDmaWorker）共用此 shim；
 *     worker 的 sleep_us / mutex_lock 也走它进入协程 pump。
 *   - 依赖：coact::pal::Posix（基类）/ coro_mode.hpp（StackfulExecutor 与
 *     yield/resume）/ coact::coro 原语；common.hpp 的 g_pal 单例持有。
 *
 * 文字图（PAL shim 视角）
 * ┌──────────────────────────────────────────────────────────────────┐
 * │ 例子系统数据流（→事件信号  ⇒DDR/黑板数据）                        │
 * │                                                                  │
 * │  [sensor_irsc]──kFrameIrscOut──▶[isp_chain]──kHlFused/Done──▶   │
 * │   ▲ DemoPal (ISP_DEMO_CORO → 本文件 CoroPal)                    │
 * │   │ thread_create / sleep_us / cond_wait → StackfulExecutor pump│
 * │                                  [video_stream]──kPicPacked──▶  │
 * │                                  [output_itf]──kFrameEof──▶     │
 * │                                  [winhost 观察者]                │
 * │                                                                  │
 * │  编排: [main.cpp] ──kIrscCmd/kVideoCmd──▶ 各模块 AO             │
 * │  重配: [recfg_session] ◀──kRecfgReq── main；门控 g_session      │
 * │  共享: [common.hpp] 词汇+DdrCtx+黑板 / PAL: g_pal (本文件 shim)  │
 * └──────────────────────────────────────────────────────────────────┘
 * ===========================================================================
 */
// ISP demo coro-mode PAL shim: coact::pal::Posix subclass whose
// thread_create/sleep_us route into the coro executor instead of pthreads.
// SPDX-License-Identifier: MIT
#pragma once

#include "isp_pipeline/coro_mode.hpp"

#ifdef ISP_DEMO_CORO

#include <cstdint>

#include "coact/pal_posix.hpp"

namespace isp_demo_coro {

struct CoroThreadCtx {
    void (*entry)(void*) = nullptr;
    void* arg = nullptr;
    coact::coro::posix::Coroutine* co = nullptr;
};

inline bool is_on_coroutine_stack() noexcept
{
    return pthread_self() == g_thread && g_thread_valid;
}

inline void coro_pal_sleep_us(coact::coro::posix::Coroutine* co,
                              uint32_t us) noexcept
{
    if ((nullptr != co) && is_on_coroutine_stack()) {
        coro_sleep_us(*co, us);
    } else {
        sleep_us_impl(us);
    }
}

inline void coro_thread_body(void* user,
                             coact::coro::posix::Coroutine& self);

class CoroPal final : public coact::pal::Posix {
public:
    static constexpr uint16_t kMaxCoroThreads = 12U;

    bool thread_create(ThreadHandle& t, coact::pal::ThreadEntry entry,
                       void* context) noexcept
    {
        for (uint16_t i = 0U; i < kMaxCoroThreads; ++i) {
            if (nullptr == slots_[i].entry) {
                slots_[i].entry = entry;
                slots_[i].arg = context;
                slots_[i].co = g_exec->arm(&coro_thread_body, &slots_[i],
                                           coact::coro::posix::ResumeArg{});
                if (nullptr == slots_[i].co) {
                    slots_[i].entry = nullptr;
                    return false;
                }
                t.valid = true;
                t.tid = reinterpret_cast<pthread_t>(slots_[i].co);
                return true;
            }
        }
        return false;
    }

    void thread_join(ThreadHandle& t) noexcept
    {
        for (uint16_t i = 0U; i < kMaxCoroThreads; ++i) {
            if (reinterpret_cast<pthread_t>(slots_[i].co) == t.tid &&
                nullptr != slots_[i].entry) {
                for (int pass = 0; pass < 2000000; ++pass) {
                    if (!slots_[i].co->is_running()) {
                        slots_[i].entry = nullptr;
                        t.valid = false;
                        return;
                    }
                    sleep_us_impl(200U);
                }
                return;
            }
        }
        t.valid = false;
    }

    void sleep_us(uint32_t us) noexcept
    {
        coro_pal_sleep_us(current_, us);
    }

    // Cooperative cond_wait: when called from INSIDE a coroutine body, the
    // pump thread must never park in pthread_cond_wait (that would freeze
    // every other coroutine). Instead: unlock, yield one scheduling pass
    // (200 us park via the executor), relock - the single-core polling
    // scheduler equivalent of a condvar wait. On any other thread it is a
    // real cond_wait (main-thread joins keep blocking semantics).
    void cond_wait(CondHandle& c, MutexHandle& m,
                   uint32_t timeout_ms) noexcept
    {
        if ((nullptr != current_) && is_on_coroutine_stack()) {
            coact::pal::Posix::mutex_unlock(m);
            coro_sleep_us(*current_, 200U);
            /* Re-acquire with a yield-loop trylock: the main thread may hold
               the mutex inside its own (real) cond_wait stop protocol - a
               blocking pthread_mutex_lock here would park the pump thread
               and freeze every coroutine (single-core rule: never park). */
            while (0 != pthread_mutex_trylock(&m.mtx)) {
                coro_sleep_us(*current_, 200U);
            }
            return;
        }
        coact::pal::Posix::cond_wait(c, m, timeout_ms);
    }

    static void set_current(coact::coro::posix::Coroutine* co) noexcept
    {
        current_ = co;
    }

private:
    CoroThreadCtx slots_[kMaxCoroThreads]{};
    static thread_local coact::coro::posix::Coroutine* current_;
};

inline thread_local coact::coro::posix::Coroutine* CoroPal::current_ = nullptr;

inline void coro_thread_body(void* user,
                             coact::coro::posix::Coroutine& self)
{
    CoroThreadCtx* ctx = static_cast<CoroThreadCtx*>(user);
    CoroPal::set_current(ctx->co);
    ctx->entry(ctx->arg);
    CoroPal::set_current(nullptr);
    (void)self.yield(coact::coro::posix::YieldRequest{
        coact::coro::posix::WaitReason::kDone, 0U, 0U});
}

}  // namespace isp_demo_coro

#endif  // ISP_DEMO_CORO
