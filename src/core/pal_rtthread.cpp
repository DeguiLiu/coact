// coact RT-Thread PAL implementation (staticized, design §7.5).
// SPDX-License-Identifier: MIT
//
// Static resources are owned by the caller (RtThreadResources<StackBytes,
// ContextSlots>): static struct rt_thread, RT_ALIGN_SIZE-aligned Dispatcher
// stack, two static struct rt_semaphore, fixed ContextSlot[N]. The PAL holds
// only references. The constructor never calls kernel API; explicit
// initialize() runs rt_sem_init + rt_thread_init in task context and returns a
// definite InitError on any failure. start_dispatcher() returns kOk only after
// rt_thread_startup()==RT_EOK. Lifecycle is one init / one start / one stop;
// restart is rejected.
// No rt_sem_create / rt_thread_create / rt_malloc are used.
#include "coact/pal_rtthread.hpp"

namespace coact {
namespace pal {

namespace {

/* Host-test convenience resource for the default ctor. Production boards pass
   explicit RtThreadResources so this 16 KiB static never exists in firmware. */
RtThreadResources<16384U, 8U>& default_resources() noexcept
{
    static RtThreadResources<16384U, 8U> res;
    return res;
}

/* TCB of the running Dispatcher thread, captured in dispatcher_thread_entry.
   in_dispatcher_thread() compares rt_thread_self() against it - no thread_local,
   so a no-TLS ARM target (QEMU) never touches the TLS runtime. */
rt_thread_t g_dispatcher_tcb = nullptr;

}  // namespace

RtThread::RtThread() noexcept
    : res_(&default_resources()),
      user_entry_(nullptr),
      user_ctx_(nullptr),
      state_(Lifecycle::kUninitialized),
      last_error_(InitError::kOk),
      dispatcher_stack_bytes_(4096U),
      clock_ops_(tick_clock_ops())
{
}

void RtThread::set_dispatcher_stack_bytes(uint32_t bytes) noexcept
{
    dispatcher_stack_bytes_ = bytes;
}

uint32_t RtThread::dispatcher_stack_bytes() const noexcept
{
    return dispatcher_stack_bytes_;
}

void RtThread::set_clock_ops(ClockOps ops) noexcept
{
    clock_ops_ = ops;
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

/* ---------------------------------------------------------------------------
 * Fixed ContextSlot table lookups (design §7.5). Registration is the only path
 * that needs the irq-mask critical section (dedup + occupy a free slot);
 * reads (current_context / tls_ctx) are lock-free because a slot is owned by
 * its registering thread and the table is frozen after start.
 * ------------------------------------------------------------------------- */

ContextSlot* RtThread::find_slot(RtThreadResourcesBase* res,
                                 rt_thread_t t) noexcept
{
    if (nullptr == res || nullptr == res->slot_table) {
        return nullptr;
    }
    for (uint16_t i = 0U; i < res->slot_count; ++i) {
        if (res->slot_table[i].tid == t) {
            return &res->slot_table[i];
        }
    }
    return nullptr;
}

ContextSlot* RtThread::alloc_slot(RtThreadResourcesBase* res,
                                  rt_thread_t t) noexcept
{
    if (nullptr == res || nullptr == res->slot_table) {
        return nullptr;
    }
    ContextSlot* free_slot = nullptr;
    for (uint16_t i = 0U; i < res->slot_count; ++i) {
        if (nullptr == res->slot_table[i].tid) {
            free_slot = &res->slot_table[i];
            break;
        }
    }
    if (nullptr != free_slot) {
        free_slot->tid = t;
        free_slot->ctx.kind         = ContextKind::Task;
        free_slot->ctx.logical_prio = 0U;
        free_slot->ctx.prio_valid   = false;
        free_slot->ctx.direct_depth = 0U;
    }
    return free_slot;
}

ExecutionContext* RtThread::tls_ctx() const noexcept
{
    rt_thread_t self = rt_thread_self();
    if (nullptr == self) {
        return nullptr;
    }
    ContextSlot* slot = find_slot(res_, self);
    return (nullptr != slot) ? &slot->ctx : nullptr;
}

InitError RtThread::ensure_initialized() noexcept
{
    if (Lifecycle::kUninitialized == state_) {
        return initialize();
    }
    if (Lifecycle::kInitFailed == state_) {
        return last_error_;
    }
    return InitError::kOk;
}

InitError RtThread::initialize() noexcept
{
    if (Lifecycle::kReady == state_) {
        return InitError::kOk;   /* idempotent once initialized */
    }
    if (Lifecycle::kInitFailed == state_) {
        return last_error_;      /* cached failure, no retry */
    }
    if (Lifecycle::kStarted == state_ || Lifecycle::kStopped == state_) {
        return InitError::kAlreadyInitialized;
    }

    uint32_t stack = dispatcher_stack_bytes_;
    if (stack < 512U) {
        stack = 512U;
    }
    if (stack > res_->stack_bytes) {
        /* Design §7.5: a stack request larger than the resource is an error,
           never a silent clamp. */
        last_error_ = InitError::kStackTooLarge;
        state_      = Lifecycle::kInitFailed;
        return last_error_;
    }

    /* Clear the fixed ContextSlot table under the irq mask: registration is
       frozen after start, so each init begins with a fresh table. */
    const rt_base_t key = rt_hw_interrupt_disable();
    if (nullptr != res_->slot_table) {
        for (uint16_t i = 0U; i < res_->slot_count; ++i) {
            res_->slot_table[i].tid         = nullptr;
            res_->slot_table[i].ctx.kind         = ContextKind::Task;
            res_->slot_table[i].ctx.logical_prio = 0U;
            res_->slot_table[i].ctx.prio_valid   = false;
            res_->slot_table[i].ctx.direct_depth = 0U;
        }
    }
    rt_hw_interrupt_enable(key);

    /* Static semaphores: rt_sem_init in task context (ISR-safe release only).
       Any failure returns a definite InitError - no dynamic fallback. */
    if (RT_EOK != rt_sem_init(res_->wake_sem_obj, "coact_wake", 0U,
                              RT_IPC_FLAG_PRIO)) {
        last_error_ = InitError::kSemInitFailed;
        state_      = Lifecycle::kInitFailed;
        return last_error_;
    }
    if (RT_EOK != rt_sem_init(res_->join_sem_obj, "coact_join", 0U,
                              RT_IPC_FLAG_PRIO)) {
        last_error_ = InitError::kSemInitFailed;
        state_      = Lifecycle::kInitFailed;
        return last_error_;
    }

    const rt_err_t terr = rt_thread_init(
        res_->thread_obj, "coact_disp", &RtThread::dispatcher_thread_entry,
        this, res_->stack_base, stack, 10U, 10U);
    if (RT_EOK != terr) {
        last_error_ = InitError::kThreadInitFailed;
        state_      = Lifecycle::kInitFailed;
        return last_error_;
    }

    state_ = Lifecycle::kReady;
    return InitError::kOk;
}

bool RtThread::register_current_task(LogicalPrio prio) noexcept
{
    if (kInvalidPrio == prio || prio > kMaxPrio) {
        return false;
    }
    if (InitError::kOk != ensure_initialized()) {
        return false;
    }
    rt_thread_t self = rt_thread_self();
    if (nullptr == self) {
        return false;  /* called from ISR or before scheduler start */
    }

    /* Short irq-mask critical section: dedup + occupy a free slot, and check
       the frozen flag (design §7.5). */
    const rt_base_t key = rt_hw_interrupt_disable();
    ContextSlot* slot = nullptr;
    if (Lifecycle::kStarted == state_ || Lifecycle::kStopped == state_) {
        slot = nullptr;   /* frozen after start */
    }
    else if (nullptr != find_slot(res_, self)) {
        slot = nullptr;   /* same tid already registered: rejected */
    }
    else {
        slot = alloc_slot(res_, self);
    }
    rt_hw_interrupt_enable(key);

    if (nullptr == slot) {
        return false;
    }
    slot->ctx.kind         = ContextKind::Task;
    slot->ctx.logical_prio = prio;
    slot->ctx.prio_valid   = true;
    slot->ctx.direct_depth = 0U;
    return true;
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
    /* Dispatcher: identified by comparing rt_thread_self() with its own static
       TCB (design §7.5) - it needs no ContextSlot, so a full table cannot
       block it. */
    const rt_thread_t self = rt_thread_self();
    if (nullptr != self && self == res_->thread_obj) {
        ExecutionContext ctx;
        ctx.kind         = ContextKind::Dispatcher;
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
    const uint64_t hz = clock_ops_.frequency_hz;
    if (0U == hz) {
        return 0U;
    }
    const uint64_t counter = clock_ops_.read_counter(clock_ops_.ctx);
    /* 32-bit counter @ 10 MHz wraps at ~4.29e9 ticks -> product ~4.29e18,
       which fits uint64; the caller extends the high word across wraps. */
    return (counter * 1000000000ULL) / hz;
}

uint64_t RtThread::clock_resolution_ns() const noexcept
{
    const uint64_t hz = clock_ops_.frequency_hz;
    return (0U == hz) ? 0U : (1000000000ULL / hz);
}

void RtThread::wait_dispatcher(uint32_t timeout_ms) noexcept
{
    if (InitError::kOk != ensure_initialized()) {
        return;   /* never initialized: nothing to wait on */
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
    rt_sem_take(res_->wake_sem_obj, ticks);
}

void RtThread::signal_dispatcher_from_task() noexcept
{
    if (InitError::kOk != ensure_initialized()) {
        return;
    }
    rt_sem_release(res_->wake_sem_obj);
}

void RtThread::signal_dispatcher_from_isr() noexcept
{
    /* rt_sem_release is ISR-safe in RT-Thread. No lazy initialize(): rt_sem_init
       is not ISR-safe, and an ISR firing before init is an invalid usage. */
    rt_sem_release(res_->wake_sem_obj);
}

void RtThread::dispatcher_thread_entry(void* param) noexcept
{
    RtThread* self = static_cast<RtThread*>(param);

    /* Capture this thread's TCB: the non-forgeable Dispatcher identity that
       in_dispatcher_thread() reads (TCB comparison, no TLS required). */
    g_dispatcher_tcb = rt_thread_self();

    /* Run the user-supplied entry (Dispatcher::run). The Dispatcher context is
       identified by current_context() via the static TCB comparison, so no
       ContextSlot registration is needed here. */
    self->user_entry_(self->user_ctx_);

    /* Signal join_dispatcher() that the thread has finished. */
    rt_sem_release(self->res_->join_sem_obj);
}

rt_thread_t RtThread::dispatcher_tcb() noexcept
{
    return g_dispatcher_tcb;
}

InitError RtThread::start_dispatcher(ThreadEntry entry, void* context) noexcept
{
    if (InitError::kOk != ensure_initialized()) {
        return last_error_;   /* propagates a cached init failure */
    }
    if (Lifecycle::kStarted == state_ || Lifecycle::kStopped == state_) {
        return InitError::kAlreadyStarted;   /* one start only; no restart */
    }

    user_entry_ = entry;
    user_ctx_   = context;

    /* Design §7.5: start_dispatcher returns status; only
       rt_thread_startup()==RT_EOK enters the started state. */
    const rt_err_t err = rt_thread_startup(res_->thread_obj);
    if (RT_EOK != err) {
        last_error_ = InitError::kThreadStartFailed;
        state_      = Lifecycle::kInitFailed;
        return last_error_;
    }
    state_ = Lifecycle::kStarted;
    return InitError::kOk;
}

void RtThread::join_dispatcher() noexcept
{
    if (Lifecycle::kStarted == state_) {
        rt_sem_take(res_->join_sem_obj,
                    static_cast<rt_int32_t>(RT_WAITING_FOREVER));
        state_ = Lifecycle::kStopped;   /* terminal: no restart */
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
