// FreeRTOS-only footprint baseline: scheduler, one static task, one counting
// semaphore, UART output. This is the smallest useful application the coact
// image is compared against; it pulls no coact header.
// SPDX-License-Identifier: MIT

#include "FreeRTOS.h"
#include "output.hpp"
#include "semphr.h"
#include "task.h"

namespace {

StaticTask_t g_boot_tcb{};
StackType_t g_boot_stack[512]{};
StaticSemaphore_t g_sem_storage{};

void bootstrap(void*) {
  SemaphoreHandle_t sem = xSemaphoreCreateCountingStatic(4U, 0U, &g_sem_storage);
  if (nullptr == sem) {
    harness::puts("FAIL baseline.sem_create\n");
    vTaskSuspend(nullptr);
  }
  (void)xSemaphoreGive(sem);
  if (pdTRUE != xSemaphoreTake(sem, portMAX_DELAY)) {
    harness::puts("FAIL baseline.sem_take\n");
    vTaskSuspend(nullptr);
  }
  const UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
  harness::puts("BASELINE PASS stack_hwm_words=");
  harness::put_u64(hwm);
  harness::newline();
  vTaskSuspend(nullptr);
}

}  // namespace

int main() {
  harness::uart_init();
  TaskHandle_t boot = xTaskCreateStatic(&bootstrap, "boot", 512U, nullptr, 2U, g_boot_stack, &g_boot_tcb);
  if (nullptr == boot) {
    harness::puts("FAIL baseline.task_create\n");
    for (;;) {}
  }
  vTaskStartScheduler();
  harness::puts("FAIL baseline.scheduler_returned\n");
  for (;;) {}
}
