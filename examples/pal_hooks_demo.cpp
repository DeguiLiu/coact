// BSP-level PalHooks adapter: the example, not coact core, owns the three
// AUTOSAR-style entry points.
//
//   on_error       diagnostic for a coact call that returned failure
//   on_protection  called by the BSP MemManage/HardFault handler
//   on_panic       installed through coact::set_fatal_handler()
//
// On target the protection and panic paths reset the controller after logging.
// Here they record evidence and the panic path runs in a forked child so the
// process can verify that fatal_assert() reached the hook before abort().
// A single first-fault latch is captured before any hook runs. It is not a
// second fault manager: normal on_error may forward to the product FCCU,
// while protection/panic keep this lock-free latch on the critical path.
// A hook that re-enters the adapter is counted and suppressed.
//
// SPDX-License-Identifier: MIT

#include "coact/assert.hpp"
#include "coact/event.hpp"
#include "coact/pal.hpp"
#include "coact/pool.hpp"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/wait.h>
#include <unistd.h>

namespace {

enum class PalError : int32_t {
  kPoolExhausted = 1,
  kServiceRejected = 2,
};

enum class FaultKind : uint8_t { kNone, kError, kProtection, kPanic };

// Critical-path snapshot of the first fault only. No queue, allocation or
// consumer task, so it is safe to fill before resetting the controller.
struct FirstFaultLatch {
  FaultKind kind = FaultKind::kNone;
  const char* where = nullptr;
  uint32_t detail0 = 0U;
  uint32_t detail1 = 0U;
  int32_t line = 0;
};

struct PalHooks {
  void (*on_error)(void* ctx, PalError code, const char* where) = nullptr;
  void (*on_protection)(void* ctx, uint32_t cfsr, uint32_t mmfar) = nullptr;
  void (*on_panic)(void* ctx, const char* file, int32_t line) = nullptr;
  void* ctx = nullptr;
};

struct HookState {
  int errors = 0;
  int protections = 0;
  int panics = 0;
  uint32_t total_faults = 0U;
  uint32_t reentry_blocked = 0U;
  FirstFaultLatch first{};
  bool in_hook = false;
};

int g_panic_pipe = -1;
bool g_panic_reentry = false;
PalHooks g_hooks{};

void record_fault(HookState* state, FaultKind kind, const char* where, uint32_t detail0, uint32_t detail1,
                  int32_t line) noexcept {
  if (nullptr == state) {
    return;
  }
  ++state->total_faults;
  if (FaultKind::kNone == state->first.kind) {
    state->first = FirstFaultLatch{kind, where, detail0, detail1, line};
  }
  if (state->in_hook) {
    ++state->reentry_blocked;
    return;
  }
  state->in_hook = true;
  switch (kind) {
    case FaultKind::kError:
      if (nullptr != g_hooks.on_error) {
        g_hooks.on_error(state, static_cast<PalError>(detail0), where);
      }
      break;
    case FaultKind::kProtection:
      if (nullptr != g_hooks.on_protection) {
        g_hooks.on_protection(state, detail0, detail1);
      }
      break;
    case FaultKind::kPanic:
      if (nullptr != g_hooks.on_panic) {
        g_hooks.on_panic(state, where, line);
      }
      break;
    case FaultKind::kNone:
    default:
      break;
  }
  state->in_hook = false;
}

void fatal_bridge(const char* file, int line) {
  record_fault(static_cast<HookState*>(g_hooks.ctx), FaultKind::kPanic, file, 0U, 0U, line);
  // fatal_assert() still aborts after this returns.
}

namespace bsp {

void install_hooks(PalHooks hooks) {
  g_hooks = hooks;
  (void)coact::set_fatal_handler(&fatal_bridge);
}

// Every coact call that returns a status goes through this wrapper. The
// return value stays authoritative; the hook is diagnostic only.
bool check(bool ok, PalError code, const char* where) {
  if (!ok) {
    record_fault(static_cast<HookState*>(g_hooks.ctx), FaultKind::kError, where, static_cast<uint32_t>(code), 0U, 0);
  }
  return ok;
}

// Target equivalent: called from MemManage_Handler / HardFault_Handler with
// CFSR and MMFAR already latched. The BSP resets after the hook returns.
void protection_fault(uint32_t cfsr, uint32_t mmfar) {
  record_fault(static_cast<HookState*>(g_hooks.ctx), FaultKind::kProtection, "MemManage_Handler", cfsr, mmfar, 0);
}

// Called by the BSP when an invariant break must take the panic path. The
// adapter is the only owner of latching and re-entry control.
void panic(const char* file, int32_t line) {
  record_fault(static_cast<HookState*>(g_hooks.ctx), FaultKind::kPanic, file, 0U, 0U, line);
}

}  // namespace bsp

void on_error(void* ctx, PalError code, const char* where) {
  auto* state = static_cast<HookState*>(ctx);
  ++state->errors;
  // Product integration point: forward recoverable software faults to the
  // FCCU here. Do not route protection/panic through an FCCU consumer queue.
  std::printf("[error] code=%d where=%s\n", static_cast<int>(code), where);
}

void on_protection(void* ctx, uint32_t cfsr, uint32_t mmfar) {
  auto* state = static_cast<HookState*>(ctx);
  ++state->protections;
  std::printf("[protection] cfsr=0x%08X mmfar=0x%08X\n", static_cast<unsigned>(cfsr), static_cast<unsigned>(mmfar));
}

void on_panic(void* ctx, const char* file, int32_t line) {
  auto* state = static_cast<HookState*>(ctx);
  ++state->panics;
  if (g_panic_reentry) {
    // A hook-internal fault must not recurse through the same hooks.
    g_panic_reentry = false;
    bsp::panic(file, line);
  }
  // 'P' proves both that the hook ran and that the inner fault was suppressed.
  const char marker = (1U == state->reentry_blocked) ? 'P' : 'F';
  if (g_panic_pipe >= 0) {
    (void)::write(g_panic_pipe, &marker, 1U);
  }
}

}  // namespace

int main() {
  HookState state{};
  bsp::install_hooks(PalHooks{&on_error, &on_protection, &on_panic, &state});

  static coact::SpinCriticalSection spin;
  alignas(16) static unsigned char storage[16U * 2U];
  coact::EventPool<16U, 1U, coact::RttSingleCoreProfile> pool;
  pool.init(storage, sizeof(storage), coact::make_spin_critical_section(spin));

  const bool first = bsp::check(nullptr != pool.alloc(1U), PalError::kPoolExhausted, "EventPool::alloc/1");
  const bool second = bsp::check(nullptr != pool.alloc(1U), PalError::kPoolExhausted, "EventPool::alloc/2");

  bsp::protection_fault(0x00000082U, 0x20020000U);
  const bool first_latched = (FaultKind::kError == state.first.kind) && (1U == state.first.detail0) &&
                             (0 == std::strcmp(state.first.where, "EventPool::alloc/2")) &&
                             (2U == state.total_faults) && (0U == state.reentry_blocked);

  int pipefd[2] = {-1, -1};
  bool panic_ok = false;
  if (0 == ::pipe(pipefd)) {
    g_panic_pipe = pipefd[1];
    const pid_t pid = ::fork();
    if (0 == pid) {
      (void)::close(pipefd[0]);
      g_panic_reentry = true;
      coact::fatal_assert(__FILE__, __LINE__);
      std::_Exit(1);
    }
    (void)::close(pipefd[1]);
    if (pid > 0) {
      char markers[2] = {'\0', '\0'};
      size_t total = 0U;
      while (total < sizeof(markers)) {
        const ssize_t got = ::read(pipefd[0], markers + total, sizeof(markers) - total);
        if (got <= 0) {
          break;
        }
        total += static_cast<size_t>(got);
      }
      (void)::close(pipefd[0]);
      int status = 0;
      (void)::waitpid(pid, &status, 0);
      panic_ok = (1U == total) && ('P' == markers[0]) && WIFSIGNALED(status) && (SIGABRT == WTERMSIG(status));
    }
  }
  g_panic_pipe = -1;

  const bool errors_ok = first && !second && (1 == state.errors);
  const bool protections_ok = (1 == state.protections);
  std::printf("hooks: errors=%d protections=%d panic=%s first=%u reentry=%s\n", state.errors, state.protections,
              panic_ok ? "routed" : "missed", static_cast<unsigned>(state.first.kind),
              panic_ok ? "blocked" : "unverified");
  const bool all_ok = first_latched && errors_ok && protections_ok && panic_ok;
  std::printf("RESULT %s\n", all_ok ? "PASS" : "FAIL");
  return all_ok ? 0 : 1;
}
