// coact Platform Abstraction Layer contract.
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>

#include "coact/config.hpp"

namespace coact {

// ---------------------------------------------------------------------------
// CriticalSection: platform interrupt-critical-section hook injected into the
// single-core pool / queue backends. save() masks interrupts and returns an
// opaque token; restore(token) unmasks them again. Host tests inject no-op
// hooks; RT-Thread 5.2.x maps save/restore to rt_hw_interrupt_disable/enable so
// a single-core 100 MHz MCU guards the pool head RMW in O(1), no libatomic.
// Per contract 4.3 the function pointers carry no noexcept qualifier.
// ---------------------------------------------------------------------------
struct CriticalSection {
    typedef uintptr_t Token;
    void* ctx;                             // opaque PAL context (passed to hooks)
    Token (*save)(void* ctx);
    void (*restore)(void* ctx, Token);
};

namespace pal {
struct CriticalToken {
    uintptr_t value;
};
}  // namespace pal

// Build a CriticalSection from any PAL that exposes irq_save()/irq_restore()
// (e.g. pal::RtThread -> rt_hw_interrupt_disable/enable on RT-Thread 5.2.x,
// pal::Posix -> no-op on host). The PAL pointer travels as the CS ctx (the
// hooks are capture-less, so they convert to plain function pointers).
// Pass the result to EventPool::init and the SingleCoreCriticalRing staging
// backend so single-core targets guard the pool / queue head RMW in O(1)
// without libatomic.
template <typename PalT>
inline CriticalSection make_critical_section(PalT& pal) noexcept
{
    CriticalSection cs;
    cs.ctx     = static_cast<void*>(&pal);
    cs.save    = [](void* ctx) -> CriticalSection::Token {
        return static_cast<PalT*>(ctx)->irq_save().value;
    };
    cs.restore = [](void* ctx, CriticalSection::Token v) {
        pal::CriticalToken tok;
        tok.value = v;
        static_cast<PalT*>(ctx)->irq_restore(tok);
    };
    return cs;
}

namespace pal {

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
//   void set_dispatcher_stack_bytes(uint32_t bytes) noexcept;   // may be no-op

}  // namespace pal
}  // namespace coact
