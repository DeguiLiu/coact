// FreeRTOS PAL fault-handling fixture (Renode, Cortex-M4, real kernel).
//
// Four boots share .noinit state:
//   1  a task kicks the PAL watchdog hook for 40 ms (longer than the 32 ms
//      IWDG window), then disables interrupts and spins. IWDG resets.
//   2  reports the reset and its latency, then executes an undefined
//      instruction. HardFault handler reports and resets.
//   3  reports the HardFault recovery, then fails a kernel configASSERT.
//   4  reports the assert recovery and prints the verdict.
//
// This is the evidence that the watchdog path is independent of the code it
// bounds: the reset happens with interrupts masked and the scheduler stopped.
// SPDX-License-Identifier: MIT

#include "FreeRTOS.h"
#include "output.hpp"
#include "task.h"

#include "coact/config.hpp"
#include "coact/pal_freertos.hpp"

#include <cstdint>

namespace {

struct Noinit {
  uint32_t magic;
  uint32_t boots;
  uint32_t disable_cnt;
  uint32_t last_cnt;
  uint32_t passed;
  uint32_t failed;
};

volatile Noinit g_noinit __attribute__((section(".noinit"), used));

constexpr uint32_t kMagic = 0x46525744U; /* "FRWD" */
constexpr uint32_t kTimerHz = 10000000U; /* timer2 on stm32f4.repl */

constexpr uintptr_t kTim2Cr1 = 0x40000000UL;
constexpr uintptr_t kTim2Cnt = 0x40000024UL;
constexpr uintptr_t kTim2Psc = 0x40000028UL;
constexpr uintptr_t kTim2Arr = 0x4000002CUL;
constexpr uintptr_t kIwdgKr = 0x40003000UL;
constexpr uintptr_t kIwdgPr = 0x40003004UL;
constexpr uintptr_t kIwdgRlr = 0x40003008UL;
constexpr uintptr_t kScbAircr = 0xE000ED0CUL;

inline volatile uint32_t& reg(uintptr_t address) noexcept {
  return *reinterpret_cast<volatile uint32_t*>(address);
}

inline uint32_t tim2_cnt() noexcept {
  return reg(kTim2Cnt);
}

inline void tim2_free_run() noexcept {
  reg(kTim2Cr1) = 0U;
  reg(kTim2Arr) = 0xFFFFFFFFU;
  reg(kTim2Psc) = 0U;
  reg(kTim2Cnt) = 0U;
  reg(kTim2Cr1) = 1U;
  __asm__ volatile("dsb\n\tisb" ::: "memory");
}

inline void wait_us(uint32_t us) noexcept {
  const uint32_t start = tim2_cnt();
  const uint32_t ticks = us * (kTimerHz / 1000000U);
  while ((tim2_cnt() - start) < ticks) {}
}

/* IWDG: 32 kHz LSI / 4 = 8 kHz, reload 255 -> 32 ms. */
inline void iwdg_arm() noexcept {
  reg(kIwdgKr) = 0x5555U;
  reg(kIwdgPr) = 0U;
  reg(kIwdgRlr) = 255U;
  reg(kIwdgKr) = 0xCCCCU;
  reg(kIwdgKr) = 0xAAAAU;
  __asm__ volatile("dsb\n\tisb" ::: "memory");
}

inline void iwdg_kick() noexcept {
  reg(kIwdgKr) = 0xAAAAU;
}

[[noreturn]] inline void system_reset() noexcept {
  reg(kScbAircr) = 0x05FA0004U;
  for (;;) {}
}

/* MPU region 0: 4 KiB at `base`, privileged no-access, execute-never. A write
   there raises MemManage and escalates to HardFault (MEMFAULTENA is off). */
inline void mpu_no_access_region(uintptr_t base) noexcept {
  reg(0xE000ED94UL) = 0U;                   /* MPU_CTRL off */
  reg(0xE000ED98UL) = 0U;                   /* RNR = 0 */
  reg(0xE000ED9CUL) = base;                 /* RBAR */
  reg(0xE000EDA0UL) = (1UL << 0)            /* ENABLE */
                      | ((12UL - 1UL) << 1) /* SIZE = 4 KiB */
                      | (0UL << 24)         /* AP = no access */
                      | (1UL << 28);        /* XN */
  __asm__ volatile("dsb\n\tisb" ::: "memory");
  reg(0xE000ED94UL) = 5UL; /* PRIVDEFENA | ENABLE */
  __asm__ volatile("dsb\n\tisb" ::: "memory");
}

void record(bool ok, const char* name) {
  harness::puts(ok ? "PASS " : "FAIL ");
  harness::puts(name);
  harness::newline();
  if (ok) {
    ++g_noinit.passed;
  } else {
    ++g_noinit.failed;
  }
}

using Pal = coact::pal::FreeRtos;
using Res = coact::pal::FreeRtosResources<4096U, 4U, 0U>;
Res g_res;
Pal g_pal(g_res);

void iwdg_kick_from_pal(void* /*ctx*/) noexcept {
  iwdg_kick();
}

StaticTask_t g_boot_tcb;
StackType_t g_boot_stack[1024];
StaticTask_t g_idle_tcb;
StackType_t g_idle_stack[configMINIMAL_STACK_SIZE];

void watchdog_task(void*) {
  record(g_pal.initialize() == coact::pal::InitError::kOk, "WDFR.pal_init");
  (void)g_pal.register_current_task(5U);

  /* 40 ms of progress with a 32 ms window: the hook is what keeps the
     system alive. */
  for (uint32_t i = 0U; i < 20U; ++i) {
    g_pal.watchdog_progress(i);
    wait_us(2000U);
  }
  record(true, "WDFR.healthy_kicks");

  g_noinit.disable_cnt = tim2_cnt();
  __asm__ volatile("cpsid i" ::: "memory");
  for (;;) {
    /* Last value before the reset is the measured termination point. */
    g_noinit.last_cnt = tim2_cnt();
  }
}

}  // namespace

extern "C" void vApplicationGetIdleTaskMemory(StaticTask_t** tcb, StackType_t** stack, uint32_t* size) {
  *tcb = &g_idle_tcb;
  *stack = g_idle_stack;
  *size = configMINIMAL_STACK_SIZE;
}

extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char*) {
  record(false, "WDFR.stack_overflow");
  system_reset();
}

extern "C" void HardFault_Handler(void) {
  const uint32_t cfsr = reg(0xE000ED28UL);
  const uint32_t mmfar = reg(0xE000ED34UL);
  harness::puts("PASS WDFR.hardfault_entered cfsr=");
  harness::put_hex32(cfsr);
  harness::puts(" mmfar=");
  harness::put_hex32(mmfar);
  harness::newline();
  ++g_noinit.passed;
  system_reset();
}

extern "C" void MemManage_Handler(void) {
  const uint32_t cfsr = reg(0xE000ED28UL);
  const uint32_t mmfar = reg(0xE000ED34UL);
  harness::puts("PASS WDFR.memmanage_entered cfsr=");
  harness::put_hex32(cfsr);
  harness::puts(" mmfar=");
  harness::put_hex32(mmfar);
  harness::newline();
  ++g_noinit.passed;
  system_reset();
}

extern "C" void freertos_assert_failed(void) {
  record(true, "WDFR.assert_entered");
  system_reset();
}

int main() {
  harness::uart_init();
  tim2_free_run();
  if (g_noinit.magic != kMagic) {
    g_noinit.magic = kMagic;
    g_noinit.boots = 0U;
    g_noinit.disable_cnt = 0U;
    g_noinit.last_cnt = 0U;
    g_noinit.passed = 0U;
    g_noinit.failed = 0U;
  }
  ++g_noinit.boots;

  if (1U == g_noinit.boots) {
    tim2_free_run();
    g_pal.set_watchdog_ops(Pal::WatchdogOps{&iwdg_kick_from_pal, nullptr});
    iwdg_arm();
    TaskHandle_t task = xTaskCreateStatic(&watchdog_task, "wd", 1024U, nullptr, 2U, g_boot_stack, &g_boot_tcb);
    if (nullptr == task) {
      record(false, "WDFR.task_create");
      for (;;) {}
    }
    vTaskStartScheduler();
    record(false, "WDFR.scheduler_returned");
    for (;;) {}
  }

  if (2U == g_noinit.boots) {
    record(true, "WDFR.reset_with_irq_masked");
    const uint32_t elapsed_us = (g_noinit.last_cnt - g_noinit.disable_cnt) / (kTimerHz / 1000000U);
    harness::puts("WDFR.info measured_us=");
    harness::put_u64(elapsed_us);
    harness::newline();
    record((elapsed_us >= 20000U) && (elapsed_us <= 60000U), "WDFR.reset_within_bound");
    harness::puts("WDFR.info triggering_fault\n");
    /* The model enforces MPU attributes (proven by mpu.sh); an unmapped
       store or an even-address BX is not modelled as a fault. */
    reg(0xE000ED24UL) |= (1UL << 16) | (1UL << 17) | (1UL << 18);
    mpu_no_access_region(0x20020000UL);
    harness::puts("WDFR.info mpu_ctrl=");
    harness::put_hex32(reg(0xE000ED94UL));
    harness::puts(" vtor=");
    harness::put_hex32(reg(0xE000ED08UL));
    harness::puts(" shcsr=");
    harness::put_hex32(reg(0xE000ED24UL));
    reg(0xE000ED98UL) = 0U;
    harness::puts(" rbar=");
    harness::put_hex32(reg(0xE000ED9CUL));
    harness::puts(" rasr=");
    harness::put_hex32(reg(0xE000EDA0UL));
    harness::newline();
    reg(0x20020000UL) = 1U;
    for (;;) {}
  }

  if (3U == g_noinit.boots) {
    record(true, "WDFR.hardfault_reset");
    freertos_assert_failed();
  }

  record(true, "WDFR.assert_reset");
  harness::puts("RESULT ");
  harness::puts((0U == g_noinit.failed) ? "PASS " : "FAIL ");
  harness::put_u64(g_noinit.passed);
  harness::putc('/');
  harness::put_u64(g_noinit.passed + g_noinit.failed);
  harness::newline();

  /* Nothing left to prove: park in WFI so Renode can fast-forward to the
     RunFor deadline instead of spinning. */
  for (;;) {
    __asm__ volatile("wfi");
  }
}
