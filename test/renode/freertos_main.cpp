// coact FreeRTOS PAL fixture for Renode (Cortex-M4 model, real FreeRTOS V11.1
// kernel + Cortex-M3 port). Proves the PAL on the target ABI: SVC/PendSV
// scheduling, SysTick, task/ISR critical-section split, Sem/Mutex/SoftIrq,
// resident workers and one Runtime assembly.
//
// Output contract matches the other fixtures: one PASS/FAIL line per
// assertion, then RESULT PASS n/m.
// SPDX-License-Identifier: MIT

#include <atomic>
#include <cstdint>
#include <new>

#include "FreeRTOS.h"
#include "task.h"

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/pal_freertos.hpp"
#include "coact/pool.hpp"
#include "coact/runtime.hpp"

#include "output.hpp"

namespace {

uint32_t g_pass = 0U;
uint32_t g_fail = 0U;

void record(bool ok, const char* name)
{
    harness::puts(ok ? "PASS " : "FAIL ");
    harness::puts(name);
    harness::newline();
    if (ok) {
        ++g_pass;
    }
    else {
        ++g_fail;
    }
}

void record_u64(const char* name, uint64_t got, uint64_t want)
{
    if (got == want) {
        record(true, name);
        return;
    }
    harness::puts("FAIL ");
    harness::puts(name);
    harness::puts(" got=");
    harness::put_u64(got);
    harness::puts(" want=");
    harness::put_u64(want);
    harness::newline();
    ++g_fail;
}

// ---------------------------------------------------------------------------
// Timer2 / NVIC (same wiring as the admission fixture: 10 MHz, IRQ 28).
// ---------------------------------------------------------------------------
namespace hw {

constexpr uintptr_t kTimer2Base = 0x40000000UL;
constexpr uint32_t kTimer2Irq = 28U;
constexpr uint32_t kCr1 = 0x00U / 4U;
constexpr uint32_t kDier = 0x0CU / 4U;
constexpr uint32_t kSr = 0x10U / 4U;
constexpr uint32_t kPsc = 0x28U / 4U;
constexpr uint32_t kArr = 0x2CU / 4U;
constexpr uintptr_t kNvicIser = 0xE000E100UL;
constexpr uintptr_t kNvicIpr = 0xE000E400UL;

inline volatile uint32_t* timer2(uint32_t index) noexcept
{
    return reinterpret_cast<volatile uint32_t*>(kTimer2Base) + index;
}

inline void timer2_start(uint32_t period_counts) noexcept
{
    *timer2(kArr) = period_counts;
    *timer2(kPsc) = 0U;
    *timer2(kSr) = 0U;
    *timer2(kDier) = 1U;
    *timer2(kCr1) = 1U;
}

inline void timer2_stop() noexcept
{
    *timer2(kCr1) = 0U;
    *timer2(kDier) = 0U;
    *timer2(kSr) = 0U;
}

inline void nvic_enable(uint32_t irq) noexcept
{
    volatile uint32_t* iser =
        reinterpret_cast<volatile uint32_t*>(kNvicIser) + (irq / 32U);
    *iser = 1U << (irq % 32U);
}

inline void nvic_set_priority(uint32_t irq, uint8_t prio) noexcept
{
    /* STM32F4 implements the top 4 priority bits; the byte holds them in the
       high nibble. 6 >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (5). */
    volatile uint8_t* ipr = reinterpret_cast<volatile uint8_t*>(kNvicIpr) + irq;
    *ipr = static_cast<uint8_t>(prio << 4U);
}

}  // namespace hw

// ---------------------------------------------------------------------------
// HSM fixture (one Internal action on signal 1).
// ---------------------------------------------------------------------------
struct Ctx {
    int actions = 0;
};

void ctx_entry(Ctx&) {}
void ctx_exit(Ctx&) {}
bool ctx_guard(const Ctx&, const coact::Event&) { return true; }
void ctx_action(Ctx& ctx, const coact::Event&) { ++ctx.actions; }

const coact::StateDef<Ctx> kStates[] = {
    {-1, nullptr, nullptr},
    {0, ctx_entry, ctx_exit},
};

const coact::TransitionDef<Ctx> kTransitions[] = {
    {1, 1U, 1, coact::TransitionKind::Internal, ctx_guard, ctx_action},
};

struct StagedTraits {
    static coact::LogicalPrio logical_prio() { return 11U; }
    static coact::PriorityClass priority_class()
    {
        return coact::PriorityClass::Normal;
    }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using StagedAo = coact::Ao<Ctx, coact::Hsm<Ctx>, StagedTraits>;

struct Cfg {
    enum : uint8_t {
        kMaxAo = 2U,
        kMaxStateDepth = 6U,
        kMaxDirectDepth = 4U,
        kBatchSizeMax = 4U
    };
    enum : uint16_t {
        kHighCapacity = 4U,
        kNormalCapacity = 8U,
        kLowCapacity = 8U,
        kCooldownCycles = 3U
    };
    enum : uint32_t {
        kBatchTimeoutMs = 1U,
        kLowMaxWaitMs = 10U,
        kDispatcherStackBytes = 4096U
    };
    enum : uint64_t {
        kDirectBudgetNs = 50000ULL,
        kRtcBudgetNs = 1000000ULL
    };
};

using Pal = coact::pal::FreeRtos;
using Res = coact::pal::FreeRtosResources<4096U, 8U, 4U, 1024U>;
using Rt = coact::Runtime<Cfg, Pal, Pal::Profile>;

Res g_res;
Pal g_pal(g_res);

// ISR -> PAL hand-off.
Pal::SoftIrqHandle g_isr_mail;
Pal::SemHandle g_isr_sem;
volatile uint32_t g_isr_count = 0U;

extern "C" void TIM2_IRQHandler(void)
{
    hw::timer2_stop();
    ++g_isr_count;
    (void)g_pal.softirq_raise(g_isr_mail, 7);
    g_pal.sem_release_from_isr(g_isr_sem);
}

// Worker used by the ThreadOps check.
std::atomic<int> g_worker_runs{0};
void worker_entry(void*) noexcept
{
    g_worker_runs.fetch_add(1, std::memory_order_relaxed);
}

coact::Event make_event(uint16_t signal) noexcept
{
    coact::Event e{};
    e.signal = signal;
    e.pool_id = 0U;
    e.ref_ctr = 0U;
    return e;
}

// ---------------------------------------------------------------------------
// Static task storage for the bootstrap task and idle task.
// ---------------------------------------------------------------------------
StaticTask_t g_boot_tcb;
StackType_t g_boot_stack[2048];
StaticTask_t g_idle_tcb;
StackType_t g_idle_stack[configMINIMAL_STACK_SIZE];

alignas(16) unsigned char g_pool_storage[16U * 16U + 16U];
coact::EventPool<16U, 16U, coact::RttSingleCoreProfile> g_pool;

void print_sizes()
{
    harness::puts("FR.info pal=");
    harness::put_u64(sizeof(Pal));
    harness::puts(" res=");
    harness::put_u64(sizeof(Res));
    harness::puts(" boot_stack=");
    harness::put_u64(sizeof(g_boot_stack));
    harness::newline();
}

void bootstrap(void*)
{
    /* 1. Explicit initialization and context registration. */
    const int init_rc = static_cast<int>(g_pal.initialize());
    record(static_cast<int>(coact::pal::InitError::kOk) == init_rc,
           "FR.pal_init");
    record_u64("FR.stack_size", g_pal.dispatcher_stack_bytes(), 4096U);
    record(g_pal.register_current_task(5U), "FR.register_task");
    record(!g_pal.register_current_task(5U), "FR.register_dup_rejected");
    const coact::ExecutionContext ctx = g_pal.current_context();
    record((ctx.kind == coact::ContextKind::Task) && ctx.prio_valid
               && (ctx.logical_prio == 5U),
           "FR.context_task");

    /* 2. SysTick-backed clock really advances. */
    const uint64_t ns0 = g_pal.monotonic_ns();
    vTaskDelay(5U);
    const uint64_t ns1 = g_pal.monotonic_ns();
    record((ns1 > ns0) && (ns1 >= 5000000ULL), "FR.clock_advances");
    record_u64("FR.clock_resolution", g_pal.clock_resolution_ns(), 1000000U);

    /* 3. Sem / Mutex on the target kernel. */
    Pal::SemHandle sem;
    record(g_pal.sem_init(sem, 0U), "FR.sem_init");
    record(!g_pal.sem_take(sem, 0U), "FR.sem_empty");
    g_pal.sem_release(sem);
    record(g_pal.sem_take(sem, 1U), "FR.sem_take");
    g_pal.sem_deinit(sem);

    Pal::MutexHandle mutex;
    record(g_pal.mutex_init(mutex), "FR.mutex_init");
    g_pal.mutex_lock(mutex);
    g_pal.mutex_unlock(mutex);
    record(true, "FR.mutex_roundtrip");
    g_pal.mutex_deinit(mutex);

    /* 4. Real TIM2 interrupt enters the ISR branch of the PAL. */
    record(g_pal.softirq_init(g_isr_mail), "FR.softirq_init");
    record(g_pal.sem_init(g_isr_sem, 0U), "FR.isr_sem_init");
    hw::nvic_set_priority(hw::kTimer2Irq, 6U);
    hw::nvic_enable(hw::kTimer2Irq);
    hw::timer2_start(100000U);  /* 10 ms at 10 MHz, one shot (handler stops) */
    const int32_t payload = g_pal.softirq_take(g_isr_mail, 200U);
    record_u64("FR.isr_softirq_payload", static_cast<uint64_t>(payload), 7U);
    record_u64("FR.isr_count", g_isr_count, 1U);
    record(g_pal.sem_take(g_isr_sem, 0U), "FR.isr_sem_release");
    g_pal.sem_deinit(g_isr_sem);
    g_pal.softirq_deinit(g_isr_mail);

    /* 5. Resident worker: create, run, join, run again. */
    Pal::ThreadHandle th;
    g_worker_runs.store(0, std::memory_order_relaxed);
    record(g_pal.thread_create(th, &worker_entry, nullptr), "FR.thread_create");
    g_pal.thread_join(th);
    record_u64("FR.thread_join", static_cast<uint64_t>(
                                     g_worker_runs.load(std::memory_order_relaxed)),
               1U);
    record(g_pal.thread_create(th, &worker_entry, nullptr), "FR.thread_reuse");
    g_pal.thread_join(th);

    /* 6. Runtime end-to-end: dispatcher task, staging, event pool. */
    g_pool.init(g_pool_storage, sizeof(g_pool_storage),
                coact::make_critical_section(g_pal));
    /* Placement-new into static storage: a function-local static with a
       destructor pulls in atexit(), which a freestanding image does not have. */
    alignas(StagedAo) static unsigned char ao_storage[sizeof(StagedAo)];
    auto* ao = new (ao_storage) StagedAo(kStates, 2U, kTransitions, 1U, 1, 4U);
    ao->init(make_event(0U));
    alignas(Rt) static unsigned char rt_storage[sizeof(Rt)];
    auto* rt = new (rt_storage) Rt(g_pal);
    record(rt->bind(ao), "FR.runtime_bind");
    record(rt->initialize(), "FR.runtime_init");
    record(rt->start(), "FR.runtime_start");

    const coact::TargetId t1(1U);
    const coact::EventQos qos{false, false};
    constexpr int kN = 8;
    int accepted = 0;
    for (int i = 0; i < kN; ++i) {
        coact::Event* e = g_pool.alloc(1U);
        if (nullptr == e) {
            continue;
        }
        const coact::SubmitResult r =
            rt->coordinator().submit_from_task(t1, e, qos);
        if (coact::SubmitDisposition::RejectedFull != r.disposition) {
            ++accepted;
        }
    }
    record_u64("FR.runtime_submitted", static_cast<uint64_t>(accepted), kN);

    uint32_t spins = 0U;
    while ((ao->context().actions < kN) && (spins < 200U)) {
        vTaskDelay(1U);
        ++spins;
    }
    record_u64("FR.runtime_actions",
               static_cast<uint64_t>(ao->context().actions), kN);
    const UBaseType_t hwm =
        uxTaskGetStackHighWaterMark(g_pal.dispatcher_task_handle());
    harness::puts("FR.info dispatcher_hwm_words=");
    harness::put_u64(static_cast<uint64_t>(hwm));
    harness::newline();
    record(hwm >= (4096U / sizeof(StackType_t)) / 4U,
           "FR.dispatcher_stack_watermark");
    rt->stop();
    record_u64("FR.runtime_pool_used", g_pool.used(), 0U);

    print_sizes();
    harness::puts("RESULT ");
    harness::puts((0U == g_fail) ? "PASS " : "FAIL ");
    harness::put_u64(g_pass);
    harness::putc('/');
    harness::put_u64(g_pass + g_fail);
    harness::newline();

    /* FreeRTOS task functions must never return. */
    vTaskSuspend(nullptr);
    for (;;) {
    }
}

}  // namespace

extern "C" void vApplicationGetIdleTaskMemory(StaticTask_t** tcb,
                                              StackType_t** stack,
                                              uint32_t* size)
{
    *tcb = &g_idle_tcb;
    *stack = g_idle_stack;
    *size = configMINIMAL_STACK_SIZE;
}

extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char*)
{
    /* configCHECK_FOR_STACK_OVERFLOW == 2: reaching this hook is a fixture
       failure, not something to recover from. */
    harness::puts("FAIL FR.stack_overflow\n");
    for (;;) {
    }
}

extern "C" void abort(void) noexcept
{
    harness::puts("FAIL FR.abort\n");
    for (;;) {
    }
}

int main()
{
    harness::uart_init();
    TaskHandle_t boot = xTaskCreateStatic(&bootstrap, "boot", 2048U, nullptr,
                                          2U, g_boot_stack, &g_boot_tcb);
    if (nullptr == boot) {
        harness::puts("FAIL FR.boot_create\n");
        for (;;) {
        }
    }
    vTaskStartScheduler();
    harness::puts("FAIL FR.scheduler_returned\n");
    for (;;) {
    }
}
