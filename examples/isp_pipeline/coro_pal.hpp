/*
 * ===========================================================================
 * coro_pal.hpp —— ISP demo 的 coro 模式 PAL 垫片（Posix 子类路由到协程）
 * ===========================================================================
 * 功能描述
 *   - 本文件是 coro 模式下的 DemoPal 实现：把 coact::pal::Posix 的
 *     thread_create / sleep_us / cond_wait 操作面重写为协程化版本，落到
 *     coro_mode.hpp 的 StackfulExecutor 上。原本各自跑在 7 个 pthread 上
 *     的 worker execute()，经此 shim 后共享单 pthread 的 pump。
 *   - 仅在编译期定义 ISP_DEMO_CORO 时被 common.hpp 的 DemoPal alias 选中
 *     （ISP_DEMO_USE_RTT 优先于 ISP_DEMO_CORO，host 默认走 Posix）。
 *   - 线程模型：只有 1 个 worker 载体 pthread（pump）。thread_create 仅
 *     登记请求；pump 在自己的栈上 materialize 协程（getcontext/makecontext
 *     全部发生在 pump 线程），随后 run_once 协作驱动。
 *
 * 与其他文件的关系
 *   - 上游：common.hpp 在 ISP_DEMO_CORO 时把 DemoPal 绑到本文件 CoroPal；
 *     main.cpp 启动顺序为：构造 StackfulExecutor → install_pump_hook →
 *     启动 pump → 实例化 worker → worker.start() 经 g_pal->thread_create
 *     落入本文件 shim 的登记表。
 *   - 下游：全部 7 个非 AO worker 共用此 shim。
 *   - 依赖：coact::pal::Posix（基类）/ coro_mode.hpp（StackfulExecutor 与
 *     yield/resume）/ coact::coro 原语；common.hpp 的 g_pal 单例持有。
 * ===========================================================================
 */
// ISP demo coro-mode PAL shim: coact::pal::Posix subclass whose
// thread_create/sleep_us/cond_wait route into the coro executor instead of
// pthreads. SPDX-License-Identifier: MIT
#pragma once

#include "isp_pipeline/coro_mode.hpp"

#ifdef ISP_DEMO_CORO

#include <cstdint>
#include <mutex>

#include "coact/pal_posix.hpp"

namespace isp_demo_coro {

// One deferred thread_create request. The pump materializes it (arms the
// coroutine) on the pump thread; the calling thread never touches ucontext.
struct PendingSlot {
    void (*entry)(void*) = nullptr;
    void* arg = nullptr;
    coact::coro::posix::Coroutine* co = nullptr;  // armed by the pump
};

inline bool is_on_pump_thread() noexcept
{
    return pthread_self() == g_thread && g_thread_valid;
}

// Sleep routing: inside a coroutine body (pump thread with a live current_)
// the sleep becomes a cooperative yield; on any other thread it is a real
// sleep (main-thread pacing keeps real semantics).
inline void coro_pal_sleep_us(coact::coro::posix::Coroutine* co,
                              uint32_t us) noexcept
{
    if ((nullptr != co) && is_on_pump_thread()) {
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

    // Deferred arm: the pump thread materializes pending thread_create
    // requests on ITS OWN stack (ucontext setup stays on one thread).
    static void pump_materialize() noexcept
    {
        for (uint16_t i = 0U; i < kMaxCoroThreads; ++i) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            PendingSlot& p = pending_[i];
            if ((nullptr != p.entry) && (nullptr == p.co)) {
                p.co = g_exec->arm(&coro_thread_body, &p,
                                   coact::coro::posix::ResumeArg{});
            }
        }
    }

    // thread_create only ENQUEUES the request; the pump arms it.
    bool thread_create(ThreadHandle& t, coact::pal::ThreadEntry entry,
                       void* context) noexcept
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (uint16_t i = 0U; i < kMaxCoroThreads; ++i) {
            if (nullptr == pending_[i].entry) {
                pending_[i].entry = entry;
                pending_[i].arg = context;
                pending_[i].co = nullptr;
                t.valid = true;
                t.tid = reinterpret_cast<pthread_t>(&pending_[i]);
                return true;
            }
        }
        return false;
    }

    void thread_join(ThreadHandle& t) noexcept
    {
        PendingSlot* p = reinterpret_cast<PendingSlot*>(t.tid);
        for (uint32_t pass = 0U; pass < 2000000U; ++pass) {
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                if ((nullptr != p->co) && !p->co->is_running()) {
                    p->entry = nullptr;
                    p->co = nullptr;
                    t.valid = false;
                    return;
                }
            }
            sleep_us_impl(200U);
        }
        t.valid = false;
    }

    void sleep_us(uint32_t us) noexcept
    {
        coro_pal_sleep_us(coact::coro::posix::Coroutine::current(), us);
    }

    // Cooperative cond_wait: when called from INSIDE a coroutine body, the
    // pump thread must never park in pthread_cond_wait (that would freeze
    // every other coroutine). Instead: unlock, yield one scheduling pass,
    // relock with a trylock loop (the main thread may hold the mutex inside
    // its own real cond_wait stop protocol; a blocking lock here would park
    // the pump - single-core rule: never park). On any other thread this is
    // a real cond_wait.
    void cond_wait(CondHandle& c, MutexHandle& m,
                   uint32_t timeout_ms) noexcept
    {
        coact::coro::posix::Coroutine* current =
            coact::coro::posix::Coroutine::current();
        if ((nullptr != current) && is_on_pump_thread()) {
            coact::pal::Posix::mutex_unlock(m);
            coro_sleep_us(*current, 200U);
            while (0 != pthread_mutex_trylock(&m.mtx)) {
                coro_sleep_us(*current, 200U);
            }
            return;
        }
        coact::pal::Posix::cond_wait(c, m, timeout_ms);
    }

private:
    static PendingSlot pending_[kMaxCoroThreads];
    static std::mutex pending_mutex_;
};

inline PendingSlot CoroPal::pending_[kMaxCoroThreads] = {};
inline std::mutex CoroPal::pending_mutex_;

// Install the pump hook (called once from main via install_pump_hook()).
inline void install_pump_hook() noexcept
{
    pump_materialize_hook = &CoroPal::pump_materialize;
}

inline void coro_thread_body(void* user,
                             coact::coro::posix::Coroutine& self)
{
    PendingSlot* ctx = static_cast<PendingSlot*>(user);
    ctx->entry(ctx->arg);
    (void)self.yield(coact::coro::posix::YieldRequest{
        coact::coro::posix::WaitReason::kDone, 0U, 0U});
}

}  // namespace isp_demo_coro

#endif  // ISP_DEMO_CORO
