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
//   - Staged:       allocation reference transfers to staging; Dispatcher
//                   calls event_gc after dispatch.
//   - Direct:       allocation reference is consumed after dispatch.
//   - Drop / Merge: allocation reference is consumed immediately.
//   The caller MUST NOT access the event after submit returns.
// ---------------------------------------------------------------------------
template <typename StagingT, typename PalT,
          typename BreakerRouterT = Breaker<typename StagingT::ConfigType>>
class DispatchCoordinator
{
public:
    using RegistryT = AoRegistry<typename StagingT::ConfigType>;
    using MonitorT  = Monitor<typename StagingT::ConfigType>;

    DispatchCoordinator(StagingT& staging, RegistryT& registry,
                        MonitorT& monitor, BreakerRouterT& breaker, PalT& pal,
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
    class SubmissionLease {
    public:
        SubmissionLease(StagingT& staging, PalT& pal, bool from_isr) noexcept
            : staging_(staging.acquire_submission() ? &staging : nullptr),
              pal_(pal),
              from_isr_(from_isr)
        {
        }

        ~SubmissionLease()
        {
            if ((nullptr != staging_) && staging_->release_submission()) {
                if (from_isr_) {
                    pal_.signal_dispatcher_from_isr();
                }
                else {
                    pal_.signal_dispatcher_from_task();
                }
            }
        }

        bool admitted() const noexcept
        {
            return nullptr != staging_;
        }

    private:
        StagingT* staging_;
        PalT& pal_;
        bool from_isr_;
    };

    SubmitResult submit_internal(TargetId target, Event* e,
                                 const EventQos& qos, bool from_isr) noexcept
    {
        /* Monotonic clock is sampled only when the M4 policy or Low-aging
           staging path needs it. Cached here so those two paths share one
           sample; High/Normal staged events carry an unused zero timestamp. */
        uint64_t now_ns = 0U;
        bool now_init = false;

        /* --- C1: target must be bound ------------------------------------ */
        AoBase* ao = registry_.lookup(target);
        if (nullptr == ao) {
            event_gc(e);
            return {SubmitDisposition::RejectedState, 0U};
        }

        SubmissionLease lease(staging_, pal_, from_isr);
        if (!lease.admitted()) {
            event_gc(e);
            monitor_.record_disposition(SubmitDisposition::RejectedState);
            return {SubmitDisposition::RejectedState, 0U};
        }

        /* --- M6 overload guard ------------------------------------------ */
        Breaker<typename StagingT::ConfigType>& target_breaker =
            detail::target_breaker(breaker_, target);
        const BreakerLevel lvl = target_breaker.level();
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
        /* The allocation reference transfers directly to the successful
           dispatch or, after a lost race, to the staging path below. */
        if (!from_isr
            && ao->direct_eligible()
            && target_breaker.direct_allowed(target))
        {
            /* dispatch_direct() owns the acquire internally with a RunningDirect
               state (distinct from RunningDispatcher, so C5 monitoring can tell
               the two paths apart). */
            if (AoRunState::Idle == ao->lease().state()) {
                pal_.enter_direct();
                const uint64_t t0 = pal_.monotonic_ns();
                const bool taken = ao->dispatch_direct(*e);
                const uint64_t elapsed = pal_.monotonic_ns() - t0;
                pal_.leave_direct();
                if (taken) {
                    if (elapsed > ao->rtc_budget_ns()) {
                        target_breaker.on_direct_timeout();
                    } else {
                        target_breaker.on_rtc_ok();
                    }
                    /* A queued event may already be retained by the Dispatcher
                       after losing this AO's lease. Publish the wake only after
                       dispatch_direct() released RunningDirect. The pending
                       acquire load plus the Dispatcher's arm-then-CAS retry
                       closes the release-before-wait window without polling. */
                    if ((ao->pending().load() > 0U)
                        && staging_.request_dispatcher_wake()) {
                        pal_.signal_dispatcher_from_task();
                    }
                    /* direct completed: consume the allocation reference */
                    event_gc(e);
                    monitor_.record_disposition(SubmitDisposition::Direct);
                    return {SubmitDisposition::Direct, 0U};
                }
                /* Lost the direct race: transfer the allocation reference to
                   staging below. Do NOT reclaim the block. */
                monitor_.record_lease_contention(target);
            }
            else {
                /* lease non-Idle: fall through to staging (no ref taken yet) */
                monitor_.record_lease_contention(target);
            }
        }

        /* --- staging (queued) ------------------------------------------- */
        const PriorityClass priority_class = ao->priority_class();
        if (!now_init && PriorityClass::Low == priority_class) {
            now_ns = pal_.monotonic_ns();
            now_init = true;
        }
        PendingCounter& pending = ao->pending();
        pending.increment();
        if (!staging_.enqueue(target, e, priority_class, now_ns)) {
            pending.decrement();
            /* The event is dropped: consume the allocation reference. */
            event_gc(e);
            monitor_.record_overflow();
            target_breaker.on_overflow();
            monitor_.record_disposition(SubmitDisposition::RejectedFull);
            return {SubmitDisposition::RejectedFull, 0U};
        }

        /* Coalesce wakeups behind one latch. Queue publication happens before
           this acq_rel exchange; once the Dispatcher arms its wait, exactly the
           first producer owns the PAL signal. */
        if (staging_.request_dispatcher_wake()) {
            if (!from_isr) {
                pal_.signal_dispatcher_from_task();
            }
            else {
                pal_.signal_dispatcher_from_isr();
            }
        }
        monitor_.record_disposition(SubmitDisposition::Queued);
        monitor_.record_pending(target, pending.load());
        return {SubmitDisposition::Queued, 0U};
    }

    StagingT&          staging_;
    RegistryT&         registry_;
    MonitorT&          monitor_;
    BreakerRouterT&    breaker_;
    PalT&              pal_;
    const PolicyOps*   policy_ops_;
    void*              policy_ctx_;
};

}  // namespace coact
