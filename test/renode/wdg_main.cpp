/*
 * coact watchdog fixture (Renode, Cortex-M4, bare metal).
 *
 * Design 4.3: an execution budget can only be reported after the handler
 * returns, so a non-returning action can be bounded only by an independent
 * watchdog. This fixture measures that claim instead of asserting it:
 *
 *   boot 1  a real AO action, dispatched through the coordinator, never
 *           returns. The independent watchdog (IWDG, ~32 ms) resets the
 *           machine while the loop keeps sampling a free-running 10 MHz
 *           counter into .noinit.
 *   boot 2  .noinit survived the reset, so the last sample is the measured
 *           termination latency. The same watchdog is armed again and a
 *           healthy loop that kicks it runs for 100 ms without a reset.
 *
 * Output contract matches the admission harness: one PASS/FAIL line per
 * assertion, then "RESULT WDG PASS n/m", so wdg.sh gates on the target's own
 * verdict rather than on a wall-clock timeout.
 */
#include "output.hpp"
#include "pal_baremetal.hpp"

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/coordinator.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/monitor.hpp"
#include "coact/staging.hpp"

#include <cstddef>
#include <cstdint>

namespace {

// ---------------------------------------------------------------------------
// .noinit state. Reset_Handler zeroes only .bss and a NOLOAD section carries
// no ELF image, so these words are the only evidence that survives the
// watchdog reset: boot count, and the last counter sample taken inside the
// runaway loop.
// ---------------------------------------------------------------------------
struct WdgNoinit {
  uint32_t magic;
  uint32_t boots;
  uint32_t last_cnt;
  uint32_t runaway_iters;
  uint32_t healthy_kicks;
};

volatile WdgNoinit g_noinit __attribute__((section(".noinit"), used));

constexpr uint32_t kMagic = 0x57444731U; /* "WDG1" */
constexpr uint32_t kTimerHz = 10000000U; /* timer2 on stm32f4.repl */
/* IWDG: 32 kHz LSI / 4 = 8 kHz, reload 255 -> 256 ticks = 32 ms. */
constexpr uint32_t kIwdgTimeoutUs = 32000U;

constexpr uintptr_t kTim2Cr1 = 0x40000000UL;
constexpr uintptr_t kTim2Cnt = 0x40000024UL;
constexpr uintptr_t kTim2Psc = 0x40000028UL;
constexpr uintptr_t kTim2Arr = 0x4000002CUL;
constexpr uintptr_t kIwdgKr = 0x40003000UL;
constexpr uintptr_t kIwdgPr = 0x40003004UL;
constexpr uintptr_t kIwdgRlr = 0x40003008UL;

inline volatile uint32_t& reg(uintptr_t address) noexcept {
  return *reinterpret_cast<volatile uint32_t*>(address);  // NOLINT
}

inline uint32_t tim2_cnt() noexcept {
  return reg(kTim2Cnt);
}

/* Free-running 10 MHz counter, no interrupt: the only clock the runaway loop
   can sample without depending on the code it is supposed to bound. */
inline void tim2_free_run() noexcept {
  reg(kTim2Cr1) = 0U;
  reg(kTim2Arr) = 0xFFFFFFFFU;
  reg(kTim2Psc) = 0U;
  reg(kTim2Cnt) = 0U;
  reg(kTim2Cr1) = 1U; /* CEN */
  __asm__ volatile("dsb\n\tisb" ::: "memory");
}

inline void iwdg_arm() noexcept {
  reg(kIwdgKr) = 0x5555U; /* unlock PR/RLR */
  reg(kIwdgPr) = 0U;      /* /4 -> 8 kHz */
  reg(kIwdgRlr) = 255U;   /* 256 ticks */
  reg(kIwdgKr) = 0xCCCCU; /* start */
  reg(kIwdgKr) = 0xAAAAU; /* reload */
  __asm__ volatile("dsb\n\tisb" ::: "memory");
}

inline void iwdg_kick() noexcept {
  reg(kIwdgKr) = 0xAAAAU;
}

inline void wait_us(uint32_t us) noexcept {
  const uint32_t start = tim2_cnt();
  const uint32_t ticks = us * (kTimerHz / 1000000U);
  while ((tim2_cnt() - start) < ticks) {}
}

inline uint64_t cnt_to_us(uint32_t counts) noexcept {
  return (static_cast<uint64_t>(counts) * 1000000ULL) / kTimerHz;
}

// ---------------------------------------------------------------------------
// Assertion reporting, same line format as the admission harness.
// ---------------------------------------------------------------------------
int g_pass = 0;
int g_fail = 0;

void record(bool ok, const char* name) {
  harness::puts(ok ? "PASS " : "FAIL ");
  harness::puts(name);
  harness::newline();
  if (ok) {
    ++g_pass;
  } else {
    ++g_fail;
  }
}

void record_u64(const char* name, uint64_t got, uint64_t want) {
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
// A minimal AO whose action never returns: exactly the case a software budget
// cannot report, because reporting happens after the handler returns.
// ---------------------------------------------------------------------------
struct RunCtx {
  int actions = 0;
};

void run_entry(RunCtx&) {}
void run_exit(RunCtx&) {}
bool run_guard(const RunCtx&, const coact::Event&) {
  return true;
}

void run_action(RunCtx& ctx, const coact::Event&) {
  ++ctx.actions;
  for (;;) {
    /* Sample first, iterate second: the stored counter is the instant the
       loop was still alive, so after the reset it reads as the termination
       latency rather than as an arbitrary older value. */
    g_noinit.last_cnt = tim2_cnt();
    ++g_noinit.runaway_iters;
  }
}

const coact::StateDef<RunCtx> kStates[] = {
    {-1, nullptr, nullptr},
    {0, run_entry, run_exit},
};

const coact::TransitionDef<RunCtx> kTransitions[] = {
    {1, 1U, 1, coact::TransitionKind::Internal, run_guard, run_action},
};

struct RunawayTraits {
  static coact::LogicalPrio logical_prio() { return 10U; }
  static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
  static bool direct_eligible() { return true; }
  static bool isr_direct_safe() { return false; }
  static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

struct WdgCfg {
  enum : uint8_t { kMaxAo = 1U, kMaxStateDepth = 4U, kMaxDirectDepth = 2U, kBatchSizeMax = 2U };
  enum : uint16_t { kHighCapacity = 2U, kNormalCapacity = 4U, kLowCapacity = 2U, kCooldownCycles = 1U };
  enum : uint32_t { kBatchTimeoutMs = 1U, kLowMaxWaitMs = 1U };
  enum : uint64_t { kDirectBudgetNs = 1000000ULL, kRtcBudgetNs = 1000000ULL };
};

using WdgStage = coact::Staging<WdgCfg, coact::SingleCoreCriticalRing>;
using WdgRegistry = coact::AoRegistry<WdgCfg>;
using WdgMonitor = coact::Monitor<WdgCfg>;
using WdgBreaker = coact::Breaker<WdgCfg>;
using WdgCoord = coact::DispatchCoordinator<WdgStage, harness::BarePal, WdgBreaker>;
using RunawayAo = coact::Ao<RunCtx, coact::Hsm<RunCtx>, RunawayTraits>;

coact::CriticalSection noop_cs() noexcept {
  return coact::CriticalSection{nullptr, [](void*) -> coact::CriticalSection::Token { return 0U; },
                                [](void*, coact::CriticalSection::Token) {}};
}

/* Does not return: the submission takes the direct path, so the runaway action
   executes inside submit_from_task and only a reset can end it. */
[[noreturn]] void run_runaway_submission() {
  WdgStage staging(noop_cs());
  WdgRegistry registry;
  WdgMonitor monitor;
  WdgCfg cfg{};
  WdgBreaker breaker(cfg);
  harness::BarePal pal;
  WdgCoord coord(staging, registry, monitor, breaker, pal);

  RunawayAo ao(kStates, 2U, kTransitions, 1U, 1, 4U);
  coact::Event init_evt{};
  init_evt.signal = 0U;
  init_evt.pool_id = 0U;
  init_evt.ref_ctr = 0U;
  ao.init(init_evt);
  (void)registry.bind(&ao, ao.logical_prio());

  coact::Event e{};
  e.signal = 1U;
  e.pool_id = 0U;
  e.ref_ctr = 0U;
  const coact::EventQos qos{false, false};
  (void)coord.submit_from_task(coact::TargetId(1U), &e, qos);
  for (;;) {}
}

}  // namespace

extern "C" {

[[noreturn]] void trap_line(const char* kind) {
  harness::puts("FAIL trap.");
  harness::puts(kind);
  harness::newline();
  harness::puts("RESULT WDG FAIL 0/0\r\n");
  for (;;) {}
}

}  // extern "C"

extern "C" void HardFault_Handler(void) {
  trap_line("hardfault");
}
extern "C" void MemManage_Handler(void) {
  trap_line("memmanage");
}
extern "C" void BusFault_Handler(void) {
  trap_line("busfault");
}
extern "C" void UsageFault_Handler(void) {
  trap_line("usagefault");
}
extern "C" void SVC_Handler(void) {
  trap_line("svc");
}
extern "C" void DebugMon_Handler(void) {
  trap_line("debugmon");
}
extern "C" void PendSV_Handler(void) {
  trap_line("pendsv");
}
extern "C" void SysTick_Handler(void) {
  trap_line("systick");
}
extern "C" void abort(void) noexcept {
  trap_line("abort");
}

extern "C" int main(void) {
  harness::uart_init();

  if (kMagic != g_noinit.magic) {
    g_noinit.magic = kMagic;
    g_noinit.boots = 0U;
    g_noinit.last_cnt = 0U;
    g_noinit.runaway_iters = 0U;
    g_noinit.healthy_kicks = 0U;
  }
  ++g_noinit.boots;
  const uint32_t boot = g_noinit.boots;

  harness::puts("WDG BOOT n=");
  harness::put_u64(boot);
  harness::newline();

  if (1U == boot) {
    harness::puts("WDG RUNAWAY configured_us=");
    harness::put_u64(kIwdgTimeoutUs);
    harness::newline();
    /* Start the measurement clock last so the reported latency covers the
       watchdog window and not the UART output before it. */
    tim2_free_run();
    iwdg_arm();
    run_runaway_submission();
  }

  /* ---- boot 2: the runaway was terminated; measure, then control ---- */
  const uint64_t latency_us = cnt_to_us(g_noinit.last_cnt);
  const uint64_t iterations = g_noinit.runaway_iters;

  record_u64("WDG.single_reset_only", boot, 2U);
  record(iterations > 0U, "WDG.runaway_action_entered");
  /* Two-sided bracket: a reset at ~0 us would mean something other than the
     watchdog restarted the machine, and a reset far past the configured
     timeout would mean the termination was not bounded. */
  record(latency_us >= (kIwdgTimeoutUs - 7000U), "WDG.terminated_after_configured_timeout");
  record(latency_us <= (kIwdgTimeoutUs + 13000U), "WDG.terminated_within_bound");

  harness::puts("WDG.info measured_us=");
  harness::put_u64(latency_us);
  harness::puts(" configured_us=");
  harness::put_u64(kIwdgTimeoutUs);
  harness::puts(" runaway_iters=");
  harness::put_u64(iterations);
  harness::newline();

  /* Control group: arm the same watchdog again and keep kicking it. If the
     boot-1 reset were an artifact of the model rather than a consequence of
     the missing kick, this loop would reset too. */
  g_noinit.healthy_kicks = 0U;
  tim2_free_run();
  iwdg_arm();
  for (uint32_t i = 0U; i < 100U; ++i) {
    wait_us(1000U);
    iwdg_kick();
    ++g_noinit.healthy_kicks;
  }
  const uint64_t healthy_us = cnt_to_us(tim2_cnt());

  record_u64("WDG.control_kicks", g_noinit.healthy_kicks, 100U);
  record_u64("WDG.control_no_reset", g_noinit.boots, 2U);
  record(healthy_us >= 100000U, "WDG.control_outlived_timeout");
  harness::puts("WDG.info healthy_us=");
  harness::put_u64(healthy_us);
  harness::newline();

  harness::puts("RESULT WDG ");
  harness::puts((0 == g_fail) ? "PASS " : "FAIL ");
  harness::put_u64(static_cast<uint64_t>(g_pass));
  harness::putc('/');
  harness::put_u64(static_cast<uint64_t>(g_pass + g_fail));
  harness::newline();

  /* IWDG cannot be stopped once started, so the terminal loop keeps kicking:
     parking without a kick would reboot the target and turn every assertion
     above into a boot-3 failure. */
  for (;;) {
    wait_us(1000U);
    iwdg_kick();
  }
}
