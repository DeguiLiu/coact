// coact FreeRTOS PAL - concrete platform abstraction for FreeRTOS Kernel
// V11.1.0 LTS (202406-LTS) and V11.3.x.
// SPDX-License-Identifier: MIT
//
// Scope (docs/design_freertos_pal_zh.md, stages A and B):
//   irq_save/restore      -> taskENTER_CRITICAL in tasks, portSET_INTERRUPT_
//                            MASK_FROM_ISR in ISRs (one token, two contexts)
//   monotonic_ns          -> ClockOps table over xTaskGetTickCount(); boards
//                            may inject a hardware counter
//   wait/signal           -> static binary semaphore, ISR-safe give
//   start/join dispatcher -> static TCB + stack, join semaphore, no restart
//   context table         -> fixed ContextSlot[N] keyed by TaskHandle_t
//   Sem/Mutex/Cond        -> static semaphores (counting / mutex / wake pair)
//   ThreadOps             -> resident worker tasks + start/join semaphores;
//                            a FreeRTOS task must never return, and a static
//                            TCB must not be recycled before the idle task has
//                            reaped the old task
//   SoftIrqOps            -> SPSC ring + counting semaphore, no signals
//
// Not covered, by design: TimerScheduler, diag backend, coro. Cortex-M0/M0+
// is excluded because staging/spsc_ring require lock-free byte/half-word
// atomics that ARMv6-M does not provide.
//
// Host testing: build with -DCOACT_FREERTOS_STUB to pull in
// test/freertos_stub.h, a pthread-backed FreeRTOS API subset.
//
// Static resources are owned by the caller (FreeRtosResources). The PAL
// constructor only saves references; initialize() creates every kernel object
// in task context and returns a definite InitError. No pvPortMalloc,
// xTaskCreate or xSemaphoreCreate (dynamic) call exists in this port.
#pragma once

#ifndef __cplusplus
#error "coact/pal_freertos.hpp requires C++"
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

#ifdef COACT_FREERTOS_STUB
#include "test/freertos_stub.h"
#else
#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>
#endif

#include "coact/config.hpp"
#include "coact/pal.hpp"
#include "coact/pool.hpp"
#include "coact/queue.hpp"

/* ---- Configuration gates (design section 7) ---------------------------- */
#if !defined(configNUMBER_OF_CORES) || (configNUMBER_OF_CORES != 1)
#error "coact::pal::FreeRtos requires configNUMBER_OF_CORES == 1"
#endif
#if !defined(configSUPPORT_STATIC_ALLOCATION) || (configSUPPORT_STATIC_ALLOCATION != 1)
#error "coact::pal::FreeRtos requires configSUPPORT_STATIC_ALLOCATION == 1"
#endif
#if !defined(configUSE_MUTEXES) || (configUSE_MUTEXES != 1)
#error "coact::pal::FreeRtos requires configUSE_MUTEXES == 1"
#endif
#if !defined(configUSE_COUNTING_SEMAPHORES) || (configUSE_COUNTING_SEMAPHORES != 1)
#error "coact::pal::FreeRtos requires configUSE_COUNTING_SEMAPHORES == 1"
#endif
#if !defined(INCLUDE_vTaskSuspend) || (INCLUDE_vTaskSuspend != 1)
#error "coact::pal::FreeRtos requires INCLUDE_vTaskSuspend == 1 (portMAX_DELAY must block forever)"
#endif
#if !defined(INCLUDE_vTaskDelay) || (INCLUDE_vTaskDelay != 1)
#error "coact::pal::FreeRtos requires INCLUDE_vTaskDelay == 1 (sleep_us)"
#endif
#if !defined(INCLUDE_xTaskGetSchedulerState) || (INCLUDE_xTaskGetSchedulerState != 1)
#error "coact::pal::FreeRtos requires INCLUDE_xTaskGetSchedulerState == 1"
#endif
#if !defined(INCLUDE_xTaskGetCurrentTaskHandle) || (INCLUDE_xTaskGetCurrentTaskHandle != 1)
#error "coact::pal::FreeRtos requires INCLUDE_xTaskGetCurrentTaskHandle == 1"
#endif
#if !defined(INCLUDE_vTaskDelete) || (INCLUDE_vTaskDelete != 1)
#error "coact::pal::FreeRtos requires INCLUDE_vTaskDelete == 1"
#endif
#if !defined(INCLUDE_vTaskPrioritySet) || (INCLUDE_vTaskPrioritySet != 1)
#error "coact::pal::FreeRtos requires INCLUDE_vTaskPrioritySet == 1 (logical prio mapping)"
#endif
#if defined(configUSE_16_BIT_TICKS) && (configUSE_16_BIT_TICKS == 1)
#error "coact::pal::FreeRtos requires a 32-bit TickType_t"
#endif
#if defined(configTICK_TYPE_WIDTH_IN_BITS) && \
    (configTICK_TYPE_WIDTH_IN_BITS != TICK_TYPE_WIDTH_32_BITS)
#error "coact::pal::FreeRtos requires configTICK_TYPE_WIDTH_IN_BITS == TICK_TYPE_WIDTH_32_BITS"
#endif
#if (configMAX_PRIORITIES < 3)
#error "coact::pal::FreeRtos requires configMAX_PRIORITIES >= 3 (idle, producer, dispatcher)"
#endif

#if !defined(COACT_FREERTOS_STUB) && defined(__ARM_ARCH_6M__)
#error "coact::pal::FreeRtos does not support Cortex-M0/M0+ (no lock-free 8/16-bit atomics)"
#endif

namespace coact {
namespace pal {

// ---------------------------------------------------------------------------
// ISR detection. Cortex-M3/M4/M7 use the port's xPortIsInsideInterrupt().
// Other ports must define COACT_FREERTOS_IN_ISR_HOOK() - silently treating an
// ISR as a task would call taskENTER_CRITICAL() from an ISR and corrupt the
// kernel's critical nesting counter.
// ---------------------------------------------------------------------------
#if defined(COACT_FREERTOS_IN_ISR_HOOK)
inline bool freertos_in_isr() noexcept
{
    return COACT_FREERTOS_IN_ISR_HOOK();
}
#elif defined(COACT_FREERTOS_STUB)
inline bool freertos_in_isr() noexcept
{
    return freertos_stub_in_isr();
}
#elif defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
inline bool freertos_in_isr() noexcept
{
    return xPortIsInsideInterrupt() != pdFALSE;
}
#else
#error "define COACT_FREERTOS_IN_ISR_HOOK() for this FreeRTOS port"
#endif

// Default ClockOps over the FreeRTOS tick. 32-bit tick with extension is
// handled by FreeRtos::read_extended_counter(); boards that need microsecond
// budgets inject a hardware counter through set_clock_ops().
inline uint64_t freertos_tick_counter_read(void* /*ctx*/) noexcept
{
    return freertos_in_isr() ? static_cast<uint64_t>(xTaskGetTickCountFromISR())
                             : static_cast<uint64_t>(xTaskGetTickCount());
}

inline ClockOps tick_clock_ops() noexcept
{
    ClockOps ops;
    ops.read_counter = &freertos_tick_counter_read;
    ops.frequency_hz = static_cast<uint32_t>(configTICK_RATE_HZ);
    ops.ctx = nullptr;
    ops.counter_bits = static_cast<uint8_t>(sizeof(TickType_t) * 8U);
    return ops;
}

namespace detail {

inline constexpr uint64_t exact_ns_per_counter(uint32_t frequency_hz) noexcept
{
    return (frequency_hz != 0U && (1000000000ULL % frequency_hz) == 0U)
        ? (1000000000ULL / frequency_hz)
        : 0U;
}

// Convert milliseconds to ticks, rounding up and never returning 0 for a
// non-zero timeout. pdMS_TO_TICKS() truncates: at 100 Hz a 5 ms batch timeout
// becomes 0 ticks, i.e. a busy poll. Only 0 remains the non-blocking value.
inline constexpr TickType_t ms_to_ticks(uint32_t timeout_ms) noexcept
{
    if (timeout_ms == 0U) {
        return 0U;
    }
    const uint64_t ticks =
        (static_cast<uint64_t>(timeout_ms) * configTICK_RATE_HZ + 999U) / 1000U;
    const uint64_t forever = static_cast<uint64_t>(portMAX_DELAY);
    if (ticks >= forever) {
        return static_cast<TickType_t>(forever - 1U);
    }
    return static_cast<TickType_t>((ticks == 0U) ? 1U : ticks);
}

// Blocking-wait conversion shared by the Dispatcher wait and Cond/SoftIrq:
// 0 and kWaitForever are the only infinite values.
inline constexpr TickType_t wait_ticks(uint32_t timeout_ms) noexcept
{
    return (timeout_ms == 0U || timeout_ms == kWaitForever)
        ? static_cast<TickType_t>(portMAX_DELAY)
        : ms_to_ticks(timeout_ms);
}

inline constexpr UBaseType_t map_logical_prio(LogicalPrio prio) noexcept
{
    const UBaseType_t floor = 1U;
    /* The top application priority belongs to the Dispatcher. A producer that
       mapped to it would time-slice with (or, with time slicing off, starve)
       the only thread that drains the staging queue. */
    const UBaseType_t ceiling =
        static_cast<UBaseType_t>(configMAX_PRIORITIES - 2);
    if (prio < floor) {
        return floor;
    }
    return (prio > ceiling) ? ceiling : static_cast<UBaseType_t>(prio);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Static resource layout (design section 5.5).
// ---------------------------------------------------------------------------
inline constexpr uint32_t kFreeRtosMinDispatcherStackBytes = 512U;
inline constexpr uint32_t kFreeRtosSoftIrqSlots = 8U;

static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "SoftIrq / SPSC indices must stay lock-free (no libatomic)");
static_assert(std::atomic<uint16_t>::is_always_lock_free,
              "SpscRing indices must stay lock-free (no libatomic)");

// Fixed per-task context slot. Keyed by TaskHandle_t, which is unique for the
// lifetime of a static TCB.
struct ContextSlot {
    TaskHandle_t task = nullptr;
    ExecutionContext ctx{};
};

// One resident worker. The task is created in initialize() and loops forever;
// thread_create() only hands it work, thread_join() waits for completion.
// A static TCB is never deleted or recycled.
struct WorkerSlot {
    StaticTask_t tcb{};
    TaskHandle_t task = nullptr;
    StackType_t* stack = nullptr;
    uint32_t stack_words = 0U;
    StaticSemaphore_t start_storage{};
    StaticSemaphore_t join_storage{};
    SemaphoreHandle_t start = nullptr;
    SemaphoreHandle_t join = nullptr;
    ThreadEntry entry = nullptr;
    void* context = nullptr;
    bool busy = false;
};

struct FreeRtosResourcesBase {
    StaticTask_t* dispatcher_tcb = nullptr;
    StackType_t* dispatcher_stack = nullptr;
    uint32_t dispatcher_stack_words = 0U;
    uint32_t dispatcher_stack_bytes = 0U;
    StaticSemaphore_t* wake_storage = nullptr;
    StaticSemaphore_t* join_storage = nullptr;
    SemaphoreHandle_t wake = nullptr;
    SemaphoreHandle_t join = nullptr;
    ContextSlot* slot_table = nullptr;
    uint16_t slot_count = 0U;
    WorkerSlot* worker_slots = nullptr;
    uint16_t worker_slot_count = 0U;
    /* Native priority for every resident worker. Defaults to one above idle;
       a board that runs real work on workers can raise it, but it must stay
       below dispatcher_priority() (the PAL does not verify this at run time). */
    UBaseType_t worker_priority = 1U;
};

// Zero-size placeholder for WorkerSlots == 0 (zero-length arrays are a GNU
// extension; std::array<T,0> still carries the T member).
struct EmptyResource {};

template <uint32_t DispatcherStackBytes, uint16_t ContextSlots,
          uint16_t WorkerSlots = 0U, uint32_t WorkerStackBytes = 1024U>
struct FreeRtosResources : public FreeRtosResourcesBase {
    static_assert(DispatcherStackBytes >= kFreeRtosMinDispatcherStackBytes,
                  "dispatcher stack below the 512-byte floor");
    static_assert(ContextSlots > 0U, "at least one context slot is required");
    static_assert(WorkerSlots == 0U || WorkerStackBytes >= 256U,
                  "worker stack below the 256-byte floor");
    static_assert(WorkerSlots == 0U || configMAX_PRIORITIES >= 3,
                  "worker slots need a priority below the dispatcher");

    static constexpr uint32_t kDispatcherWords =
        (DispatcherStackBytes + sizeof(StackType_t) - 1U) / sizeof(StackType_t);
    static constexpr uint32_t kWorkerWords =
        (WorkerStackBytes + sizeof(StackType_t) - 1U) / sizeof(StackType_t);

    StaticTask_t dispatcher_tcb_storage{};
    StackType_t dispatcher_stack_storage[kDispatcherWords]{};
    StaticSemaphore_t wake_obj{};
    StaticSemaphore_t join_obj{};
    ContextSlot slots[ContextSlots]{};
    typename std::conditional<
        (WorkerSlots > 0U),
        std::array<WorkerSlot, WorkerSlots>,
        EmptyResource>::type worker_slot_storage{};
    typename std::conditional<
        (WorkerSlots > 0U),
        std::array<std::array<StackType_t, kWorkerWords>, WorkerSlots>,
        EmptyResource>::type worker_stack_storage{};

    FreeRtosResources() noexcept
        : FreeRtosResourcesBase(),
          dispatcher_tcb_storage{},
          dispatcher_stack_storage{},
          wake_obj{},
          join_obj{},
          slots{},
          worker_slot_storage{},
          worker_stack_storage{}
    {
        this->dispatcher_tcb = &this->dispatcher_tcb_storage;
        this->dispatcher_stack = this->dispatcher_stack_storage;
        this->dispatcher_stack_words = kDispatcherWords;
        this->dispatcher_stack_bytes = DispatcherStackBytes;
        this->wake_storage = &this->wake_obj;
        this->join_storage = &this->join_obj;
        this->slot_table = this->slots;
        this->slot_count = ContextSlots;
        if constexpr (WorkerSlots > 0U) {
            this->worker_slots = this->worker_slot_storage.data();
            this->worker_slot_count = WorkerSlots;
            for (uint16_t i = 0U; i < WorkerSlots; ++i) {
                this->worker_slot_storage[i].stack =
                    this->worker_stack_storage[i].data();
                this->worker_slot_storage[i].stack_words = kWorkerWords;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// FreeRtos PAL. Lifecycle is one init, one start, one stop; restart is
// rejected. The application must call start_dispatcher() (through
// Runtime::start()) from a task that is already running under the FreeRTOS
// scheduler. See design section D9.
//
// initialize()/start_dispatcher()/join_dispatcher() are NOT internally
// synchronized: call them from one task, in order. Producer-side entry points
// (signal/wait/sem/softirq) are safe from other tasks and ISRs.
// ---------------------------------------------------------------------------
class FreeRtos
{
public:
    struct SemHandle {
        StaticSemaphore_t storage{};
        SemaphoreHandle_t handle = nullptr;
        FreeRtos* pal = nullptr;
    };

    struct MutexHandle {
        StaticSemaphore_t storage{};
        SemaphoreHandle_t handle = nullptr;
        FreeRtos* pal = nullptr;
    };

    struct CondHandle {
        StaticSemaphore_t storage{};
        SemaphoreHandle_t handle = nullptr;
        std::atomic<uint32_t> waiters{0U};
        FreeRtos* pal = nullptr;
    };

    struct ThreadHandle {
        FreeRtos* pal = nullptr;
        ThreadEntry entry = nullptr;
        void* context = nullptr;
        uint16_t slot_idx = 0U;
        bool valid = false;
    };

    // Optional independent watchdog hook. The BSP owns the watchdog; the PAL
    // only gives the progress path one place to kick it. No hook means no-op.
    struct WatchdogOps {
        void (*kick)(void* ctx) = nullptr;
        void* ctx = nullptr;
    };

    // SPSC mailbox: raise() publishes into the ring and gives one counting-
    // semaphore token; take() consumes one token and one payload. A counting
    // semaphore (not binary) keeps token count equal to payload count, so a
    // burst of raises cannot leave payloads without wakeups.
    // deinit() requires that all producers and the consumer have stopped:
    // it deletes the wake semaphore, and a concurrent raise/take would touch
    // freed kernel state.
    struct SoftIrqHandle {
        std::atomic<uint32_t> head{0U};
        std::atomic<uint32_t> tail{0U};
        int32_t ring[kFreeRtosSoftIrqSlots]{};
        StaticSemaphore_t wake_storage{};
        SemaphoreHandle_t wake = nullptr;
        FreeRtos* pal = nullptr;
    };

    // Host-test convenience only: references an internal static resource.
    // Production boards must pass explicit FreeRtosResources.
    FreeRtos() noexcept;

    template <typename ResT>
    explicit FreeRtos(ResT& res) noexcept
        : res_(&res),
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

    FreeRtos(const FreeRtos&) = delete;
    FreeRtos& operator=(const FreeRtos&) = delete;

    /* ---- Interrupt masking -------------------------------------------- */
    CriticalToken irq_save() noexcept;
    void irq_restore(CriticalToken token) noexcept;

    /* ---- Execution context -------------------------------------------- */
    bool register_current_task(LogicalPrio prio) noexcept;
    ExecutionContext current_context() const noexcept;
    /* Instance method: two FreeRtos objects can run two dispatchers in one
       image, and each must recognise only its own task handle. */
    bool in_dispatcher_thread() const noexcept;

    /* ---- Monotonic clock ---------------------------------------------- */
    uint64_t monotonic_ns() const noexcept;
    uint64_t clock_resolution_ns() const noexcept;
    void set_clock_ops(ClockOps ops) noexcept;

    /* ---- Dispatcher wait/signal (0 ms = wait forever) ----------------- */
    void wait_dispatcher(uint32_t timeout_ms) noexcept;
    void signal_dispatcher_from_task() noexcept;
    void signal_dispatcher_from_isr() noexcept;

    /* ---- Dispatcher thread lifecycle ---------------------------------- */
    /* initialize() calls kernel APIs and is not re-entrant. A failure is
       terminal (kInitFailed); the caller must not reuse the same resources
       object with a new PAL, because partially created workers/semaphores are
       left to the idle task. */
    InitError initialize() noexcept;
    InitError start_dispatcher(ThreadEntry entry, void* context) noexcept;
    void join_dispatcher() noexcept;
    void watchdog_progress(uint32_t marker) noexcept;
    void set_watchdog_ops(WatchdogOps ops) noexcept
    {
        watchdog_ = ops;
    }
    void set_dispatcher_stack_bytes(uint32_t bytes) noexcept;
    uint32_t dispatcher_stack_bytes() const noexcept;
    /* Native handle of this instance's dispatcher task, or nullptr before
       start. Diagnostics only (stack watermark, debugging). */
    TaskHandle_t dispatcher_task_handle() const noexcept
    {
        return dispatcher_handle_;
    }

    /* ---- Direct dispatch depth (per registered task) ------------------ */
    void enter_direct() noexcept;
    void leave_direct() noexcept;

    /* ---- SemOps family ------------------------------------------------ */
    bool sem_init(SemHandle& sem, uint32_t initial) noexcept;
    bool sem_take(SemHandle& sem, uint32_t timeout_ms) noexcept;
    void sem_release(SemHandle& sem) noexcept;
    void sem_release_from_isr(SemHandle& sem) noexcept;
    void sem_deinit(SemHandle& sem) noexcept;

    /* ---- MutexOps family ---------------------------------------------- */
    bool mutex_init(MutexHandle& m) noexcept;
    void mutex_lock(MutexHandle& m) noexcept;
    void mutex_unlock(MutexHandle& m) noexcept;
    void mutex_deinit(MutexHandle& m) noexcept;

    /* ---- CondOps family ----------------------------------------------- */
    bool cond_init(CondHandle& c) noexcept;
    void cond_wait(CondHandle& c, MutexHandle& m, uint32_t timeout_ms) noexcept;
    void cond_signal(CondHandle& c) noexcept;
    void cond_broadcast(CondHandle& c) noexcept;
    void cond_deinit(CondHandle& c) noexcept;

    /* ---- ThreadOps family (resident workers) --------------------------- */
    bool thread_create(ThreadHandle& t, ThreadEntry entry, void* context) noexcept;
    void thread_join(ThreadHandle& t) noexcept;

    /* ---- SoftIrqOps family -------------------------------------------- */
    bool softirq_init(SoftIrqHandle& h) noexcept;
    bool softirq_raise(SoftIrqHandle& h, int32_t payload) noexcept;
    int32_t softirq_take(SoftIrqHandle& h, uint32_t timeout_ms) noexcept;
    void softirq_deinit(SoftIrqHandle& h) noexcept;

    /* ---- Sleep --------------------------------------------------------- */
    void sleep_us(uint32_t us) noexcept;

    /* ---- Queue backend and board profile ------------------------------- */
    template <typename T, uint16_t Cap>
    using QueueBackend = coact::SingleCoreCriticalRing<T, Cap>;

    using Profile = coact::RttSingleCoreProfile;

    static constexpr UBaseType_t dispatcher_priority() noexcept
    {
        return static_cast<UBaseType_t>(configMAX_PRIORITIES - 1);
    }

private:
    enum class Lifecycle : uint8_t {
        kUninitialized,
        kReady,
        kStarted,
        kStopped,
        kInitFailed,
    };

    static void dispatcher_thread_entry(void* param) noexcept;
    static void worker_thread_entry(void* param) noexcept;
    InitError ensure_initialized() noexcept;
    static ContextSlot* find_slot(FreeRtosResourcesBase* res,
                                  TaskHandle_t task) noexcept;
    static ContextSlot* alloc_slot(FreeRtosResourcesBase* res,
                                   TaskHandle_t task) noexcept;
    ExecutionContext* tls_ctx() const noexcept;
    uint64_t read_extended_counter() const noexcept;

    FreeRtosResourcesBase* res_;
    ThreadEntry user_entry_;
    void* user_ctx_;
    Lifecycle state_;
    InitError last_error_;
    uint32_t dispatcher_stack_bytes_;
    ClockOps clock_ops_;
    uint64_t ns_per_counter_;
    mutable uint64_t counter_epoch_ = 0U;
    mutable uint64_t last_counter_ = 0U;
    mutable bool counter_seen_ = false;
    std::atomic_flag init_guard_ = ATOMIC_FLAG_INIT;
    TaskHandle_t dispatcher_handle_ = nullptr;
    WatchdogOps watchdog_{};
};

}  // namespace pal
}  // namespace coact
