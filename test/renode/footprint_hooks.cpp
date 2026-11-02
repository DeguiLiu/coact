// Shared FreeRTOS hooks for the footprint images: static idle task storage and
// fault paths that reset instead of returning. Linked into both images so the
// comparison keeps the same kernel configuration.
// SPDX-License-Identifier: MIT

#include "FreeRTOS.h"
#include "output.hpp"
#include "task.h"

namespace {

StaticTask_t g_idle_tcb{};
StackType_t g_idle_stack[configMINIMAL_STACK_SIZE]{};

[[noreturn]] void reset_now(const char* tag) {
  harness::puts("FAIL ");
  harness::puts(tag);
  harness::newline();
  *reinterpret_cast<volatile uint32_t*>(0xE000ED0CUL) = 0x05FA0004UL;
  for (;;) {}
}

}  // namespace

extern "C" void vApplicationGetIdleTaskMemory(StaticTask_t** tcb, StackType_t** stack, uint32_t* size) {
  *tcb = &g_idle_tcb;
  *stack = g_idle_stack;
  *size = configMINIMAL_STACK_SIZE;
}

extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char*) {
  reset_now("footprint.stack_overflow");
}

extern "C" void freertos_assert_failed(void) {
  reset_now("footprint.assert");
}

extern "C" void abort(void) {
  reset_now("footprint.abort");
}
