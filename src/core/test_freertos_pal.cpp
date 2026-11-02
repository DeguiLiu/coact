// coact FreeRTOS PAL host tests (docs/design_freertos_pal_zh.md, stages A/B).
//
// The FreeRTOS API is backed by test/freertos_stub.h (pthreads). These tests
// cover the parts that are portable to a host: static resources, explicit
// initialization and error propagation, context registration/freeze, ISR
// context, clock ops, dispatcher lifecycle, Sem/Mutex/Cond, resident workers,
// SoftIrq and one end-to-end Runtime assembly.
//
// SPDX-License-Identifier: MIT

#include "test/test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <unistd.h>

#ifndef COACT_FREERTOS_STUB
#define COACT_FREERTOS_STUB
#endif
#include "test/freertos_stub.h"

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/pal.hpp"
#include "coact/pal_freertos.hpp"
#include "coact/pool.hpp"
#include "coact/runtime.hpp"
#include "coact/staging.hpp"

namespace {

using ResStandard = coact::pal::FreeRtosResources<4096U, 4U, 2U, 1024U>;

/* Runs fn(arg) on a distinct stub task identity and joins it. */
struct TaskRun {
    void (*fn)(void*);
    void* arg;
};

void task_trampoline(void* p) noexcept
{
    auto* run = static_cast<TaskRun*>(p);
    run->fn(run->arg);
}

bool run_task(void (*fn)(void*), void* arg) noexcept
{
    /* Each run gets its own TCB identity: two tasks sharing a handle would
       look like a duplicate registration and hide the table-full case. */
    StaticTask_t* tcb = new (std::nothrow) StaticTask_t{};
    StackType_t* stack = new (std::nothrow) StackType_t[256];
    if ((nullptr == tcb) || (nullptr == stack)) {
        delete tcb;
        delete[] stack;
        return false;
    }
    TaskRun run{fn, arg};
    TaskHandle_t h = xTaskCreateStatic(&task_trampoline, "test", 256U, &run,
                                       2U, stack, tcb);
    if (nullptr == h) {
        delete tcb;
        delete[] stack;
        return false;
    }
    pthread_join(h->thread, nullptr);
    delete tcb;
    delete[] stack;
    return true;
}

struct MockCounter {
    uint64_t counter = 0U;
};

uint64_t mock_counter_read(void* ctx) noexcept
{
    return static_cast<MockCounter*>(ctx)->counter;
}

/* ---- Static resources --------------------------------------------------- */
COACT_TEST(freertos_resources_are_caller_owned)
{
    static_assert(ResStandard::kDispatcherWords == 4096U / sizeof(StackType_t),
                  "stack must convert to StackType_t words");
    static_assert(coact::pal::FreeRtos::dispatcher_priority()
                      == static_cast<UBaseType_t>(configMAX_PRIORITIES - 1),
                  "dispatcher runs at the highest application priority");
    static_assert(std::is_same<coact::pal::FreeRtos::Profile,
                               coact::RttSingleCoreProfile>::value,
                  "the FreeRTOS board profile reuses the single-core profile");

    static ResStandard res;
    CHECK(res.dispatcher_tcb == &res.dispatcher_tcb_storage);
    CHECK(res.dispatcher_stack == res.dispatcher_stack_storage);
    CHECK_EQ(res.dispatcher_stack_words, ResStandard::kDispatcherWords);
    CHECK_EQ(res.dispatcher_stack_bytes, 4096U);
    CHECK(res.wake_storage == &res.wake_obj);
    CHECK(res.join_storage == &res.join_obj);
    CHECK_EQ(res.slot_count, 4U);
    CHECK(res.slot_table == res.slots);
    CHECK_EQ(res.worker_slot_count, 2U);
    CHECK(res.worker_slots == res.worker_slot_storage.data());
    CHECK(res.worker_slots[0].stack == res.worker_stack_storage[0].data());
    CHECK_EQ(res.worker_slots[1].stack_words, ResStandard::kWorkerWords);
}

/* ---- Initialization ----------------------------------------------------- */
COACT_TEST(freertos_initialize_is_explicit_and_idempotent)
{
    stub_freertos_reset_faults();
    static ResStandard res;
    coact::pal::FreeRtos pal(res);

    CHECK_EQ(static_cast<int>(coact::pal::InitError::kOk),
             static_cast<int>(pal.initialize()));
    CHECK_EQ(static_cast<int>(coact::pal::InitError::kOk),
             static_cast<int>(pal.initialize()));
    CHECK_EQ(pal.dispatcher_stack_bytes(), 4096U);
}

COACT_TEST(freertos_initialize_propagates_failures)
{
    stub_freertos_reset_faults();

    {
        static ResStandard res;
        coact::pal::FreeRtos pal(res);
        stub_freertos_fail_sem() = true;
        CHECK_EQ(static_cast<int>(coact::pal::InitError::kSemInitFailed),
                 static_cast<int>(pal.initialize()));
        /* Terminal: no retry after a failed init. */
        CHECK_EQ(static_cast<int>(coact::pal::InitError::kSemInitFailed),
                 static_cast<int>(pal.initialize()));
        stub_freertos_reset_faults();
    }
    {
        static ResStandard res;
        coact::pal::FreeRtos pal(res);
        stub_freertos_fail_task() = true;
        CHECK_EQ(static_cast<int>(coact::pal::InitError::kThreadInitFailed),
                 static_cast<int>(pal.initialize()));
        stub_freertos_reset_faults();
    }
    {
        static ResStandard res;
        coact::pal::FreeRtos pal(res);
        pal.set_dispatcher_stack_bytes(8192U);
        CHECK_EQ(static_cast<int>(coact::pal::InitError::kStackTooLarge),
                 static_cast<int>(pal.initialize()));
    }
    {
        /* A request below the floor is clamped, and the clamped value is what
           start_dispatcher() will use to size the TCB stack. */
        static ResStandard res;
        coact::pal::FreeRtos pal(res);
        pal.set_dispatcher_stack_bytes(64U);
        CHECK_EQ(static_cast<int>(coact::pal::InitError::kOk),
                 static_cast<int>(pal.initialize()));
        CHECK_EQ(pal.dispatcher_stack_bytes(),
                 coact::pal::kFreeRtosMinDispatcherStackBytes);
    }
}

/* ---- Dispatcher lifecycle ---------------------------------------------- */
namespace {
std::atomic<int> g_dispatch_runs{0};
void dispatcher_once(void* /*ctx*/) noexcept
{
    g_dispatch_runs.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

COACT_TEST(freertos_dispatcher_starts_once_and_joins)
{
    stub_freertos_reset_faults();
    static ResStandard res;
    coact::pal::FreeRtos pal(res);
    g_dispatch_runs.store(0, std::memory_order_relaxed);

    CHECK_EQ(static_cast<int>(coact::pal::InitError::kOk),
             static_cast<int>(pal.start_dispatcher(&dispatcher_once, nullptr)));
    pal.join_dispatcher();
    CHECK_EQ(g_dispatch_runs.load(std::memory_order_relaxed), 1);
    CHECK_EQ(static_cast<int>(coact::pal::InitError::kAlreadyStarted),
             static_cast<int>(pal.start_dispatcher(&dispatcher_once, nullptr)));
    /* Joining a stopped dispatcher is a no-op, not a second wait. */
    pal.join_dispatcher();
}

COACT_TEST(freertos_start_requires_running_scheduler)
{
    stub_freertos_reset_faults();
    static ResStandard res;
    coact::pal::FreeRtos pal(res);
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));
    stub_freertos_scheduler_running() = false;
    CHECK_EQ(static_cast<int>(coact::pal::InitError::kThreadStartFailed),
             static_cast<int>(pal.start_dispatcher(&dispatcher_once, nullptr)));
    stub_freertos_scheduler_running() = true;
}

namespace {
struct DualCtx {
    coact::pal::FreeRtos* self;
    coact::pal::FreeRtos* other;
    std::atomic<int>* out;
};

void dual_dispatcher_entry(void* p) noexcept
{
    auto* ctx = static_cast<DualCtx*>(p);
    int result = 0;
    if (ctx->self->in_dispatcher_thread()) {
        result |= 1;
    }
    if (ctx->other->in_dispatcher_thread()) {
        result |= 2;
    }
    ctx->out->store(result, std::memory_order_relaxed);
}
}  // namespace

COACT_TEST(freertos_two_instances_track_their_own_dispatcher)
{
    stub_freertos_reset_faults();
    static ResStandard res_a;
    static ResStandard res_b;
    coact::pal::FreeRtos a(res_a);
    coact::pal::FreeRtos b(res_b);
    std::atomic<int> result_a{-1};
    std::atomic<int> result_b{-1};

    DualCtx ctx_a{&a, &b, &result_a};
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(a.start_dispatcher(&dual_dispatcher_entry, &ctx_a)));
    a.join_dispatcher();
    CHECK_EQ(result_a.load(std::memory_order_relaxed), 1);  /* own only */

    DualCtx ctx_b{&b, &a, &result_b};
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(b.start_dispatcher(&dual_dispatcher_entry, &ctx_b)));
    b.join_dispatcher();
    CHECK_EQ(result_b.load(std::memory_order_relaxed), 1);  /* stale A is not B */
}

/* ---- Context table ------------------------------------------------------ */
namespace {
coact::pal::FreeRtos* g_ctx_pal = nullptr;
std::atomic<int> g_ctx_result{-1};
std::atomic<int> g_ctx_depth{-1};

void register_and_probe(void* /*ctx*/) noexcept
{
    const bool ok = g_ctx_pal->register_current_task(5U);
    const bool duplicate = g_ctx_pal->register_current_task(5U);
    const bool bad_prio = g_ctx_pal->register_current_task(0U);
    const coact::ExecutionContext before = g_ctx_pal->current_context();
    g_ctx_pal->enter_direct();
    const coact::ExecutionContext during = g_ctx_pal->current_context();
    g_ctx_pal->leave_direct();
    const coact::ExecutionContext after = g_ctx_pal->current_context();
    const bool good = ok && !duplicate && !bad_prio
        && (before.kind == coact::ContextKind::Task)
        && before.prio_valid
        && (before.logical_prio == 5U)
        && (during.direct_depth == 1U)
        && (after.direct_depth == 0U);
    g_ctx_result.store(good ? 1 : 0, std::memory_order_relaxed);
    g_ctx_depth.store(static_cast<int>(during.direct_depth),
                      std::memory_order_relaxed);
}

void register_only(void* p) noexcept
{
    auto* out = static_cast<int*>(p);
    *out = g_ctx_pal->register_current_task(7U) ? 1 : 0;
}

std::atomic<int> g_prio_mapped{-1};
void priority_probe(void* /*ctx*/) noexcept
{
    if (!g_ctx_pal->register_current_task(63U)) {
        g_prio_mapped.store(-2, std::memory_order_relaxed);
        return;
    }
    g_prio_mapped.store(
        static_cast<int>(xTaskGetCurrentTaskHandle()->prio),
        std::memory_order_relaxed);
}
}  // namespace

COACT_TEST(freertos_context_table_registers_and_dedups)
{
    stub_freertos_reset_faults();
    static ResStandard res;
    coact::pal::FreeRtos pal(res);
    g_ctx_pal = &pal;
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));

    g_ctx_result.store(-1, std::memory_order_relaxed);
    REQUIRE(run_task(&register_and_probe, nullptr));
    CHECK_EQ(g_ctx_result.load(std::memory_order_relaxed), 1);
    CHECK_EQ(g_ctx_depth.load(std::memory_order_relaxed), 1);
}

COACT_TEST(freertos_context_table_is_frozen_after_start)
{
    stub_freertos_reset_faults();
    static ResStandard res;
    coact::pal::FreeRtos pal(res);
    g_ctx_pal = &pal;
    g_dispatch_runs.store(0, std::memory_order_relaxed);
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.start_dispatcher(&dispatcher_once, nullptr)));
    pal.join_dispatcher();

    int ok = -1;
    REQUIRE(run_task(&register_only, &ok));
    CHECK_EQ(ok, 0);
}

COACT_TEST(freertos_context_table_full_is_reported)
{
    stub_freertos_reset_faults();
    static coact::pal::FreeRtosResources<4096U, 1U> res;
    coact::pal::FreeRtos pal(res);
    g_ctx_pal = &pal;
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));

    int first = -1;
    int second = -1;
    REQUIRE(run_task(&register_only, &first));
    REQUIRE(run_task(&register_only, &second));
    CHECK_EQ(first, 1);
    CHECK_EQ(second, 0);
}

COACT_TEST(freertos_register_maps_logical_priority)
{
    stub_freertos_reset_faults();
    static ResStandard res;
    coact::pal::FreeRtos pal(res);
    g_ctx_pal = &pal;
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));
    g_prio_mapped.store(-1, std::memory_order_relaxed);
    REQUIRE(run_task(&priority_probe, nullptr));
    /* LogicalPrio 63 must not be passed to FreeRTOS; it maps to the highest
       application priority, leaving 0 for idle. */
    CHECK_EQ(g_prio_mapped.load(std::memory_order_relaxed),
             static_cast<int>(configMAX_PRIORITIES - 2));
}

/* ---- ISR context and clock --------------------------------------------- */
COACT_TEST(freertos_isr_context_and_clock_ops)
{
    stub_freertos_reset_faults();
    static ResStandard res;
    coact::pal::FreeRtos pal(res);
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));

    stub_freertos_set_in_isr(true);
    const coact::ExecutionContext isr = pal.current_context();
    /* Both paths of irq_save must be exercised; a bug here would call
       taskENTER_CRITICAL from an ISR. */
    const coact::pal::CriticalToken token = pal.irq_save();
    pal.irq_restore(token);
    stub_freertos_set_in_isr(false);
    CHECK(isr.kind == coact::ContextKind::Isr);

    CHECK_EQ(pal.clock_resolution_ns(), 1000000ULL);
    const uint64_t t0 = pal.monotonic_ns();
    vTaskDelay(2U);
    const uint64_t t1 = pal.monotonic_ns();
    CHECK(t1 >= t0);
}

COACT_TEST(freertos_clock_ops_extend_32bit_counter)
{
    stub_freertos_reset_faults();
    static ResStandard res;
    coact::pal::FreeRtos pal(res);
    MockCounter counter{0xFFFFFFFCULL};
    coact::pal::ClockOps ops;
    ops.read_counter = &mock_counter_read;
    ops.frequency_hz = 100U;
    ops.ctx = &counter;
    ops.counter_bits = 32U;
    pal.set_clock_ops(ops);

    const uint64_t before = pal.monotonic_ns();
    counter.counter = 5U;
    const uint64_t after = pal.monotonic_ns();
    CHECK(after > before);
    CHECK_EQ(after - before, 90000000ULL);
}

/* ---- Sem / Mutex / Cond ------------------------------------------------- */
COACT_TEST(freertos_sem_and_mutex)
{
    stub_freertos_reset_faults();
    static ResStandard res;
    coact::pal::FreeRtos pal(res);
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));

    coact::pal::FreeRtos::SemHandle sem;
    REQUIRE(pal.sem_init(sem, 0U));
    CHECK(!pal.sem_take(sem, 0U));       /* 0 = non-blocking try */
    CHECK(!pal.sem_take(sem, 1U));       /* finite timeout */
    /* Counting semantics: three releases must yield three tokens. */
    pal.sem_release(sem);
    pal.sem_release(sem);
    pal.sem_release(sem);
    CHECK(pal.sem_take(sem, 0U));
    CHECK(pal.sem_take(sem, 0U));
    CHECK(pal.sem_take(sem, 0U));
    CHECK(!pal.sem_take(sem, 0U));
    pal.sem_deinit(sem);

    coact::pal::FreeRtos::SemHandle counting;
    REQUIRE(pal.sem_init(counting, 3U));
    CHECK(pal.sem_take(counting, 0U));
    CHECK(pal.sem_take(counting, 0U));
    CHECK(pal.sem_take(counting, 0U));
    CHECK(!pal.sem_take(counting, 0U));
    pal.sem_deinit(counting);

    coact::pal::FreeRtos::MutexHandle mutex;
    REQUIRE(pal.mutex_init(mutex));
    pal.mutex_lock(mutex);
    pal.mutex_unlock(mutex);
    pal.mutex_deinit(mutex);
}

COACT_TEST(freertos_isr_release_nested_critical_and_sleep)
{
    stub_freertos_reset_faults();
    static ResStandard res;
    coact::pal::FreeRtos pal(res);
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));

    /* Nested task-side critical sections must pair without corrupting the
       kernel nesting counter. */
    const coact::pal::CriticalToken outer = pal.irq_save();
    const coact::pal::CriticalToken inner = pal.irq_save();
    pal.irq_restore(inner);
    pal.irq_restore(outer);

    coact::pal::FreeRtos::SemHandle sem;
    REQUIRE(pal.sem_init(sem, 0U));
    stub_freertos_set_in_isr(true);
    pal.sem_release_from_isr(sem);
    stub_freertos_set_in_isr(false);
    CHECK(pal.sem_take(sem, 0U));
    pal.sem_deinit(sem);

    const uint64_t t0 = pal.monotonic_ns();
    pal.sleep_us(2000U);
    const uint64_t t1 = pal.monotonic_ns();
    CHECK(t1 - t0 >= 1000000ULL);
}

namespace {
coact::pal::FreeRtos::MutexHandle g_cond_mutex;
coact::pal::FreeRtos::CondHandle g_cond;
std::atomic<bool> g_cond_ready{false};
std::atomic<bool> g_cond_flag{false};
std::atomic<int> g_cond_seen{-1};

void cond_waiter(void* /*ctx*/) noexcept
{
    g_cond.pal->mutex_lock(g_cond_mutex);
    g_cond_ready.store(true, std::memory_order_release);
    while (!g_cond_flag.load(std::memory_order_acquire)) {
        g_cond.pal->cond_wait(g_cond, g_cond_mutex, 0U);
    }
    g_cond_seen.store(g_cond_flag.load(std::memory_order_acquire) ? 1 : 0,
                      std::memory_order_relaxed);
    g_cond.pal->mutex_unlock(g_cond_mutex);
}
}  // namespace

COACT_TEST(freertos_cond_wait_signal)
{
    stub_freertos_reset_faults();
    static ResStandard res;
    coact::pal::FreeRtos pal(res);
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));
    REQUIRE(pal.mutex_init(g_cond_mutex));
    REQUIRE(pal.cond_init(g_cond));
    g_cond_ready.store(false, std::memory_order_relaxed);
    g_cond_flag.store(false, std::memory_order_relaxed);
    g_cond_seen.store(-1, std::memory_order_relaxed);

    /* Spawn without joining: the waiter cannot finish until we signal it. */
    static TaskRun run{&cond_waiter, nullptr};
    static StaticTask_t tcb;
    static StackType_t stack[256];
    TaskHandle_t waiter = xTaskCreateStatic(&task_trampoline, "cond", 256U,
                                            &run, 2U, stack, &tcb);
    REQUIRE(waiter != nullptr);
    while (!g_cond_ready.load(std::memory_order_acquire)) {
        usleep(1000);
    }
    pal.mutex_lock(g_cond_mutex);
    g_cond_flag.store(true, std::memory_order_release);
    pal.mutex_unlock(g_cond_mutex);
    pal.cond_signal(g_cond);
    pthread_join(waiter->thread, nullptr);

    /* Joined above: the waiter has observed the wake-up. */
    CHECK_EQ(g_cond_seen.load(std::memory_order_relaxed), 1);
    pal.cond_deinit(g_cond);
    pal.mutex_deinit(g_cond_mutex);
}

namespace {
std::atomic<int> g_bcast_ready{0};
std::atomic<int> g_bcast_woken{0};

void broadcast_waiter(void* /*ctx*/) noexcept
{
    g_cond.pal->mutex_lock(g_cond_mutex);
    g_bcast_ready.fetch_add(1, std::memory_order_acq_rel);
    while (!g_cond_flag.load(std::memory_order_acquire)) {
        g_cond.pal->cond_wait(g_cond, g_cond_mutex, 0U);
    }
    g_bcast_woken.fetch_add(1, std::memory_order_acq_rel);
    g_cond.pal->mutex_unlock(g_cond_mutex);
}
}  // namespace

COACT_TEST(freertos_cond_broadcast_wakes_every_waiter)
{
    stub_freertos_reset_faults();
    static ResStandard res;
    coact::pal::FreeRtos pal(res);
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));
    REQUIRE(pal.mutex_init(g_cond_mutex));
    REQUIRE(pal.cond_init(g_cond));
    g_cond_flag.store(false, std::memory_order_relaxed);
    g_bcast_ready.store(0, std::memory_order_relaxed);
    g_bcast_woken.store(0, std::memory_order_relaxed);

    static TaskRun run{&broadcast_waiter, nullptr};
    static StaticTask_t tcb_a;
    static StaticTask_t tcb_b;
    static StackType_t stack_a[256];
    static StackType_t stack_b[256];
    TaskHandle_t a = xTaskCreateStatic(&task_trampoline, "bw0", 256U, &run,
                                       2U, stack_a, &tcb_a);
    TaskHandle_t b = xTaskCreateStatic(&task_trampoline, "bw1", 256U, &run,
                                       2U, stack_b, &tcb_b);
    REQUIRE((a != nullptr) && (b != nullptr));
    while (g_bcast_ready.load(std::memory_order_acquire) < 2) {
        usleep(1000);
    }
    pal.mutex_lock(g_cond_mutex);
    g_cond_flag.store(true, std::memory_order_release);
    pal.mutex_unlock(g_cond_mutex);
    pal.cond_broadcast(g_cond);
    pthread_join(a->thread, nullptr);
    pthread_join(b->thread, nullptr);
    CHECK_EQ(g_bcast_woken.load(std::memory_order_relaxed), 2);
    pal.cond_deinit(g_cond);
    pal.mutex_deinit(g_cond_mutex);
}

namespace {
long long elapsed_ms(std::chrono::steady_clock::time_point start) noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}
}  // namespace

COACT_TEST(freertos_cond_signal_before_wait_is_not_lost)
{
    stub_freertos_reset_faults();
    static ResStandard res;
    coact::pal::FreeRtos pal(res);
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));
    coact::pal::FreeRtos::MutexHandle mutex;
    coact::pal::FreeRtos::CondHandle cond;
    REQUIRE(pal.mutex_init(mutex));
    REQUIRE(pal.cond_init(cond));

    pal.mutex_lock(mutex);
    pal.cond_signal(cond);  /* no waiter yet: the counting semaphore stores it */
    const auto start = std::chrono::steady_clock::now();
    pal.cond_wait(cond, mutex, 200U);
    const long long ms = elapsed_ms(start);
    pal.mutex_unlock(mutex);
    /* A stored token returns immediately; a lost one would block ~200 ms. */
    CHECK(ms < 100);
    pal.cond_deinit(cond);
    pal.mutex_deinit(mutex);
}

COACT_TEST(freertos_cond_wait_times_out)
{
    stub_freertos_reset_faults();
    static ResStandard res;
    coact::pal::FreeRtos pal(res);
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));
    coact::pal::FreeRtos::MutexHandle mutex;
    coact::pal::FreeRtos::CondHandle cond;
    REQUIRE(pal.mutex_init(mutex));
    REQUIRE(pal.cond_init(cond));

    pal.mutex_lock(mutex);
    const auto start = std::chrono::steady_clock::now();
    pal.cond_wait(cond, mutex, 5U);
    const long long ms = elapsed_ms(start);
    pal.mutex_unlock(mutex);
    CHECK(ms >= 1);
    CHECK(ms < 100);
    pal.cond_deinit(cond);
    pal.mutex_deinit(mutex);
}

/* ---- ThreadOps ---------------------------------------------------------- */
namespace {
std::atomic<int> g_worker_runs{0};
std::atomic<int> g_worker_prio{-1};
void worker_count(void* /*ctx*/) noexcept
{
    g_worker_prio.store(static_cast<int>(xTaskGetCurrentTaskHandle()->prio),
                        std::memory_order_relaxed);
    g_worker_runs.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

COACT_TEST(freertos_worker_pool_runs_and_reuses_slots)
{
    stub_freertos_reset_faults();
    static ResStandard res;
    res.worker_priority = 2U;
    coact::pal::FreeRtos pal(res);
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));
    g_worker_runs.store(0, std::memory_order_relaxed);
    g_worker_prio.store(-1, std::memory_order_relaxed);

    coact::pal::FreeRtos::ThreadHandle a;
    coact::pal::FreeRtos::ThreadHandle b;
    coact::pal::FreeRtos::ThreadHandle c;
    REQUIRE(pal.thread_create(a, &worker_count, nullptr));
    REQUIRE(pal.thread_create(b, &worker_count, nullptr));
    CHECK(!pal.thread_create(c, &worker_count, nullptr));  /* pool full */
    pal.thread_join(a);
    pal.thread_join(b);

    /* Slots are reusable after join; the task itself stays resident. */
    REQUIRE(pal.thread_create(c, &worker_count, nullptr));
    pal.thread_join(c);
    CHECK_EQ(g_worker_runs.load(std::memory_order_relaxed), 3);
    CHECK_EQ(g_worker_prio.load(std::memory_order_relaxed), 2);
}

/* ---- SoftIrq ------------------------------------------------------------ */
COACT_TEST(freertos_softirq_roundtrip_and_bound)
{
    stub_freertos_reset_faults();
    static ResStandard res;
    coact::pal::FreeRtos pal(res);
    REQUIRE_EQ(static_cast<int>(coact::pal::InitError::kOk),
               static_cast<int>(pal.initialize()));

    coact::pal::FreeRtos::SoftIrqHandle h;
    REQUIRE(pal.softirq_init(h));
    CHECK_EQ(pal.softirq_take(h, 1U), -1);   /* timeout */
    REQUIRE(pal.softirq_raise(h, 11));
    REQUIRE(pal.softirq_raise(h, 22));
    stub_freertos_set_in_isr(true);
    REQUIRE(pal.softirq_raise(h, 33));
    stub_freertos_set_in_isr(false);
    CHECK_EQ(pal.softirq_take(h, 10U), 11);
    CHECK_EQ(pal.softirq_take(h, 10U), 22);
    CHECK_EQ(pal.softirq_take(h, 10U), 33);

    for (int32_t i = 0; i < static_cast<int32_t>(coact::pal::kFreeRtosSoftIrqSlots);
         ++i) {
        REQUIRE(pal.softirq_raise(h, i));
    }
    CHECK(!pal.softirq_raise(h, 99));        /* ring full */
    for (int32_t i = 0; i < static_cast<int32_t>(coact::pal::kFreeRtosSoftIrqSlots);
         ++i) {
        CHECK_EQ(pal.softirq_take(h, 10U), i);
    }
    pal.softirq_deinit(h);
}

/* ---- End-to-end Runtime assembly --------------------------------------- */
namespace {
std::atomic<int> g_rt_events{0};
struct RtCtx {};
void rt_on_event(RtCtx&, const coact::Event&) noexcept
{
    g_rt_events.fetch_add(1, std::memory_order_relaxed);
}
bool rt_guard(const RtCtx&, const coact::Event&) noexcept
{
    return true;
}
const coact::StateDef<RtCtx> kRtStates[] = {
    { -1, nullptr, nullptr, nullptr },
    { 0, nullptr, nullptr, nullptr },
};
const coact::TransitionDef<RtCtx> kRtTrans[] = {
    { 1, 1U, 1, coact::TransitionKind::Internal, rt_guard, rt_on_event },
};
struct RtTraits {
    static coact::LogicalPrio logical_prio() noexcept { return 20U; }
    static coact::PriorityClass priority_class() noexcept
    {
        return coact::PriorityClass::Normal;
    }
    static bool direct_eligible() noexcept { return false; }
    static bool isr_direct_safe() noexcept { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
using RtAo = coact::Ao<RtCtx, coact::Hsm<RtCtx>, RtTraits>;
}  // namespace

COACT_TEST(freertos_runtime_single_core_assembly)
{
    stub_freertos_reset_faults();
    static ResStandard res;
    coact::pal::FreeRtos pal(res);

    static constexpr uint16_t kBlk = 16U;
    static constexpr uint16_t kCap = 16U;
    alignas(16) static unsigned char storage[kBlk * kCap + kBlk];
    coact::EventPool<kBlk, kCap, coact::RttSingleCoreProfile> pool;
    pool.init(storage, sizeof(storage), coact::make_critical_section(pal));

    RtAo ao(kRtStates, 2U, kRtTrans, 1U, 1, 4U);
    coact::Event init_e{0U, 0U, 0U};
    ao.init(init_e);

    coact::Runtime<coact::DefaultConfig, coact::pal::FreeRtos,
                   coact::RttSingleCoreProfile> rt(pal);
    REQUIRE(rt.bind(&ao));
    REQUIRE(rt.initialize());
    REQUIRE(rt.start());

    g_rt_events.store(0, std::memory_order_relaxed);
    constexpr int kN = 6;
    for (int i = 0; i < kN; ++i) {
        coact::Event* e = pool.alloc(1U);
        REQUIRE(e != nullptr);
        const coact::EventQos qos{false, false};
        const coact::SubmitResult r =
            rt.coordinator().submit_from_task(coact::TargetId(1U), e, qos);
        REQUIRE(r.disposition != coact::SubmitDisposition::RejectedFull);
    }
    for (int w = 0; w < 200; ++w) {
        if (g_rt_events.load(std::memory_order_relaxed) >= kN) {
            break;
        }
        usleep(5000);
    }
    rt.stop();
    CHECK_EQ(g_rt_events.load(std::memory_order_relaxed), kN);
    CHECK_EQ(pool.used(), 0U);
}

}  // namespace

COACT_TEST_MAIN()
