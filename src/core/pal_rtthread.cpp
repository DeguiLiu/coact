// coact RT-Thread PAL implementation.
// SPDX-License-Identifier: MIT
#include "coact/pal_rtthread.hpp"

#include <cstdlib>

namespace coact {
namespace pal {

/* Per-thread ExecutionContext. On RT-Thread we store a pointer to a
   heap-allocated ExecutionContext in rt_thread::user_data. The POSIX stub
   backs this with thread_local via tls_self. */
static ExecutionContext* alloc_ctx(LogicalPrio prio) noexcept
{
    ExecutionContext* ctx = static_cast<ExecutionContext*>(
        rt_malloc(sizeof(ExecutionContext)));
    if (nullptr == ctx) {
        return nullptr;
    }
    ctx->kind         = ContextKind::Task;
    ctx->logical_prio = prio;
    ctx->prio_valid   = true;
    ctx->direct_depth = 0U;
    return ctx;
}

RtThread::RtThread(uint32_t /*cpu_freq_hz*/) noexcept
    : wake_sem_(nullptr),
      join_sem_(nullptr),
      dispatcher_thread_(nullptr),
      user_entry_(nullptr),
      user_ctx_(nullptr),
      thread_started_(false),
      ns_per_tick_(1000000000U / RT_TICK_PER_SECOND)
{
    wake_sem_ = rt_sem_create("coact_wake", 0U, RT_IPC_FLAG_PRIO);
    join_sem_ = rt_sem_create("coact_join", 0U, RT_IPC_FLAG_PRIO);
    /* Both sems are required; assert on failure (init-phase allocation). */
}

CriticalToken RtThread::irq_save() noexcept
{
    CriticalToken tok;
    /* rt_hw_interrupt_disable returns rt_base_t; fits in uintptr_t. */
    tok.value = static_cast<uintptr_t>(rt_hw_interrupt_disable());
    return tok;
}

void RtThread::irq_restore(CriticalToken token) noexcept
{
    rt_hw_interrupt_enable(static_cast<rt_base_t>(token.value));
}

bool RtThread::register_current_task(LogicalPrio prio) noexcept
{
    if (kInvalidPrio == prio || prio > kMaxPrio) {
        return false;
    }
    rt_thread_t self = rt_thread_self();
    if (nullptr == self) {
        return false;  /* called from ISR or before scheduler start */
    }
    /* Reuse existing context if already registered. */
    ExecutionContext* ctx = static_cast<ExecutionContext*>(
        reinterpret_cast<void*>(self->user_data));
    if (nullptr == ctx) {
        ctx = alloc_ctx(prio);
        if (nullptr == ctx) {
            return false;
        }
        self->user_data = reinterpret_cast<rt_ubase_t>(ctx);
    }
    else {
        ctx->logical_prio = prio;
        ctx->prio_valid   = true;
    }
    return true;
}

ExecutionContext* RtThread::tls_ctx() noexcept
{
    rt_thread_t self = rt_thread_self();
    if (nullptr == self) {
        return nullptr;
    }
    return static_cast<ExecutionContext*>(
        reinterpret_cast<void*>(self->user_data));
}

ExecutionContext RtThread::current_context() const noexcept
{
    /* ISR context: rt_interrupt_get_nest() > 0. */
    if (0U < rt_interrupt_get_nest()) {
        ExecutionContext ctx;
        ctx.kind         = ContextKind::Isr;
        ctx.logical_prio = 0U;
        ctx.prio_valid   = false;
        ctx.direct_depth = 0U;
        return ctx;
    }
    ExecutionContext* stored = tls_ctx();
    if (nullptr != stored) {
        return *stored;
    }
    /* Unregistered task: return generic Task context. */
    ExecutionContext ctx;
    ctx.kind         = ContextKind::Task;
    ctx.logical_prio = 0U;
    ctx.prio_valid   = false;
    ctx.direct_depth = 0U;
    return ctx;
}

uint64_t RtThread::monotonic_ns() const noexcept
{
    return static_cast<uint64_t>(rt_tick_get())
         * static_cast<uint64_t>(ns_per_tick_);
}

uint64_t RtThread::clock_resolution_ns() const noexcept
{
    return static_cast<uint64_t>(ns_per_tick_);
}

void RtThread::wait_dispatcher(uint32_t timeout_ms) noexcept
{
    if (nullptr == wake_sem_) {
        return;
    }
    /* Convert ms -> ticks (ceiling). RT_TICK_PER_SECOND ticks per second. */
    rt_int32_t ticks;
    if (0U == timeout_ms) {
        ticks = static_cast<rt_int32_t>(RT_WAITING_FOREVER);
    }
    else {
        const uint64_t t = (static_cast<uint64_t>(timeout_ms)
                            * RT_TICK_PER_SECOND + 999U) / 1000U;
        ticks = (t > 0x7FFFFFFFU) ? 0x7FFFFFFF : static_cast<rt_int32_t>(t);
    }
    rt_sem_take(wake_sem_, ticks);
}

void RtThread::signal_dispatcher_from_task() noexcept
{
    if (nullptr != wake_sem_) {
        rt_sem_release(wake_sem_);
    }
}

void RtThread::signal_dispatcher_from_isr() noexcept
{
    /* rt_sem_release is ISR-safe in RT-Thread. */
    if (nullptr != wake_sem_) {
        rt_sem_release(wake_sem_);
    }
}

void RtThread::dispatcher_thread_entry(void* param) noexcept
{
    RtThread* self = static_cast<RtThread*>(param);

    /* Tag the dispatcher thread context so current_context() returns
       ContextKind::Dispatcher for events posted from within an AO action. */
    rt_thread_t tid = rt_thread_self();
    if (nullptr != tid) {
        ExecutionContext* ctx = alloc_ctx(0U);
        if (nullptr != ctx) {
            ctx->kind       = ContextKind::Dispatcher;
            ctx->prio_valid = false;
            tid->user_data  = reinterpret_cast<rt_ubase_t>(ctx);
        }
    }

    /* Run the user-supplied entry (Dispatcher::run). */
    self->user_entry_(self->user_ctx_);

    /* Signal join_dispatcher() that the thread has finished. */
    if (nullptr != self->join_sem_) {
        rt_sem_release(self->join_sem_);
    }
}

void RtThread::start_dispatcher(ThreadEntry entry, void* context) noexcept
{
    user_entry_ = entry;
    user_ctx_   = context;

    dispatcher_thread_ = rt_thread_create(
        "coact_disp",
        &RtThread::dispatcher_thread_entry,
        this,
        /* stack size */ 4096U,
        /* priority  */ 10U,
        /* timeslice */ 10U);

    if (nullptr != dispatcher_thread_) {
        rt_thread_startup(dispatcher_thread_);
        thread_started_ = true;
    }
}

void RtThread::join_dispatcher() noexcept
{
    if (thread_started_ && nullptr != join_sem_) {
        rt_sem_take(join_sem_, static_cast<rt_int32_t>(RT_WAITING_FOREVER));
        thread_started_ = false;
    }
}

void RtThread::watchdog_progress(uint32_t /*marker*/) noexcept
{
    /* No-op: RS500 watchdog management is handled at the BSP layer. */
}

void RtThread::enter_direct() noexcept
{
    ExecutionContext* ctx = tls_ctx();
    if (nullptr != ctx) {
        ++ctx->direct_depth;
    }
}

void RtThread::leave_direct() noexcept
{
    ExecutionContext* ctx = tls_ctx();
    if (nullptr != ctx && ctx->direct_depth > 0U) {
        --ctx->direct_depth;
    }
}

}  // namespace pal
}  // namespace coact
