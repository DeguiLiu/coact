// coact DispatchCoordinator - unified submit pipeline: M4 policy -> M1 C1-C7
// admission -> direct dispatch | staging. See design 8 and contract 4.8.
//
// The submit path follows the same idea as the QF post path in QP/C++;
// the implementation here is independent. Project license: MIT.
// SPDX-License-Identifier: MIT
#pragma once

#include "coact/admission.hpp"
#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/event.hpp"
#include "coact/monitor.hpp"
#include "coact/policy.hpp"
#include "coact/pool.hpp"
#include "coact/staging.hpp"

#include <cstdint>

namespace coact {

// ---------------------------------------------------------------------------
// DispatchCoordinator: the single entry point for all event submissions.
// Enforces the M4 -> M1 -> (direct | merge | staging) pipeline.
//
// AdmissionT selects the admission policy: NoAdmission (the default) compiles
// the arrival interval and the backlog cap out of the submit path entirely;
// StaticAdmission<RuleSet, Config, Sync> enables both from a compile-time
// rule set; Sync is the required critical-section backend (design 4.1).
//
// Event reference-count contract (reference-counting semantics):
//   submit_from_task / try_submit_from_isr own the reference passed in.
//   - Staged:       allocation reference transfers to staging; Dispatcher
//                   calls event_gc after dispatch.
//   - Direct:       allocation reference is consumed after dispatch.
//   - Drop / Merge: allocation reference is consumed immediately.
//   The caller MUST NOT access the event after submit returns.
// ---------------------------------------------------------------------------
template <typename StagingT, typename PalT, typename BreakerRouterT = Breaker<typename StagingT::ConfigType>,
          typename AdmissionT = NoAdmission>
class DispatchCoordinator {
 public:
  using RegistryT = AoRegistry<typename StagingT::ConfigType>;
  using MonitorT = Monitor<typename StagingT::ConfigType>;

  DispatchCoordinator(StagingT& staging, RegistryT& registry, MonitorT& monitor, BreakerRouterT& breaker, PalT& pal,
                      const PolicyOps* policy_ops = nullptr, void* policy_ctx = nullptr) noexcept
      : staging_(staging),
        registry_(registry),
        monitor_(monitor),
        breaker_(breaker),
        pal_(pal),
        policy_ops_(policy_ops),
        policy_ctx_(policy_ctx),
        admission_(pal) {}

  /* Copying would give two coordinators separate admission gates over the
     same staging queue, so the interval rule could be bypassed by
     alternating submit calls between the copies. */
  DispatchCoordinator(const DispatchCoordinator&) = delete;
  DispatchCoordinator& operator=(const DispatchCoordinator&) = delete;

  // Submit an event from a task context. Owns the reference on entry.
  // admission selects the capacity lane; the default keeps every existing
  // caller on the historical three-partition behavior.
  SubmitResult submit_from_task(TargetId target, Event* e, const EventQos& qos,
                                StagingAdmission admission = StagingAdmission::Ordinary) noexcept {
    return submit_internal(target, e, qos, admission, /*from_isr=*/false);
  }

  // Submit an event from an ISR context. Must never block.
  SubmitResult try_submit_from_isr(TargetId target, Event* e, const EventQos& qos,
                                   StagingAdmission admission = StagingAdmission::Ordinary) noexcept {
    return submit_internal(target, e, qos, admission, /*from_isr=*/true);
  }

  // Diagnostic read access to the admission policy, e.g. the per-target
  // ArrivalGate state. Read-only: limits are compile-time constants and the
  // gates are mutated solely by the submit path. Named apart from the
  // StagingAdmission parameter `admission` used on the submit entry points.
  const AdmissionT& admission_policy() const noexcept { return admission_; }

 private:
  class SubmissionLease {
   public:
    SubmissionLease(StagingT& staging, PalT& pal, bool from_isr) noexcept
        : staging_(staging.acquire_submission() ? &staging : nullptr), pal_(pal), from_isr_(from_isr) {}

    ~SubmissionLease() {
      if ((nullptr != staging_) && staging_->release_submission()) {
        if (from_isr_) {
          pal_.signal_dispatcher_from_isr();
        } else {
          pal_.signal_dispatcher_from_task();
        }
      }
    }

    bool admitted() const noexcept { return nullptr != staging_; }

   private:
    StagingT* staging_;
    PalT& pal_;
    bool from_isr_;
  };

  SubmitResult submit_internal(TargetId target, Event* e, const EventQos& qos, StagingAdmission admission,
                               bool from_isr) noexcept {
    /* Monotonic clock is sampled only when the M4 policy or Low-aging
       staging path needs it. Cached here so those two paths share one
       sample; High/Normal staged events carry an unused zero timestamp. */
    uint64_t now_ns = 0U;
    bool now_init = false;

    /* Arrival-window reservation (design 4.1). The policy type carries
       the state, so a coordinator built without admission has neither
       the field nor any of the rollback branches below. Every failure
       path leaves scope with the reservation still active and hands the
       window back; only the two successful outcomes commit it. */
    typename AdmissionT::Reservation arrival{};

    /* --- C1: target must be bound ------------------------------------ */
    AoBase* ao = registry_.lookup(target);
    if (nullptr == ao) {
      event_gc(e);
      return {SubmitDisposition::RejectedState, 0U};
    }

    /* --- admission lane --------------------------------------------- */
    /* Refuse a lane the Config never opened BEFORE taking a lease or
       touching capacity accounting: an unsupported lane is a caller error,
       and silently degrading it to ordinary traffic would let a caller
       believe it holds a reservation it does not have. */
    const PriorityClass priority_class = ao->priority_class();
    if (false == staging_.admission_valid(priority_class, admission)) {
      event_gc(e);
      monitor_.record_disposition(SubmitDisposition::RejectedState);
      return {SubmitDisposition::RejectedState, 0U};
    }

    SubmissionLease lease(staging_, pal_, from_isr);
    if (false == lease.admitted()) {
      event_gc(e);
      monitor_.record_disposition(SubmitDisposition::RejectedState);
      return {SubmitDisposition::RejectedState, 0U};
    }

    /* --- M6 overload guard ------------------------------------------ */
    Breaker<typename StagingT::ConfigType>& target_breaker = detail::target_breaker(breaker_, target);
    /* One owner for the "which levels drop non-critical traffic" rule: the
       breaker. Re-deriving it from level() here let the two drift apart. */
    if (false == qos.critical && target_breaker.drop_non_critical()) {
      monitor_.record_disposition(SubmitDisposition::DroppedOverload);
      event_gc(e);
      return {SubmitDisposition::DroppedOverload, 0U};
    }

    /* --- M4 policy evaluation --------------------------------------- */
    if (policy_ops_ != nullptr) {
      if (false == now_init) {
        now_ns = pal_.monotonic_ns();
        now_init = true;
      }
      PolicyResult pr = policy_ops_->evaluate(policy_ctx_, target, *e, qos, now_ns);
      if (false == pr.accept) {
        const SubmitDisposition disp =
            (0U != pr.reason) ? SubmitDisposition::DroppedRateLimit : SubmitDisposition::DroppedPolicy;
        monitor_.record_rejection(target, RejectReason::kC7Context);
        monitor_.record_disposition(disp);
        event_gc(e);
        return {disp, pr.reason};
      }
      /* Merge hint: not implemented in v1 (no per-signal MergeCell
         registry in coordinator); fall through to staging. */
    }

    /* --- arrival interval gate (design 4.1) -------------------------- */
    /* Runs after overload/policy rejection so a rejected submission never
       consumes the window, and immediately before direct dispatch so the
       recorded timestamp is the arrival of an admitted submission. Task
       and ISR producers share one CAS; nothing is held across the HSM. */
    if constexpr (AdmissionT::kEnabled) {
      if (0U != admission_.limits_for(target).min_arrival.value) {
        if (false == now_init) {
          now_ns = pal_.monotonic_ns();
          now_init = true;
        }
        if (false == admission_.reserve(target, now_ns, arrival)) {
          event_gc(e);
          monitor_.record_rejection(target, RejectReason::kC8ArrivalInterval);
          monitor_.record_disposition(SubmitDisposition::DroppedRateLimit);
          monitor_.record_admission_snapshot(target, SubmitDisposition::DroppedRateLimit, kArrivalTooSoon, now_ns,
                                             ao->rtc_budget_ns());
          return {SubmitDisposition::DroppedRateLimit, kArrivalTooSoon};
        }
      }
    }

    /* --- M1 direct dispatch (Task path only) ------------------------ */
    /* The allocation reference transfers directly to the successful
       dispatch or, after a lost race, to the staging path below. */
    if (false == from_isr && ao->direct_eligible() && target_breaker.direct_allowed(target)) {
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
          if ((ao->pending().load() > 0U) && staging_.request_dispatcher_wake()) {
            pal_.signal_dispatcher_from_task();
          }
          /* direct completed: consume the allocation reference */
          event_gc(e);
          monitor_.record_disposition(SubmitDisposition::Direct);
          if constexpr (AdmissionT::kEnabled) {
            /* Admitted and executed: the arrival counts. */
            arrival.commit();
          }
          return {SubmitDisposition::Direct, 0U};
        }
        /* Lost the direct race: transfer the allocation reference to
           staging below. Do NOT reclaim the block. */
        monitor_.record_lease_contention(target);
      } else {
        /* lease non-Idle: fall through to staging (no ref taken yet) */
        monitor_.record_lease_contention(target);
      }
    }

    /* --- staging (queued) ------------------------------------------- */
    if (false == now_init && (PriorityClass::Low == priority_class)) {
      now_ns = pal_.monotonic_ns();
      now_init = true;
    }
    PendingCounter& pending = ao->pending();
    if constexpr (AdmissionT::kEnabled) {
      /* A capped submission needs a timestamp for its snapshot even
         when it is neither Low nor policy-evaluated. */
      if (false == now_init && (0U != admission_.backlog_cap(target))) {
        now_ns = pal_.monotonic_ns();
        now_init = true;
      }
      /* Reserve the pending slot atomically against the cap before the
         slot becomes visible. Returning with `arrival` uncommitted
         hands the window back, so a cap rejection consumes neither
         capacity nor interval. qos.critical does not exempt a target
         from its own backlog cap. */
      if (false == pending.try_increment(admission_.backlog_cap(target))) {
        event_gc(e);
        monitor_.record_rejection(target, RejectReason::kC9BacklogCap);
        monitor_.record_disposition(SubmitDisposition::RejectedFull);
        monitor_.record_admission_snapshot(target, SubmitDisposition::RejectedFull, kBacklogFull, now_ns,
                                           ao->rtc_budget_ns());
        return {SubmitDisposition::RejectedFull, kBacklogFull};
      }
    } else {
      pending.increment();
    }
    if (false == staging_.enqueue(target, e, priority_class, now_ns, qos.critical, admission)) {
      pending.decrement();
      /* The event is dropped: consume the allocation reference. */
      event_gc(e);
      monitor_.record_overflow();
      target_breaker.on_overflow();
      monitor_.record_disposition(SubmitDisposition::RejectedFull);
      if constexpr (AdmissionT::kEnabled) {
        /* A full lane is a rejection too (design 6.2), so the ring
           keeps target, path, time and budget for it. The returned
           reason stays 0: that is the historical contract of this
           path and no caller may see it change. */
        monitor_.record_admission_snapshot(target, SubmitDisposition::RejectedFull, kStagingFull, now_ns,
                                           ao->rtc_budget_ns());
      }
      return {SubmitDisposition::RejectedFull, 0U};
    }
    if constexpr (AdmissionT::kEnabled) {
      /* Queued: the arrival counts. */
      arrival.commit();
    }

    /* Coalesce wakeups behind one latch. Queue publication happens before
       this acq_rel exchange; once the Dispatcher arms its wait, exactly the
       first producer owns the PAL signal. */
    if (staging_.request_dispatcher_wake()) {
      if (false == from_isr) {
        pal_.signal_dispatcher_from_task();
      } else {
        pal_.signal_dispatcher_from_isr();
      }
    }
    monitor_.record_disposition(SubmitDisposition::Queued);
    monitor_.record_pending(target, pending.load());
    return {SubmitDisposition::Queued, 0U};
  }

  StagingT& staging_;
  RegistryT& registry_;
  MonitorT& monitor_;
  BreakerRouterT& breaker_;
  PalT& pal_;
  const PolicyOps* policy_ops_;
  void* policy_ctx_;
  /* NoAdmission is empty; StaticAdmission owns one ArrivalGate per target
     plus the critical section those gates run under, bound from the PAL
     above. Either way the storage is fixed and shared by the task and ISR
     entry points, never allocated. */
  AdmissionT admission_;
};

}  // namespace coact
