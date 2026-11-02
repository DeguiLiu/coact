// Minimal coact application for footprint measurement: one AO, one HSM
// transition, one static event pool, default capacities, one dispatcher.
// The boot task, stack and UART path match footprint_baseline_main.cpp.
// SPDX-License-Identifier: MIT

#include "FreeRTOS.h"
#include "output.hpp"
#include "task.h"

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/pal_freertos.hpp"
#include "coact/pool.hpp"
#include "coact/runtime.hpp"

#include <new>

namespace {

using Pal = coact::pal::FreeRtos;

#if defined(COACT_FOOTPRINT_SMALL)
struct Cfg {
  enum : uint8_t { kMaxAo = 4U, kMaxStateDepth = 6U, kMaxDirectDepth = 4U, kBatchSizeMax = 4U };
  enum : uint16_t {
    kHighCapacity = 8U,
    kNormalCapacity = 16U,
    kLowCapacity = 32U,
    kCooldownCycles = 100U,
    kHighCriticalReserve = 0U,
    kNormalReservedCapacity = 0U
  };
  enum : uint32_t { kBatchTimeoutMs = 5U, kLowMaxWaitMs = 100U, kDispatcherStackBytes = 4096U };
  enum : uint64_t { kDirectBudgetNs = 50000ULL, kRtcBudgetNs = 1000000ULL };
};
using Res = coact::pal::FreeRtosResources<4096U, 4U, 0U>;
#else
using Cfg = coact::DefaultConfig;
using Res = coact::pal::FreeRtosResources<4096U, 4U, 0U>;
#endif

Res g_res{};
Pal g_pal(g_res);

constexpr uint16_t kBlockBytes = 32U;
constexpr uint16_t kBlockCount = 8U;
alignas(16) unsigned char g_pool_storage[kBlockBytes * kBlockCount + kBlockBytes]{};
coact::EventPool<kBlockBytes, kBlockCount, coact::RttSingleCoreProfile> g_pool{};

StaticTask_t g_boot_tcb{};
StackType_t g_boot_stack[512]{};

struct Ctx {
  int actions = 0;
};

void on_event(Ctx& c, const coact::Event&) noexcept {
  ++c.actions;
}
bool guard(const Ctx&, const coact::Event&) noexcept {
  return true;
}

const coact::StateDef<Ctx> kStates[] = {
    {-1, nullptr, nullptr, nullptr},
    {0, nullptr, nullptr, nullptr},
};
const coact::TransitionDef<Ctx> kTransitions[] = {
    {1, 1U, 1, coact::TransitionKind::Internal, guard, on_event},
};

struct Traits {
  static coact::LogicalPrio logical_prio() noexcept { return 5U; }
  static coact::PriorityClass priority_class() noexcept { return coact::PriorityClass::Normal; }
  static bool direct_eligible() noexcept { return false; }
  static bool isr_direct_safe() noexcept { return false; }
  static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using Ao = coact::Ao<Ctx, coact::Hsm<Ctx>, Traits>;

void bootstrap(void*) {
  int failures = 0;
  if (coact::pal::InitError::kOk != g_pal.initialize()) {
    ++failures;
  }
  if (!g_pal.register_current_task(2U)) {
    ++failures;
  }

  g_pool.init(g_pool_storage, sizeof(g_pool_storage), coact::make_critical_section(g_pal));
  alignas(Ao) static unsigned char ao_storage[sizeof(Ao)];
  auto* ao = new (ao_storage) Ao(kStates, 2U, kTransitions, 1U, 1, 4U);
  coact::Event init_e{0U, 0U, 0U};
  ao->init(init_e);

  using Rt = coact::Runtime<Cfg, Pal, coact::RttSingleCoreProfile>;
  alignas(Rt) static unsigned char rt_storage[sizeof(Rt)];
  auto* rt = new (rt_storage) Rt(g_pal);
  if (!rt->bind(ao) || !rt->initialize() || !rt->start()) {
    ++failures;
  }

  constexpr int kEvents = 8;
  for (int i = 0; i < kEvents; ++i) {
    coact::Event* e = g_pool.alloc(1U);
    if (nullptr == e) {
      ++failures;
      break;
    }
    const coact::EventQos qos{false, false};
    if (coact::SubmitDisposition::RejectedFull ==
        rt->coordinator().submit_from_task(coact::TargetId(1U), e, qos).disposition) {
      ++failures;
    }
  }

  for (int wait = 0; (wait < 100) && (ao->context().actions < kEvents); ++wait) {
    vTaskDelay(1U);
  }
  rt->stop();

  if ((kEvents != ao->context().actions) || (0U != g_pool.used())) {
    ++failures;
  }
  harness::puts(failures == 0 ? "COACT PASS dispatcher_hwm_words=" : "COACT FAIL\n");
  if (0 == failures) {
    harness::put_u64(uxTaskGetStackHighWaterMark(g_pal.dispatcher_task_handle()));
    harness::newline();
  }
  vTaskSuspend(nullptr);
}

}  // namespace

int main() {
  harness::uart_init();
  TaskHandle_t boot = xTaskCreateStatic(&bootstrap, "boot", 512U, nullptr, 2U, g_boot_stack, &g_boot_tcb);
  if (nullptr == boot) {
    harness::puts("FAIL coact.task_create\n");
    for (;;) {}
  }
  vTaskStartScheduler();
  harness::puts("FAIL coact.scheduler_returned\n");
  for (;;) {}
}
