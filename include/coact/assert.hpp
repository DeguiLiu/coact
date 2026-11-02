// coact assertion support.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdlib>

namespace coact {

using FatalHandler = void (*)(const char* file, int line);

namespace detail {

// Set during initialization; concurrent replacement is not supported.
inline FatalHandler& fatal_handler_slot() noexcept {
  static FatalHandler handler = nullptr;
  return handler;
}

}  // namespace detail

// Install a product fatal handler, e.g. one that drives outputs to a safe
// state or resets the controller. Returns the previous handler. If the
// installed handler returns, fatal_assert() still aborts: a fatal invariant
// break must never silently resume normal execution.
inline FatalHandler set_fatal_handler(FatalHandler handler) noexcept {
  FatalHandler previous = detail::fatal_handler_slot();
  detail::fatal_handler_slot() = handler;
  return previous;
}

// Invoked on an irrecoverable invariant break. Default aborts. The handler may
// reset or enter a safe state; if it returns, abort() still terminates.
[[noreturn]] inline void fatal_assert(const char* file, int line) noexcept {
  FatalHandler handler = detail::fatal_handler_slot();
  if (nullptr != handler) {
    handler(file, line);
  }
  std::abort();
}

}  // namespace coact

#define COACT_ASSERT(cond)                       \
  do {                                           \
    if (!(cond)) {                               \
      ::coact::fatal_assert(__FILE__, __LINE__); \
    }                                            \
  } while (0)

// Static branch hints for the hot path (event_gc, pool alloc, batch select).
// __builtin_expect is a GCC/Clang extension (host Linux, Xuantie RISC-V GCC,
// and MSVC's clang-cl); plain MSVC has no equivalent, so the expression is
// used unchanged there. Hint only the branches whose direction is stable under
// load - never sprinkle.
#if defined(__clang__) || defined(__GNUC__)
#define COACT_LIKELY(x) __builtin_expect(!!(x), 1)
#define COACT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define COACT_LIKELY(x) (x)
#define COACT_UNLIKELY(x) (x)
#endif
