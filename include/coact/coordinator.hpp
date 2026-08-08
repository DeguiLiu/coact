// coact DispatchCoordinator - unified submit pipeline: M4 policy -> M1 C1-C7
// admission -> direct dispatch | staging. See design 8 and contract 4.8.
//
// Adapted from QP/C++ QF submit path (src/qf/qf_act.cpp); re-expressed
// against the coact API. Project license: MIT.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/event.hpp"
#include "coact/monitor.hpp"
#include "coact/policy.hpp"
#include "coact/pool.hpp"
#include "coact/staging.hpp"

namespace coact {

// ---------------------------------------------------------------------------
// DispatchCoordinator: the single entry point for all event submissions.
// Enforces the M4 -> M1 -> (direct | merge | staging) pipeline.
//
// Event reference-count contract (QF semantics):
//   submit_from_task / try_submit_from_isr own the reference passed in.
//   - Staged:       event_ref_inc called before enqueue; Dispatcher calls
//                   event_gc after dispatch.
//   - Direct:       event_ref_inc / dispatch / event_gc done inside submit.
//   - Drop / Merge: event_gc called immediately.
//   The caller MUST NOT access the event after submit returns.
// ---------------------------------------------------------------------------
template <typename StagingT, typename PalT>
class DispatchCoordinator
{
public:
    using RegistryT = AoRegistry<typename StagingT::ConfigType>;
    using MonitorT  = Monitor<typename StagingT::ConfigType>;
    using BreakerT  = Breaker<typename StagingT::ConfigType>;

    DispatchCoordinator(StagingT& staging, RegistryT& registry,
                        MonitorT& monitor, BreakerT& breaker, PalT& pal,
                        const PolicyOps* policy_ops = nullptr,
                        void* policy_ctx = nullptr) noexcept
        : staging_(staging),
          registry_(registry),
          monitor_(monitor),
          breaker_(breaker),
          pal_(pal),
          policy_ops_(policy_ops),
          policy_ctx_(policy_ctx)
    {
    }

    // Submit an event from a task context. Owns the reference on entry.
    SubmitResult submit_from_task(TargetId target, Event* e,
                                  const EventQos& qos) noexcept
    {
        return submit_internal(target, e, qos, /*from_isr=*/false);
    }

    // Submit an event from an ISR context. Must never block.
    SubmitResult try_submit_from_isr(TargetId target, Event* e,
                                     const EventQos& qos) noexcept
    {
        return submit_internal(target, e, qos, /*from_isr=*/true);
    }

private:
    SubmitResult submit_internal(TargetId target, Event* e,
                                 const EventQos& qos, bool from_isr) noexcept
    {
        /* Monotonic clock is sampled once per submit: the M4 policy and the
           Low-aging staging paths both need a timestamp, but the clock must not
           be read twice for the same instant (the #2 host hot spot in the flame
           profile). Cached here so the two call sites share one sample. */
        uint64_t now_ns = 0U;
        bool now_init = false;

        /* --- C1: target must be bound ------------------------------------ */
        AoBase* ao = registry_.lookup(target);
        if (nullptr == ao) {
            event_gc(e);
            return {SubmitDisposition::RejectedState, 0U};
        }

        /* --- M6 overload guard ------------------------------------------ */
        const BreakerLevel lvl = breaker_.level();
        if (BreakerLevel::BrokenL2 <= lvl) {
            if (!qos.critical) {
                monitor_.record_disposition(SubmitDisposition::DroppedOverload);
                event_gc(e);
                return {SubmitDisposition::DroppedOverload, 0U};
            }
        }

        /* --- M4 policy evaluation --------------------------------------- */
        if (policy_ops_ != nullptr) {
            if (!now_init) {
                now_ns = pal_.monotonic_ns();
                now_init = true;
            }
            PolicyResult pr = policy_ops_->evaluate(
                policy_ctx_, target, *e, qos, now_ns);
            if (!pr.accept) {
                const SubmitDisposition disp =
                    (0U != pr.reason)
                        ? SubmitDisposition::DroppedRateLimit
                        : SubmitDisposition::DroppedPolicy;
                monitor_.record_rejection(target, RejectReason::kC7Context);
                monitor_.record_disposition(disp);
                event_gc(e);
                return {disp, pr.reason};
            }
            /* Merge hint: not implemented in v1 (no per-signal MergeCell
               registry in coordinator); fall through to staging. */
        }

        /* --- M1 direct dispatch (Task path only) ------------------------ */
        /* A successful direct path consumes the event's single counted
           reference in-place. On a lost cross-thread race the direct attempt
           leaves that reference held and ownership passes to the staging path
           below, which must NOT re-increment. The event is handed by pointer
           (zero-copy); nothing is copied or reclaimed between paths. */
        bool direct_ref_held = false;
        if (!from_isr
            && ao->direct_eligible()
            && breaker_.direct_allowed(target))
        {
            /* dispatch_direct() owns the acquire internally with a RunningDirect
               state (distinct from RunningDispatcher, so C5 monitoring can tell
               the two paths apart). */
            if (AoRunState::Idle == ao->lease().state()) {
                event_ref_inc(e);
                direct_ref_held = true;
                pal_.enter_direct();
                const bool taken = ao->dispatch_direct(*e);
                pal_.leave_direct();
                if (taken) {
                    /* direct completed: the fetched reference was consumed */
                    event_gc(e);
                    direct_ref_held = false;
                    monitor_.record_disposition(SubmitDisposition::Direct);
                    return {SubmitDisposition::Direct, 0U};
                }
                /* Lost the direct race: keep the counted reference for the
                   staging path and fall through. Do NOT reclaim the block. */
                monitor_.record_lease_contention(target);
            }
            else {
                /* lease non-Idle: fall through to staging (no ref taken yet) */
                monitor_.record_lease_contention(target);
            }
        }

        /* --- staging (queued) ------------------------------------------- */
        if (!now_init) {
            now_ns = pal_.monotonic_ns();
            now_init = true;
        }
        if (!direct_ref_held) {
            event_ref_inc(e);
        }
        ao->pending().increment();
        if (!staging_.enqueue(target, e, ao->priority_class(), now_ns)) {
            ao->pending().decrement();
            /* The event is dropped: reclaim it whether its counted ref was
               taken by the direct attempt (direct_ref_held) or just now. */
            event_gc(e);
            monitor_.record_overflow();
            monitor_.record_disposition(SubmitDisposition::RejectedFull);
            return {SubmitDisposition::RejectedFull, 0U};
        }

        /* Wake the Dispatcher only while it is idle. While it is actively
           draining it picks this event up in the same batch; the draining flag
           plus the Dispatcher's re-check-before-sleep close the missed-wakeup
           window. Removes the per-submit condvar/semaphore signal that was the
           #1 single-core hot cost in the flame profile. */
        if (!staging_.dispatcher_active()) {
            if (!from_isr) {
                pal_.signal_dispatcher_from_task();
            }
            else {
                pal_.signal_dispatcher_from_isr();
            }
        }
        monitor_.record_disposition(SubmitDisposition::Queued);
        monitor_.record_pending(target, ao->pending().load());
        return {SubmitDisposition::Queued, 0U};
    }

    StagingT&          staging_;
    RegistryT&         registry_;
    MonitorT&          monitor_;
    BreakerT&          breaker_;
    PalT&              pal_;
    const PolicyOps*   policy_ops_;
    void*              policy_ctx_;
};

}  // namespace coact
