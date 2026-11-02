// coact FreeRTOS PAL implementation (design docs/design_freertos_pal_zh.md).
// SPDX-License-Identifier: MIT
//
// Static resources only: FreeRtosResources owns every TCB, stack, semaphore
// and context slot. The constructor saves references; initialize() creates the
// kernel objects in task context. No dynamic allocation is reachable from this
// translation unit.
#include "coact/pal_freertos.hpp"

#include <cstdint>

namespace coact {
namespace pal {

namespace {

/* Host-test convenience only. Production boards pass explicit resources, so
   this 20 KiB image is not linked into firmware builds that never use the
   default constructor. */
FreeRtosResources<16384U, 8U, 8U, 2048U>& default_resources() noexcept
{
    static FreeRtosResources<16384U, 8U, 8U, 2048U> res;
    return res;
}

}  // namespace

FreeRtos::FreeRtos() noexcept
    : res_(&default_resources()),
      user_entry_(nullptr),
      user_ctx_(nullptr),
      state_(Lifecycle::kUninitialized),
      last_error_(InitError::kOk),
      dispatcher_stack_bytes_(4096U),
      clock_ops_(tick_clock_ops()),
      ns_per_counter_(detail::exact_ns_per_counter(
          static_cast<uint32_t>(configTICK_RATE_HZ)))
{
}

/* ---------------------------------------------------------------------------
 * Interrupt masking: one entry point, two contexts. An ISR must never run
 * taskENTER_CRITICAL(): the kernel would decrement a nesting counter it never
 * incremented. The low bit of the token records which path was taken.
 * ------------------------------------------------------------------------- */
CriticalToken FreeRtos::irq_save() noexcept
{
    CriticalToken tok{};
    if (freertos_in_isr()) {
        const UBaseType_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
        tok.value = (static_cast<uintptr_t>(mask) << 1U) | 1U;
    }
    else {
        taskENTER_CRITICAL();
        tok.value = 0U;
    }
    return tok;
}

void FreeRtos::irq_restore(CriticalToken token) noexcept
{
    if ((token.value & 1U) != 0U) {
        portCLEAR_INTERRUPT_MASK_FROM_ISR(
            static_cast<UBaseType_t>(token.value >> 1U));
    }
    else {
        taskEXIT_CRITICAL();
    }
}

/* ---------------------------------------------------------------------------
 * Context table lookups. Registration is serialized by a critical section;
 * reads are lock-free because a slot is owned by its task and the table is
 * frozen after start.
 * ------------------------------------------------------------------------- */
ContextSlot* FreeRtos::find_slot(FreeRtosResourcesBase* res,
                                 TaskHandle_t task) noexcept
{
    if ((nullptr == res) || (nullptr == res->slot_table)) {
        return nullptr;
    }
    for (uint16_t i = 0U; i < res->slot_count; ++i) {
        if (res->slot_table[i].task == task) {
            return &res->slot_table[i];
        }
    }
    return nullptr;
}

ContextSlot* FreeRtos::alloc_slot(FreeRtosResourcesBase* res,
                                  TaskHandle_t task) noexcept
{
    if ((nullptr == res) || (nullptr == res->slot_table)) {
        return nullptr;
    }
    for (uint16_t i = 0U; i < res->slot_count; ++i) {
        ContextSlot& slot = res->slot_table[i];
        if (nullptr == slot.task) {
            slot.task = task;
            slot.ctx.kind = ContextKind::Task;
            slot.ctx.logical_prio = 0U;
            slot.ctx.prio_valid = false;
            slot.ctx.direct_depth = 0U;
            return &slot;
        }
    }
    return nullptr;
}

ExecutionContext* FreeRtos::tls_ctx() const noexcept
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (nullptr == self) {
        return nullptr;
    }
    ContextSlot* slot = find_slot(res_, self);
    return (nullptr != slot) ? &slot->ctx : nullptr;
}

InitError FreeRtos::ensure_initialized() noexcept
{
    if (Lifecycle::kUninitialized == state_) {
        return initialize();
    }
    if (Lifecycle::kInitFailed == state_) {
        return last_error_;
    }
    return InitError::kOk;
}

InitError FreeRtos::initialize() noexcept
{
    if (Lifecycle::kReady == state_) {
        return InitError::kOk;
    }
    if (Lifecycle::kInitFailed == state_) {
        return last_error_;
    }
    if ((Lifecycle::kStarted == state_) || (Lifecycle::kStopped == state_)) {
        return InitError::kAlreadyInitialized;
    }
    /* Two producers can reach the lazy path before start_dispatcher(). Only
       the first runs the kernel calls; the second gets a definite error
       instead of re-initializing the same TCB/semaphore storage. */
    if (init_guard_.test_and_set(std::memory_order_acquire)) {
        return InitError::kAlreadyInitialized;
    }
    struct ClearGuard {
        std::atomic_flag& flag;
        ~ClearGuard() { flag.clear(std::memory_order_release); }
    } clear_guard{init_guard_};

    uint32_t stack = dispatcher_stack_bytes_;
    if (stack < kFreeRtosMinDispatcherStackBytes) {
        stack = kFreeRtosMinDispatcherStackBytes;
    }
    if (stack > res_->dispatcher_stack_bytes) {
        last_error_ = InitError::kStackTooLarge;
        state_ = Lifecycle::kInitFailed;
        return last_error_;
    }
    /* start_dispatcher() derives the task depth from this field; keep the
       clamped value, not the unclamped request. */
    dispatcher_stack_bytes_ = stack;

    /* Fresh table: registration is frozen after start, so each PAL starts
       from a known state. This runs in task context, so the task-side
       critical section is the correct one. */
    taskENTER_CRITICAL();
    if (nullptr != res_->slot_table) {
        for (uint16_t i = 0U; i < res_->slot_count; ++i) {
            res_->slot_table[i].task = nullptr;
            res_->slot_table[i].ctx.kind = ContextKind::Task;
            res_->slot_table[i].ctx.logical_prio = 0U;
            res_->slot_table[i].ctx.prio_valid = false;
            res_->slot_table[i].ctx.direct_depth = 0U;
        }
    }
    taskEXIT_CRITICAL();

    res_->wake = xSemaphoreCreateBinaryStatic(res_->wake_storage);
    if (nullptr == res_->wake) {
        last_error_ = InitError::kSemInitFailed;
        state_ = Lifecycle::kInitFailed;
        return last_error_;
    }
    res_->join = xSemaphoreCreateBinaryStatic(res_->join_storage);
    if (nullptr == res_->join) {
        vSemaphoreDelete(res_->wake);
        res_->wake = nullptr;
        last_error_ = InitError::kSemInitFailed;
        state_ = Lifecycle::kInitFailed;
        return last_error_;
    }

    /* Resident workers (design D3): created once, never deleted or recycled.
       They block on their start semaphore until thread_create() hands work
       over. A failure rolls back the workers created so far. */
    uint16_t created = 0U;
    for (; created < res_->worker_slot_count; ++created) {
        WorkerSlot& worker = res_->worker_slots[created];
        worker.busy = false;
        worker.entry = nullptr;
        worker.context = nullptr;
        worker.start = xSemaphoreCreateBinaryStatic(&worker.start_storage);
        worker.join = xSemaphoreCreateBinaryStatic(&worker.join_storage);
        if ((nullptr == worker.start) || (nullptr == worker.join)) {
            break;
        }
        worker.task = xTaskCreateStatic(
            &FreeRtos::worker_thread_entry, "coact_work",
            worker.stack_words, &worker, res_->worker_priority,
            worker.stack, &worker.tcb);
        if (nullptr == worker.task) {
            break;
        }
    }
    if (created != res_->worker_slot_count) {
        /* Include the slot that failed: its start semaphore may already
           exist while its join semaphore or task does not. */
        for (uint16_t i = 0U; i <= created && i < res_->worker_slot_count; ++i) {
            WorkerSlot& worker = res_->worker_slots[i];
            if (nullptr != worker.task) {
                vTaskDelete(worker.task);
                worker.task = nullptr;
            }
            if (nullptr != worker.start) {
                vSemaphoreDelete(worker.start);
                worker.start = nullptr;
            }
            if (nullptr != worker.join) {
                vSemaphoreDelete(worker.join);
                worker.join = nullptr;
            }
        }
        vSemaphoreDelete(res_->join);
        vSemaphoreDelete(res_->wake);
        res_->join = nullptr;
        res_->wake = nullptr;
        last_error_ = InitError::kThreadInitFailed;
        state_ = Lifecycle::kInitFailed;
        return last_error_;
    }

    state_ = Lifecycle::kReady;
    return InitError::kOk;
}

bool FreeRtos::register_current_task(LogicalPrio prio) noexcept
{
    if ((kInvalidPrio == prio) || (prio > kMaxPrio)) {
        return false;
    }
    if (InitError::kOk != ensure_initialized()) {
        return false;
    }
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (nullptr == self) {
        return false;  /* before the scheduler started */
    }

    taskENTER_CRITICAL();
    ContextSlot* slot = nullptr;
    if ((Lifecycle::kStarted == state_) || (Lifecycle::kStopped == state_)) {
        slot = nullptr;  /* frozen after start */
    }
    else if (nullptr != find_slot(res_, self)) {
        slot = nullptr;  /* already registered */
    }
    else {
        slot = alloc_slot(res_, self);
    }
    taskEXIT_CRITICAL();

    if (nullptr == slot) {
        return false;
    }
    slot->ctx.kind = ContextKind::Task;
    slot->ctx.logical_prio = prio;
    slot->ctx.prio_valid = true;
    slot->ctx.direct_depth = 0U;

#if defined(INCLUDE_vTaskPrioritySet) && (INCLUDE_vTaskPrioritySet == 1)
    /* Design D5: never hand a LogicalPrio to FreeRTOS directly; map into
       1..configMAX_PRIORITIES-1 and leave 0 for the idle task. */
    vTaskPrioritySet(nullptr, detail::map_logical_prio(prio));
#endif
    return true;
}

ExecutionContext FreeRtos::current_context() const noexcept
{
    ExecutionContext ctx{};
    if (freertos_in_isr()) {
        ctx.kind = ContextKind::Isr;
        return ctx;
    }
    if (in_dispatcher_thread()) {
        ctx.kind = ContextKind::Dispatcher;
        return ctx;
    }
    ExecutionContext* stored = tls_ctx();
    if (nullptr != stored) {
        return *stored;
    }
    ctx.kind = ContextKind::Task;
    return ctx;
}

bool FreeRtos::in_dispatcher_thread() const noexcept
{
    const TaskHandle_t dispatcher = dispatcher_handle_;
    return (nullptr != dispatcher) && (xTaskGetCurrentTaskHandle() == dispatcher);
}

/* ---------------------------------------------------------------------------
 * Clock: 32-bit tick extended to 64 bits under the irq mask, same algorithm
 * as the RT-Thread PAL. A board may inject a hardware counter; the extension
 * then uses that counter's declared width.
 * ------------------------------------------------------------------------- */
uint64_t FreeRtos::read_extended_counter() const noexcept
{
    const uint8_t bits = clock_ops_.counter_bits;
    if ((0U == bits) || (bits >= 64U)) {
        return clock_ops_.read_counter(clock_ops_.ctx);
    }

    FreeRtos* const self = const_cast<FreeRtos*>(this);
    const CriticalToken token = self->irq_save();
    const uint64_t modulus = uint64_t{1U} << bits;
    const uint64_t counter =
        clock_ops_.read_counter(clock_ops_.ctx) & (modulus - 1U);
    if (counter_seen_ && (counter < last_counter_)) {
        counter_epoch_ += modulus;
    }
    last_counter_ = counter;
    counter_seen_ = true;
    const uint64_t extended = counter_epoch_ + counter;
    self->irq_restore(token);
    return extended;
}

uint64_t FreeRtos::monotonic_ns() const noexcept
{
    const uint64_t hz = clock_ops_.frequency_hz;
    if (0U == hz) {
        return 0U;
    }
    const uint64_t counter = read_extended_counter();
    if (0U != ns_per_counter_) {
        return counter * ns_per_counter_;
    }
    const uint64_t seconds = counter / hz;
    const uint64_t remainder = counter % hz;
    return (seconds * 1000000000ULL)
         + ((remainder * 1000000000ULL) / hz);
}

uint64_t FreeRtos::clock_resolution_ns() const noexcept
{
    const uint64_t hz = clock_ops_.frequency_hz;
    return (0U == hz) ? 0U : (1000000000ULL / hz);
}

void FreeRtos::set_clock_ops(ClockOps ops) noexcept
{
    clock_ops_ = ops;
    ns_per_counter_ = detail::exact_ns_per_counter(ops.frequency_hz);
    counter_epoch_ = 0U;
    last_counter_ = 0U;
    counter_seen_ = false;
}

/* ---------------------------------------------------------------------------
 * Dispatcher wait/signal.
 * ------------------------------------------------------------------------- */
void FreeRtos::wait_dispatcher(uint32_t timeout_ms) noexcept
{
    /* Design: before initialize() there is no semaphore to wait on, and this
       path must never create kernel objects (a producer can reach it before
       the scheduler owns the PAL). */
    if (Lifecycle::kUninitialized == state_) {
        return;
    }
    if (InitError::kOk != ensure_initialized()) {
        return;
    }
    (void)xSemaphoreTake(res_->wake, detail::wait_ticks(timeout_ms));
}

void FreeRtos::signal_dispatcher_from_task() noexcept
{
    if (Lifecycle::kUninitialized == state_) {
        return;
    }
    if (InitError::kOk != ensure_initialized()) {
        return;
    }
    (void)xSemaphoreGive(res_->wake);
}

void FreeRtos::signal_dispatcher_from_isr() noexcept
{
    if (nullptr == res_->wake) {
        return;  /* ISR before initialize() is invalid usage, but never crash */
    }
    BaseType_t woken = pdFALSE;
    (void)xSemaphoreGiveFromISR(res_->wake, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ---------------------------------------------------------------------------
 * Dispatcher lifecycle.
 * ------------------------------------------------------------------------- */
void FreeRtos::dispatcher_thread_entry(void* param) noexcept
{
    FreeRtos* self = static_cast<FreeRtos*>(param);
    self->dispatcher_handle_ = xTaskGetCurrentTaskHandle();

    if (nullptr != self->user_entry_) {
        self->user_entry_(self->user_ctx_);
    }

    (void)xSemaphoreGive(self->res_->join);
    vTaskDelete(nullptr);
    /* vTaskDelete() does not return. This loop only keeps compilers and ports
       that fall through from running off the end of a task function. */
    for (;;) {
        taskYIELD();
    }
}

void FreeRtos::worker_thread_entry(void* param) noexcept
{
    WorkerSlot* slot = static_cast<WorkerSlot*>(param);
    for (;;) {
        (void)xSemaphoreTake(slot->start, portMAX_DELAY);
        ThreadEntry entry = slot->entry;
        void* context = slot->context;
        if (nullptr != entry) {
            entry(context);
        }
        (void)xSemaphoreGive(slot->join);
    }
}

InitError FreeRtos::start_dispatcher(ThreadEntry entry, void* context) noexcept
{
    if (InitError::kOk != ensure_initialized()) {
        return last_error_;
    }
    if ((Lifecycle::kStarted == state_) || (Lifecycle::kStopped == state_)) {
        return InitError::kAlreadyStarted;
    }
    /* Design D9: the scheduler must already be running. The current-task
       handle is not a reliable probe (creating the bootstrap task already
       populates pxCurrentTCB), so ask the scheduler state directly. */
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        last_error_ = InitError::kThreadStartFailed;
        state_ = Lifecycle::kInitFailed;
        return last_error_;
    }

    user_entry_ = entry;
    user_ctx_ = context;

    const uint32_t words =
        (dispatcher_stack_bytes_ + sizeof(StackType_t) - 1U) / sizeof(StackType_t);
    if (words > res_->dispatcher_stack_words) {
        last_error_ = InitError::kStackTooLarge;
        state_ = Lifecycle::kInitFailed;
        return last_error_;
    }
    TaskHandle_t handle = xTaskCreateStatic(
        &FreeRtos::dispatcher_thread_entry, "coact_disp", words, this,
        dispatcher_priority(), res_->dispatcher_stack, res_->dispatcher_tcb);
    if (nullptr == handle) {
        last_error_ = InitError::kThreadStartFailed;
        state_ = Lifecycle::kInitFailed;
        return last_error_;
    }
    state_ = Lifecycle::kStarted;
    return InitError::kOk;
}

void FreeRtos::join_dispatcher() noexcept
{
    if (Lifecycle::kStarted != state_) {
        return;
    }
    (void)xSemaphoreTake(res_->join, portMAX_DELAY);
    state_ = Lifecycle::kStopped;
}

void FreeRtos::watchdog_progress(uint32_t /*marker*/) noexcept
{
    /* Watchdog ownership stays at the BSP layer. */
}

void FreeRtos::set_dispatcher_stack_bytes(uint32_t bytes) noexcept
{
    dispatcher_stack_bytes_ = bytes;
}

uint32_t FreeRtos::dispatcher_stack_bytes() const noexcept
{
    return dispatcher_stack_bytes_;
}

void FreeRtos::enter_direct() noexcept
{
    ExecutionContext* ctx = tls_ctx();
    if (nullptr != ctx) {
        ++ctx->direct_depth;
    }
}

void FreeRtos::leave_direct() noexcept
{
    ExecutionContext* ctx = tls_ctx();
    if ((nullptr != ctx) && (ctx->direct_depth > 0U)) {
        --ctx->direct_depth;
    }
}

/* ---------------------------------------------------------------------------
 * SemOps: static counting/binary semaphore embedded in the caller's handle.
 * 0 ms is the non-blocking try; kWaitForever is the only infinite value.
 * ------------------------------------------------------------------------- */
bool FreeRtos::sem_init(SemHandle& sem, uint32_t initial) noexcept
{
    /* SemOps is a COUNTING semaphore, including for initial 0/1. A binary
       semaphore would cap the count at one, so three releases followed by
       three takes would lose two tokens. */
    const UBaseType_t count = static_cast<UBaseType_t>(
        (initial > 0xFFFFU) ? 0xFFFFU : initial);
    sem.handle = xSemaphoreCreateCountingStatic(0xFFFFU, count, &sem.storage);
    sem.pal = this;
    return nullptr != sem.handle;
}

bool FreeRtos::sem_take(SemHandle& sem, uint32_t timeout_ms) noexcept
{
    if (nullptr == sem.handle) {
        return false;
    }
    const TickType_t ticks = (kWaitForever == timeout_ms)
        ? static_cast<TickType_t>(portMAX_DELAY)
        : detail::ms_to_ticks(timeout_ms);
    return pdTRUE == xSemaphoreTake(sem.handle, ticks);
}

void FreeRtos::sem_release(SemHandle& sem) noexcept
{
    if (nullptr != sem.handle) {
        (void)xSemaphoreGive(sem.handle);
    }
}

void FreeRtos::sem_release_from_isr(SemHandle& sem) noexcept
{
    if (nullptr == sem.handle) {
        return;
    }
    BaseType_t woken = pdFALSE;
    (void)xSemaphoreGiveFromISR(sem.handle, &woken);
    portYIELD_FROM_ISR(woken);
}

void FreeRtos::sem_deinit(SemHandle& sem) noexcept
{
    if (nullptr != sem.handle) {
        vSemaphoreDelete(sem.handle);
        sem.handle = nullptr;
    }
}

/* ---------------------------------------------------------------------------
 * MutexOps.
 * ------------------------------------------------------------------------- */
bool FreeRtos::mutex_init(MutexHandle& m) noexcept
{
    m.handle = xSemaphoreCreateMutexStatic(&m.storage);
    m.pal = this;
    return nullptr != m.handle;
}

void FreeRtos::mutex_lock(MutexHandle& m) noexcept
{
    if (nullptr != m.handle) {
        (void)xSemaphoreTake(m.handle, portMAX_DELAY);
    }
}

void FreeRtos::mutex_unlock(MutexHandle& m) noexcept
{
    if (nullptr != m.handle) {
        (void)xSemaphoreGive(m.handle);
    }
}

void FreeRtos::mutex_deinit(MutexHandle& m) noexcept
{
    if (nullptr != m.handle) {
        vSemaphoreDelete(m.handle);
        m.handle = nullptr;
    }
}

/* ---------------------------------------------------------------------------
 * CondOps: counting semaphore + waiter count, pthread_cond_wait discipline.
 * cond_wait releases the mutex first, then blocks, so signaling while holding
 * the mutex cannot deadlock (the token is stored in the counting semaphore);
 * the discipline of signaling after unlock only shortens the waiter's
 * re-acquire delay.
 * ------------------------------------------------------------------------- */
bool FreeRtos::cond_init(CondHandle& c) noexcept
{
    c.handle = xSemaphoreCreateCountingStatic(0xFFFFU, 0U, &c.storage);
    c.waiters.store(0U, std::memory_order_relaxed);
    c.pal = this;
    return nullptr != c.handle;
}

void FreeRtos::cond_wait(CondHandle& c, MutexHandle& m,
                         uint32_t timeout_ms) noexcept
{
    if ((nullptr == c.handle) || (nullptr == m.handle)) {
        return;
    }
    c.waiters.fetch_add(1U, std::memory_order_acq_rel);
    (void)xSemaphoreGive(m.handle);  /* release the paired mutex */
    (void)xSemaphoreTake(c.handle, detail::wait_ticks(timeout_ms));
    c.waiters.fetch_sub(1U, std::memory_order_acq_rel);
    (void)xSemaphoreTake(m.handle, portMAX_DELAY);
}

void FreeRtos::cond_signal(CondHandle& c) noexcept
{
    if (nullptr == c.handle) {
        return;
    }
    /* Unconditional release, as in the RT-Thread PAL: stray tokens are
       absorbed by the caller's while-loop predicate re-check. */
    (void)xSemaphoreGive(c.handle);
}

void FreeRtos::cond_broadcast(CondHandle& c) noexcept
{
    if (nullptr == c.handle) {
        return;
    }
    uint32_t waiters = c.waiters.load(std::memory_order_acquire);
    while (waiters-- > 0U) {
        (void)xSemaphoreGive(c.handle);
    }
}

void FreeRtos::cond_deinit(CondHandle& c) noexcept
{
    if (nullptr != c.handle) {
        vSemaphoreDelete(c.handle);
        c.handle = nullptr;
    }
}

/* ---------------------------------------------------------------------------
 * ThreadOps: resident workers, never deleted. join() only observes the join
 * semaphore and frees the slot for the next create().
 * ------------------------------------------------------------------------- */
bool FreeRtos::thread_create(ThreadHandle& t, ThreadEntry entry,
                             void* context) noexcept
{
    t.valid = false;
    if ((nullptr == entry) || (nullptr == res_->worker_slots)) {
        return false;
    }
    /* Claim the slot and publish its work under one critical section: two
       threads racing here would otherwise both see the same free slot, give
       the same binary start semaphore twice and leave one join() blocked
       forever. */
    WorkerSlot* chosen = nullptr;
    const CriticalToken token = irq_save();
    for (uint16_t i = 0U; i < res_->worker_slot_count; ++i) {
        WorkerSlot& slot = res_->worker_slots[i];
        if (slot.busy) {
            continue;
        }
        slot.entry = entry;
        slot.context = context;
        slot.busy = true;
        chosen = &slot;
        break;
    }
    irq_restore(token);
    if (nullptr == chosen) {
        return false;
    }

    t.pal = this;
    t.entry = entry;
    t.context = context;
    t.slot_idx = static_cast<uint16_t>(chosen - res_->worker_slots);
    t.valid = true;
    (void)xSemaphoreGive(chosen->start);
    return true;
}

void FreeRtos::thread_join(ThreadHandle& t) noexcept
{
    if (!t.valid || (nullptr == res_->worker_slots)
        || (t.slot_idx >= res_->worker_slot_count)) {
        return;
    }
    WorkerSlot& slot = res_->worker_slots[t.slot_idx];
    (void)xSemaphoreTake(slot.join, portMAX_DELAY);
    /* Clear the work and publish the slot as free under one critical section,
       in that order. Clearing busy first let another task claim the slot and
       publish new entry/context, which this join then erased. */
    const CriticalToken token = irq_save();
    slot.entry = nullptr;
    slot.context = nullptr;
    slot.busy = false;
    irq_restore(token);
    t.valid = false;
}

/* ---------------------------------------------------------------------------
 * SoftIrqOps: SPSC ring + counting semaphore. No POSIX signals, no task
 * notifications; the payload ring is the only data channel.
 * ------------------------------------------------------------------------- */
bool FreeRtos::softirq_init(SoftIrqHandle& h) noexcept
{
    h.wake = xSemaphoreCreateCountingStatic(kFreeRtosSoftIrqSlots, 0U,
                                            &h.wake_storage);
    h.head.store(0U, std::memory_order_relaxed);
    h.tail.store(0U, std::memory_order_relaxed);
    h.pal = this;
    return nullptr != h.wake;
}

bool FreeRtos::softirq_raise(SoftIrqHandle& h, int32_t payload) noexcept
{
    if (nullptr == h.wake) {
        return false;
    }
    /* Task and ISR are both allowed producers on one handle. Serialize the
       full/insert/publish triple: two interleaved raises could otherwise read
       the same head, overwrite one payload, yet hand out two tokens. */
    const CriticalToken token = irq_save();
    const uint32_t head = h.head.load(std::memory_order_relaxed);
    const uint32_t tail = h.tail.load(std::memory_order_acquire);
    if ((head - tail) >= kFreeRtosSoftIrqSlots) {
        irq_restore(token);
        return false;
    }
    h.ring[head % kFreeRtosSoftIrqSlots] = payload;
    h.head.store(head + 1U, std::memory_order_release);
    irq_restore(token);

    if (freertos_in_isr()) {
        BaseType_t woken = pdFALSE;
        (void)xSemaphoreGiveFromISR(h.wake, &woken);
        portYIELD_FROM_ISR(woken);
    }
    else {
        (void)xSemaphoreGive(h.wake);
    }
    return true;
}

int32_t FreeRtos::softirq_take(SoftIrqHandle& h, uint32_t timeout_ms) noexcept
{
    if (nullptr == h.wake) {
        return -1;
    }
    /* SoftIrq uses the 0 == forever convention, unlike SemOps. */
    if (pdTRUE != xSemaphoreTake(h.wake, detail::wait_ticks(timeout_ms))) {
        return -1;
    }
    const uint32_t head = h.head.load(std::memory_order_acquire);
    const uint32_t tail = h.tail.load(std::memory_order_relaxed);
    if (tail == head) {
        return -1;  /* defensive: token without payload */
    }
    const int32_t payload = h.ring[tail % kFreeRtosSoftIrqSlots];
    h.tail.store(tail + 1U, std::memory_order_release);
    return payload;
}

void FreeRtos::softirq_deinit(SoftIrqHandle& h) noexcept
{
    if (nullptr != h.wake) {
        vSemaphoreDelete(h.wake);
        h.wake = nullptr;
    }
}

void FreeRtos::sleep_us(uint32_t us) noexcept
{
    uint64_t ticks =
        (static_cast<uint64_t>(us) * configTICK_RATE_HZ + 999999U) / 1000000U;
    if ((us > 0U) && (ticks == 0U)) {
        ticks = 1U;
    }
    if (ticks == 0U) {
        taskYIELD();
    }
    else {
        vTaskDelay(static_cast<TickType_t>(ticks));
    }
}

}  // namespace pal
}  // namespace coact
