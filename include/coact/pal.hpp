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
    using Token = uintptr_t;
    void* ctx;                             // opaque PAL context (passed to hooks)
    Token (*save)(void* ctx);
    void (*restore)(void* ctx, Token);
};

// Unified RAII guard for a CriticalSection: save() on construction (a null
// save hook degrades to no-op), restore() on destruction (a null restore hook
// is a no-op). Factored into one canonical place so consumers no longer each
// hand-roll their own `CriticalSectionScope` (previously duplicated across the
// cmdfw delivery_runtime and response paths with inconsistent null-tolerance).
// Value-initialized token field makes a null save leave the token in a safe
// state. Trivial inline, zero heap, no exceptions.
class CriticalSectionGuard {
public:
    explicit CriticalSectionGuard(const CriticalSection& cs) noexcept
        : cs_(cs),
          token_((cs_.save != nullptr) ? cs_.save(cs_.ctx)
                                       : CriticalSection::Token{})
    {
    }
    CriticalSectionGuard(const CriticalSectionGuard&) = delete;
    CriticalSectionGuard& operator=(const CriticalSectionGuard&) = delete;
    ~CriticalSectionGuard() noexcept
    {
        if (cs_.restore != nullptr) {
            cs_.restore(cs_.ctx, token_);
        }
    }

private:
    CriticalSection cs_;
    CriticalSection::Token token_;
};

namespace pal {
struct CriticalToken {
    uintptr_t value;
};

// High-resolution monotonic counter injected into a PAL as a static function
// table (design §7.5): no inheritance / virtual dispatch. read_counter()
// returns raw counter ticks; frequency_hz converts them to nanoseconds.
// counter_bits declares a wrapping hardware width (64 means already extended).
// RT tick remains the source for Dispatcher blocking waits and long deadlines.
struct ClockOps {
    uint64_t (*read_counter)(void* ctx);
    uint32_t frequency_hz;
    void* ctx;
    uint8_t counter_bits = 64U;
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

// A CriticalSection that actually serializes on SMP hosts. The POSIX PAL's
// irq_save() is a documented no-op (see design 13) — its pools/queues rely on
// the 32-bit head CAS alone. That is safe for the free-list HEAD, but the
// batched reclaim (ReclaimBatcher) writes each block's `next` field OUTSIDE
// the head CAS while chaining blocks, which races a concurrent alloc's
// load_next on the same block. For any pool shared between an allocating
// thread and a reclaiming thread on SMP, inject THIS critical section instead
// of make_critical_section(pal): a short per-pool spinlock held across the
// alloc / reclaim / batch-splice operations so their head + next-field writes
// serialize. Single-core targets keep make_critical_section (irq mask), which
// is O(1) and unaffected by the race (alloc and reclaim are never concurrent
// on one core).
struct SpinCriticalSection {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
};
inline CriticalSection make_spin_critical_section(SpinCriticalSection& scs) noexcept
{
    CriticalSection cs;
    cs.ctx = static_cast<void*>(&scs);
    cs.save = [](void* ctx) -> CriticalSection::Token {
        auto* s = static_cast<SpinCriticalSection*>(ctx);
        // Acquire: spin until the flag was clear. Token 1 marks "held".
        while (s->flag.test_and_set(std::memory_order_acquire)) {
        }
        return 1U;
    };
    cs.restore = [](void* ctx, CriticalSection::Token) {
        auto* s = static_cast<SpinCriticalSection*>(ctx);
        s->flag.clear(std::memory_order_release);
    };
    return cs;
}

namespace pal {

using ThreadEntry = void (*)(void* context);

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
//       // RtThread additionally returns pal::InitError (design §7.5): kOk only
//       // after rt_thread_startup()==RT_EOK; kAlreadyStarted after a second
//       // start or after stop. Posix keeps void.
//   void join_dispatcher() noexcept;
//   void watchdog_progress(uint32_t marker) noexcept;
//   void set_dispatcher_stack_bytes(uint32_t bytes) noexcept;   // may be no-op
//   void set_clock_ops(ClockOps ops) noexcept;                  // optional (§7.5)

}  // namespace pal
}  // namespace coact
