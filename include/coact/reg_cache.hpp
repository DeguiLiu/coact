// coact PeriphRegCache: shadow/hardware register pair with an explicit
// three-flag write protocol (write-through / cache_only / cache_bypass) and
// a paired RAII bypass guard. Header-only, allocation-free.
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>

#include "coact/bitfield.hpp"

namespace coact {

// Number of 32-bit registers the cache mirrors.
inline constexpr std::uint16_t kPeriphRegCount = 8U;

// The two copies of the truth: the software shadow (authoritative) and the
// simulated hardware registers.
struct PeriphState {
    std::array<std::uint32_t, kPeriphRegCount> shadow{};
    std::array<std::uint32_t, kPeriphRegCount> hardware{};
};

// Shadow/hardware register cache. Every write goes through one of the two
// entry points (write for a whole word, write_field for one bit-field), so
// the three-flag protocol is enforced in one place rather than at each call
// site.
struct PeriphRegCache {
    PeriphState st{};
    bool cache_only{false};
    bool cache_bypass{false};
    bool cache_dirty{false};
    std::array<bool, kPeriphRegCount> dirty_regs{};

    std::uint32_t bypass_writes{0U};    // stats: bare vs guarded
    std::uint32_t drift_events{0U};     // auditor findings
    std::uint32_t syncs{0U};

    // Runtime first-line guard: mode combinations that make no sense are
    // rejected at the setter, not in code review.
    [[nodiscard]] bool modes_legal() const noexcept
    {
        return !(cache_only && cache_bypass);
    }

    // Write-through single entry: both copies update in one call.
    void write(std::uint16_t reg, std::uint32_t val) noexcept
    {
        if (cache_bypass) {
            // Controlled bypass: hardware only; resync on guard exit.
            st.hardware[reg] = val;
            ++bypass_writes;
            return;
        }
        if (cache_only) {
            // Device unwritable (freeze window): record in the shadow, mark
            // dirty; sync commits later at the frame boundary.
            st.shadow[reg] = val;
            cache_dirty = true;
            dirty_regs[reg] = true;
            return;
        }
        // Default write-through: hardware first, shadow only on success.
        st.hardware[reg] = val;
        st.shadow[reg] = val;
    }

    // Bit-field entry: the SAME three-flag protocol as write(), but the
    // value lands through a BitFieldView RMW — only the field's bits move;
    // adjacent fields in the same word are NEVER touched. Use this to change
    // one parameter that shares a word with others: the scalar write() lands
    // the whole 32-bit value and would clobber its neighbors.
    template <typename Field>
    void write_field(std::uint16_t reg, std::uint32_t val) noexcept
    {
        if (cache_bypass) {
            Field::write(st.hardware[reg], val);
            ++bypass_writes;
            return;
        }
        if (cache_only) {
            Field::write(st.shadow[reg], val);
            cache_dirty = true;
            dirty_regs[reg] = true;
            return;
        }
        Field::write(st.hardware[reg], val);
        Field::write(st.shadow[reg], val);
    }

    // Field read from the shadow copy (the authority).
    template <typename Field>
    [[nodiscard]] std::uint32_t read_field(std::uint16_t reg) const noexcept
    {
        return Field::read(st.shadow[reg]);
    }

    // Full parameter re-push: replays the shadow into hardware. If the shadow
    // is stale (a bare bypass forked the copies), this silently overwrites
    // the tuned hardware values.
    void full_repush() noexcept
    {
        for (std::uint16_t r = 0; r < kPeriphRegCount; ++r) {
            st.hardware[r] = st.shadow[r];
        }
    }

    // Pull the CURRENT hardware values back into the shadow (the cache_bypass
    // exit companion). After this the two copies agree again.
    void resync_from_hw() noexcept
    {
        st.shadow = st.hardware;
    }

    // Convergence point: write all dirty shadow values into hardware.
    // Refuses while still in cache_only (in a not-truly-written state the
    // commit is not allowed).
    [[nodiscard]] bool sync() noexcept
    {
        if (cache_only) { return false; }
        for (std::uint16_t r = 0; r < kPeriphRegCount; ++r) {
            if (dirty_regs[r]) {
                st.hardware[r] = st.shadow[r];
                dirty_regs[r] = false;
            }
        }
        cache_dirty = false;
        ++syncs;
        return true;
    }

    // Drift auditor: warn-only, never repair — surfaces delayed drift at the
    // moment it happens.
    void audit(const char* when) noexcept
    {
        for (std::uint16_t r = 0; r < kPeriphRegCount; ++r) {
            if (st.shadow[r] != st.hardware[r]) {
                ++drift_events;
                std::printf("[audit] %s: reg %u drift cache=0x%x hw=0x%x\n",
                            when, static_cast<unsigned>(r),
                            st.shadow[r], st.hardware[r]);
            }
        }
    }
};

// RAII paired-bypass guard: entering flips cache_bypass; ANY exit path
// resyncs the shadow from hardware. The pairing is structural — an early
// return cannot skip the resync.
class BypassGuard {
public:
    explicit BypassGuard(PeriphRegCache& c) noexcept : c_(c)
    {
        c_.cache_bypass = true;
    }
    ~BypassGuard()
    {
        c_.cache_bypass = false;
        c_.resync_from_hw();   // exit = align, always
    }
    BypassGuard(const BypassGuard&) = delete;
    BypassGuard& operator=(const BypassGuard&) = delete;
    BypassGuard(BypassGuard&&) = delete;
    BypassGuard& operator=(BypassGuard&&) = delete;

private:
    PeriphRegCache& c_;
};

}  // namespace coact
