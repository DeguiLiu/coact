/*
 * Bare-metal PAL for the coact Renode harness.
 *
 * Time is a plain counter owned by the test program: every scenario sets it
 * explicitly, so admission decisions are deterministic and never sleep.
 * Dispatcher wakeups and direct-path markers are no-ops.
 */
#pragma once

#include <cstdint>

#include "coact/pal.hpp"

namespace harness {

inline uint64_t& virtual_clock_ns() noexcept
{
    static uint64_t now = 0U;
    return now;
}

inline void set_time_ns(uint64_t now) noexcept
{
    virtual_clock_ns() = now;
}

/* Used by the interrupt-driven scenario: the handler advances the clock by one
 * configured step per tick, so admission decisions stay deterministic even
 * though the interrupt itself arrives asynchronously. */
inline void advance_time_ns(uint64_t step) noexcept
{
    virtual_clock_ns() += step;
}

struct BarePal {
    int task_signals = 0;
    int isr_signals = 0;

    uint64_t monotonic_ns() noexcept { return virtual_clock_ns(); }

    /* Real single-core critical section: PRIMASK save and restore. A policy
     * bound with AdmissionIrqSync installs these through
     * make_critical_section(), so the arrival gate runs under the same
     * interrupt mask the staging ring and the event pool already use and the
     * submit path needs no 64-bit atomic backend. */
    coact::pal::CriticalToken irq_save() noexcept
    {
        uint32_t primask = 0U;
        __asm__ volatile("mrs %0, primask\n\tcpsid i"
                         : "=r"(primask) : : "memory");
        return coact::pal::CriticalToken{static_cast<uintptr_t>(primask)};
    }

    void irq_restore(coact::pal::CriticalToken token) noexcept
    {
        __asm__ volatile("msr primask, %0"
                         : : "r"(static_cast<uint32_t>(token.value))
                         : "memory");
    }

    void signal_dispatcher_from_task() noexcept { ++task_signals; }
    void signal_dispatcher_from_isr() noexcept { ++isr_signals; }
    void enter_direct() noexcept {}
    void leave_direct() noexcept {}
};

}  // namespace harness
