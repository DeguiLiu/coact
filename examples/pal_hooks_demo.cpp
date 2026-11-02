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

#include <sys/wait.h>
#include <unistd.h>

namespace {

enum class PalError : int32_t {
  kPoolExhausted = 1,
  kServiceRejected = 2,
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
};

int g_panic_pipe = -1;
PalHooks g_hooks{};

void fatal_bridge(const char* file, int line) {
  if (nullptr != g_hooks.on_panic) {
    g_hooks.on_panic(g_hooks.ctx, file, line);
  }
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
  if (!ok && (nullptr != g_hooks.on_error)) {
    g_hooks.on_error(g_hooks.ctx, code, where);
  }
  return ok;
}

// Target equivalent: called from MemManage_Handler / HardFault_Handler with
// CFSR and MMFAR already latched. The BSP resets after the hook returns.
void protection_fault(uint32_t cfsr, uint32_t mmfar) {
  if (nullptr != g_hooks.on_protection) {
    g_hooks.on_protection(g_hooks.ctx, cfsr, mmfar);
  }
}

}  // namespace bsp

void on_error(void* ctx, PalError code, const char* where) {
  auto* state = static_cast<HookState*>(ctx);
  ++state->errors;
  std::printf("[error] code=%d where=%s\n", static_cast<int>(code), where);
}

void on_protection(void* ctx, uint32_t cfsr, uint32_t mmfar) {
  auto* state = static_cast<HookState*>(ctx);
  ++state->protections;
  std::printf("[protection] cfsr=0x%08X mmfar=0x%08X\n", static_cast<unsigned>(cfsr), static_cast<unsigned>(mmfar));
}

void on_panic(void* /*ctx*/, const char* file, int32_t line) {
  const char marker = 'P';
  (void)file;
  (void)line;
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

  int pipefd[2] = {-1, -1};
  bool panic_ok = false;
  if (0 == ::pipe(pipefd)) {
    g_panic_pipe = pipefd[1];
    const pid_t pid = ::fork();
    if (0 == pid) {
      (void)::close(pipefd[0]);
      coact::fatal_assert(__FILE__, __LINE__);
    }
    (void)::close(pipefd[1]);
    if (pid > 0) {
      char marker = '\0';
      const ssize_t got = ::read(pipefd[0], &marker, 1U);
      (void)::close(pipefd[0]);
      int status = 0;
      (void)::waitpid(pid, &status, 0);
      panic_ok = (1 == got) && ('P' == marker) && WIFSIGNALED(status) && (SIGABRT == WTERMSIG(status));
    }
  }
  g_panic_pipe = -1;

  const bool errors_ok = first && !second && (1 == state.errors);
  const bool protections_ok = (1 == state.protections);
  std::printf("hooks: errors=%d protections=%d panic=%s\n", state.errors, state.protections,
              panic_ok ? "routed" : "missed");
  std::printf("RESULT %s\n", (errors_ok && protections_ok && panic_ok) ? "PASS" : "FAIL");
  return (errors_ok && protections_ok && panic_ok) ? 0 : 1;
}
