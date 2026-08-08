// coact Platform Abstraction Layer contract.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "coact/config.hpp"

namespace coact {
namespace pal {

struct CriticalToken {
    uintptr_t value;
};

typedef void (*ThreadEntry)(void* context);

// Concrete PAL types (Posix, RtThread) provide these members; they are not
// required to inherit from any base (concept-checked via the Runtime template).
// Per design 14.2, callback function pointers carry no noexcept qualifier.
//
//   CriticalToken irq_save() noexcept;
//   void irq_restore(CriticalToken token) noexcept;
//   ExecutionContext current_context() const noexcept;
//   uint64_t monotonic_ns() const noexcept;
//   uint64_t clock_resolution_ns() const noexcept;
//   void wait_dispatcher(uint32_t timeout_ms) noexcept;
//   void signal_dispatcher_from_task() noexcept;
//   void signal_dispatcher_from_isr() noexcept;
//   void start_dispatcher(ThreadEntry entry, void* context) noexcept;
//   void join_dispatcher() noexcept;
//   void watchdog_progress(uint32_t marker) noexcept;

}  // namespace pal
}  // namespace coact
