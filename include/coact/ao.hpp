// coact Active Object: execution lease, pending counter, AO base interface,
// the Ao<Context, Hsm, Traits> RTC template and the fixed AoRegistry.
// See design 5 and implementation contract 4.6.
//
// Model adapted from QP/C++ (Quantum Leaps) QActive / execution model
// (src/qf/qf_act.cpp); the coact API is an original, minimal re-expression
// of the single-execution active object. Project license: MIT.
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>

#include "coact/assert.hpp"
#include "coact/config.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"

namespace coact {

// ---------------------------------------------------------------------------
// Execution context of a single AO RTC. Direct (M1) and Dispatcher (M5) both
// take the same lease; only the state differs so monitoring can tell paths.
// ---------------------------------------------------------------------------
enum class AoRunState : uint8_t {
    Idle,
    RunningDirect,
    RunningDispatcher
};

// ---------------------------------------------------------------------------
// Single-execution lease. The linearization point is the CAS that moves the
// state from Idle to the desired running state. A failed acquire never spins:
// M1 falls back to staging; the Dispatcher keeps the event in its batch.
// ---------------------------------------------------------------------------
class ExecutionLease {
public:
    ExecutionLease() noexcept : state_(AoRunState::Idle) {}

    // Atomically transition Idle -> desired. Returns false when the AO is
    // already running (or when asked to acquire into Idle, which is invalid).
    bool try_acquire(AoRunState desired) noexcept
    {
        if (AoRunState::Idle == desired) {
            return false;  // cannot acquire a lease into Idle
        }
        AoRunState expected = AoRunState::Idle;
        return state_.compare_exchange_strong(
            expected, desired,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    // Release a lease previously acquired with a matching running state.
    // Supplying the wrong expected state is a protocol violation (hard fault).
    void release(AoRunState expected) noexcept
    {
        AoRunState confirmed = expected;
        COACT_ASSERT(state_.compare_exchange_strong(
            confirmed, AoRunState::Idle,
            std::memory_order_acq_rel, std::memory_order_acquire));
    }

    AoRunState state() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }

private:
    std::atomic<AoRunState> state_;
};

// ---------------------------------------------------------------------------
// Outstanding-pending counter. increment() (release) must be published before
// the queue slot becomes visible; load() (acquire) is the C4/C6 read so no
// producer ever observes "slot visible yet pending == 0" (design 5.2/9.3).
// ---------------------------------------------------------------------------
class PendingCounter {
public:
    PendingCounter() noexcept : count_(0U) {}

    uint16_t load() const noexcept
    {
        return count_.load(std::memory_order_acquire);
    }

    void increment() noexcept
    {
        count_.fetch_add(1U, std::memory_order_release);
    }

    void decrement() noexcept
    {
        const uint16_t prior = count_.load(std::memory_order_relaxed);
        COACT_ASSERT(prior > 0U);  // underflow on an empty counter is a fault
        count_.fetch_sub(1U, std::memory_order_acq_rel);
    }

private:
    std::atomic<uint16_t> count_;
};

// ---------------------------------------------------------------------------
// Type-erased Active Object base. One vtable is the only virtual overhead;
// the hot dispatch/dequeue path never relies on RTTI.
// ---------------------------------------------------------------------------
class AoBase {
public:
    virtual ~AoBase() {}

    // Dispatcher-path RTC dispatch (M5): acquires RunningDispatcher lease
    // internally, runs the HSM to completion, releases. Re-entry is a fault.
    virtual void dispatch(const Event& event) noexcept = 0;

    // Direct-path RTC dispatch (M1): acquires RunningDirect lease internally,
    // runs the HSM to completion, releases. Kept distinct so Monitor/C5
    // can distinguish a direct dispatch from a dispatcher-driven one via
    // ExecutionLease::state() (RunningDirect vs RunningDispatcher).
    virtual void dispatch_direct(const Event& event) noexcept = 0;

    virtual LogicalPrio logical_prio() const noexcept = 0;
    virtual PriorityClass priority_class() const noexcept = 0;
    virtual bool direct_eligible() const noexcept = 0;
    virtual bool isr_direct_safe() const noexcept = 0;
    virtual ExecutionLease& lease() noexcept = 0;
    virtual PendingCounter& pending() noexcept = 0;
};

// ---------------------------------------------------------------------------
// Fixed active-object registry. TargetId is 1-based and indexes directly into
// a fixed AoBase* array (design 5.6: fixed table gives O(1) lookup for the
// C1-C7 hot path). bind validates priority uniqueness; priorities are the
// registry's own bookkeeping (kMaxPrio is far larger than kMaxAo so stealing
// a slot never needs to reuse a freed priority in steady state).
// ---------------------------------------------------------------------------
class AoRegistry {
public:
    static constexpr uint8_t kCapacity = DefaultConfig::kMaxAo;

    AoRegistry() noexcept
    {
        for (uint8_t i = 0U; i < kCapacity; ++i) {
            slots_[i] = nullptr;
        }
    }

    // 1-based TargetId lookup; returns nullptr for kInvalidTarget, for ids
    // past capacity, and for never-bound slots.
    AoBase* lookup(TargetId target) const noexcept
    {
        if (target == kInvalidTarget || target > kCapacity) {
            return nullptr;
        }
        return slots_[target - 1U];
    }

    // Bind an AO at the lowest free slot. Rejects null AOs, invalid/dummy
    // priorities and duplicate priorities (re-using a priority would let two
    // AOs claim the same scheduler priority). Returns true on success.
    bool bind(AoBase* ao, LogicalPrio prio) noexcept
    {
        if (ao == nullptr || prio == kInvalidPrio) {
            return false;
        }
        for (uint8_t i = 0U; i < kCapacity; ++i) {
            if (slots_[i] != nullptr && slots_[i]->logical_prio() == prio) {
                return false;  // duplicate priority already bound
            }
        }
        for (uint8_t i = 0U; i < kCapacity; ++i) {
            if (slots_[i] == nullptr) {
                slots_[i] = ao;
                return true;
            }
        }
        return false;  // registry full
    }

    // Reverse lookup: the 1-based TargetId of a bound AO, or kInvalidTarget.
    TargetId target_of(const AoBase* ao) const noexcept
    {
        if (ao == nullptr) {
            return kInvalidTarget;
        }
        for (uint8_t i = 0U; i < kCapacity; ++i) {
            if (slots_[i] == ao) {
                return static_cast<TargetId>(i + 1U);
            }
        }
        return kInvalidTarget;
    }

private:
    AoBase* slots_[kCapacity];
};

// ---------------------------------------------------------------------------
// Concrete Active Object. HsmT is left as a template parameter (usually
// coact::Hsm<Context>) so "coact::Hsm" the class does not shadow the same
// name when referenced inside the template. Traits statically inject the AO
// properties; nothing is stored for them, so an Ao costs only Context + Hsm
// + lease + pending and never allocates.
//
// dispatch() is the RTC unit of single execution: it takes the execution
// lease, runs the HSM to completion synchronously, then releases the lease.
// Re-entering dispatch while the AO is already running (e.g. an action that
// submits back to the same AO and drives it in-place) is a hard protocol
// violation. event_gc is the Dispatcher layer's responsibility, not ours.
// ---------------------------------------------------------------------------
template <typename Context, typename HsmT, typename Traits>
class Ao : public AoBase {
public:
    Ao(const StateDef<Context>* states, uint16_t num_states,
       const TransitionDef<Context>* transitions, uint16_t num_transitions,
       int8_t initial_state, uint8_t max_depth) noexcept
        : context_{},
          hsm_(states, num_states, transitions, num_transitions,
               initial_state, max_depth)
    {
    }

    // Enter the initial state. Called by the runtime during initialization,
    // before the AO accepts any dispatched event.
    void init(const Event& init_evt) noexcept
    {
        hsm_.init(context_, init_evt);
    }

    // Application context accessor. Callers configure the AO's context
    // (payload, flags, whatever the HSM actions need) before init().
    Context& context() noexcept { return context_; }

    void dispatch(const Event& event) noexcept override
    {
        const AoRunState held = AoRunState::RunningDispatcher;
        if (lease_.try_acquire(held)) {
            hsm_.dispatch(context_, event);
            lease_.release(held);
        }
        else {
            // Re-entry while the AO is already running: upper-layer violation.
            COACT_ASSERT(0 == 1);
        }
    }

    void dispatch_direct(const Event& event) noexcept override
    {
        const AoRunState held = AoRunState::RunningDirect;
        if (lease_.try_acquire(held)) {
            hsm_.dispatch(context_, event);
            lease_.release(held);
        }
        else {
            // Direct dispatch while the AO is already running: the caller
            // (coordinator) checked lease Idle before invoking, so this is a
            // protocol violation.
            COACT_ASSERT(0 == 1);
        }
    }

    LogicalPrio logical_prio() const noexcept override
    {
        return Traits::logical_prio();
    }

    PriorityClass priority_class() const noexcept override
    {
        return Traits::priority_class();
    }

    bool direct_eligible() const noexcept override
    {
        return Traits::direct_eligible();
    }

    bool isr_direct_safe() const noexcept override
    {
        return Traits::isr_direct_safe();
    }

    ExecutionLease& lease() noexcept override { return lease_; }
    PendingCounter& pending() noexcept override { return pending_; }

    // Compile-time RTC budget injected by Traits (monitor reads it for the
    // Dispatcher timeout path).
    static constexpr uint64_t rtc_budget_ns() noexcept
    {
        return Traits::kRtcBudgetNs;
    }

private:
    Context context_;
    HsmT hsm_;
    ExecutionLease lease_;
    PendingCounter pending_;
};

}  // namespace coact
