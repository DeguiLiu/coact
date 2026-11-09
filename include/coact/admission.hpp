// coact admission - static per-target arrival interval and backlog rules.
// SPDX-License-Identifier: MIT
//
// Design 4.1/4.2/7: admission limits are static configuration, so they are
// expressed as types instead of a runtime table. A board declares one rule
// set; StaticAdmission validates it where the mistake is made (in the rule
// table, at compile time), folds it into an O(1) per-target lookup, and owns
// exactly one ArrivalGate per target. NoAdmission is the default and removes
// the whole mechanism from the submit path.
//
// Synchronization (design 4.1): a gate is three plain fields updated inside
// ONE bounded critical section supplied by the policy's Sync backend, so the
// submit path never depends on a lock-free 64-bit atomic. That dependency is
// real on Cortex-M: arm-none-eabi-g++ 10.3 with -mcpu=cortex-m4 -Os compiles a
// std::atomic<uint64_t> CAS into calls to __atomic_load_8 and
// __atomic_compare_exchange_8, an out-of-framework backend whose worst case
// coact cannot bound and that a bare-metal link may not provide at all.
#pragma once

#include "coact/config.hpp"
#include "coact/pal.hpp"

#include <cstddef>
#include <cstdint>

#include <array>
#include <type_traits>

namespace coact {

// ---------------------------------------------------------------------------
// Strong types. A nanosecond duration and an event count must not be
// interchangeable in a rule table; both are explicit and zero-overhead. Zero
// disables the corresponding check, so a row may leave either limit unset.
// ---------------------------------------------------------------------------
struct MinArrivalNs {
  uint64_t value = 0U;
  constexpr MinArrivalNs() noexcept = default;
  constexpr explicit MinArrivalNs(uint64_t v) noexcept : value(v) {}
};

struct MaxBacklog {
  uint16_t value = 0U;
  constexpr MaxBacklog() noexcept = default;
  constexpr explicit MaxBacklog(uint16_t v) noexcept : value(v) {}
};

static_assert(sizeof(MinArrivalNs) == sizeof(uint64_t), "coact: MinArrivalNs must be zero-overhead");
static_assert(sizeof(MaxBacklog) == sizeof(uint16_t), "coact: MaxBacklog must be zero-overhead");

// Upper bound for a configured interval. ArrivalGate itself saturates over the
// whole uint64 range, but a rule larger than one hour keeps a target closed for
// the lifetime of most products and is, in practice, a ms/ns unit mistake.
// The bound also admits legitimate low-frequency industrial rules (5 s, 10 s);
// rejecting them at compile time would force a hand-written policy instead.
inline constexpr uint64_t kMaxMinArrivalNs = 3600ULL * 1000000000ULL;  // 1 h

// One rule row: the limits that apply to a single 1-based TargetId.
struct TargetAdmission {
  TargetId target;
  MinArrivalNs min_arrival;
  MaxBacklog max_backlog;
};

// What the submit path actually reads: the resolved limits for one target.
// A target with no rule row resolves to all-zero, i.e. both checks disabled.
struct TargetLimits {
  MinArrivalNs min_arrival;
  MaxBacklog max_backlog;
};

// ---------------------------------------------------------------------------
// AdmissionRules<N>: a fixed-size rule set whose size is part of the type. A
// rule set is declared once as a constexpr static member of a traits type
// (the same idiom Config uses), so the whole table lands in .rodata or is
// folded away entirely, and validation happens at the declaration site.
// ---------------------------------------------------------------------------
template <std::size_t N>
struct AdmissionRules {
  std::array<TargetAdmission, N> rows;

  static constexpr std::size_t kCount = N;

  constexpr bool targets_in_range(uint8_t max_ao) const noexcept {
    for (std::size_t i = 0U; i < N; ++i) {
      const uint8_t t = rows[i].target.raw();
      if ((0U == t) || (t > max_ao)) {
        return false;
      }
    }
    return true;
  }

  constexpr bool no_duplicate_targets() const noexcept {
    for (std::size_t i = 0U; i < N; ++i) {
      for (std::size_t j = i + 1U; j < N; ++j) {
        if (rows[i].target == rows[j].target) {
          return false;
        }
      }
    }
    return true;
  }

  constexpr bool intervals_bounded() const noexcept {
    for (std::size_t i = 0U; i < N; ++i) {
      if (rows[i].min_arrival.value > kMaxMinArrivalNs) {
        return false;
      }
    }
    return true;
  }

  constexpr bool valid(uint8_t max_ao) const noexcept {
    return targets_in_range(max_ao) && no_duplicate_targets() && intervals_bounded();
  }
};

// Rule-set factory. A board declares one traits type per configuration:
//
//   struct BoardAdmission {
//       static constexpr auto kRules = coact::make_rules(
//           coact::TargetAdmission{coact::TargetId(1U),
//                                  coact::MinArrivalNs{1000000U},
//                                  coact::MaxBacklog{4U}});
//   };
//   using Admission = coact::StaticAdmission<BoardAdmission, MyConfig,
//                                            coact::AdmissionIrqSync>;
//
// The sync backend is a required argument. It is the one choice that decides
// whether an ISR submission may block, so it must be named at the use site
// instead of inheriting a default that is wrong on one of the two platforms.
//
// The rule count comes from the argument list, so it can never disagree with
// the rows written, and make_rules() is constexpr: the table is a constant,
// not something assembled at boot.
template <typename... Rows>
constexpr AdmissionRules<sizeof...(Rows)> make_rules(Rows... rows) noexcept {
  return AdmissionRules<sizeof...(Rows)>{{{rows...}}};
}

// ---------------------------------------------------------------------------
// ArrivalGate: per-target minimum-arrival-interval gate (design 4.1).
//
// Four plain fields, every update inside ONE bounded critical section:
//   last_admit_ns_  timestamp of the last COMMITTED admission
//   hold_ns_        timestamp of the newest in-flight reservation, 0 = none
//   holds_          number of in-flight reservations
//   ever_           true once something committed, so t=0 stays a legal
//                   admitted timestamp instead of reading as "never"
//
// The barrier for a new arrival is hold_ns_ while any reservation is in
// flight, otherwise last_admit_ns_ when ever_ is set, otherwise nothing: a
// reservation closes the window against a second producer immediately, which
// is what keeps a burst of producers from all passing the check before any of
// them commits.
//
// Why holds_ is counted instead of restoring a timestamp: a rollback that
// writes back the word it observed can resurrect a timestamp whose own
// reservation already failed. A reserves t1, B reserves t2, A's submission
// fails first and its compare no longer matches, then B fails and restores the
// word it observed, t1: the gate claims an admission at t1 that never happened
// and stays closed until t1 + min. Counting live reservations cannot reach
// that state, in either rollback order. A failed submission removes its own
// effect and nothing else, and the gate returns exactly to the last committed
// admission once the last hold leaves. While reservations overlap the newest
// one is the barrier, so the gate can only reject an arrival that the
// in-flight submission would have rejected too.
//
// One conservatism is deliberate: rolling back the newest of several
// overlapping reservations does not lower the barrier to the next-oldest
// hold. The gate then drops arrivals inside one extra interval. Keeping the
// timestamp set to recover the exact maximum would cost fixed storage per
// target; rejecting one extra event is the safe direction for a limiter.
// ---------------------------------------------------------------------------
class ArrivalGate {
 public:
  ArrivalGate() noexcept = default;

  // Check the window and, when admissible, record `now` as in flight.
  // Returns false when the interval rule rejects this arrival or when the
  // hold count is saturated; both leave the gate untouched.
  [[nodiscard]] bool try_reserve(const CriticalSection& cs, uint64_t now, MinArrivalNs min_arrival) noexcept {
    const CriticalSectionGuard guard(cs);
    const uint64_t min_ns = min_arrival.value;
    const bool held = (0U != holds_);
    const uint64_t barrier = held ? hold_ns_ : last_admit_ns_;
    if ((0U != min_ns) && (held || ever_)) {
      /* now < barrier and a wrapped subtraction both mean the clock
         moved backwards; neither may be admitted. */
      if ((now < barrier) || ((now - barrier) < min_ns)) {
        return false;
      }
    }
    if (kMaxHolds == holds_) {
      return false;
    }
    hold_ns_ = now;
    ++holds_;
    return true;
  }

  // The submission succeeded, so this arrival becomes the window. The max
  // keeps an out-of-order commit from moving the window back.
  void commit(const CriticalSection& cs, uint64_t now) noexcept {
    const CriticalSectionGuard guard(cs);
    ever_ = true;
    if (now > last_admit_ns_) {
      last_admit_ns_ = now;
    }
    release_hold();
  }

  // The submission failed: drop this reservation, keep the committed window.
  void rollback(const CriticalSection& cs) noexcept {
    const CriticalSectionGuard guard(cs);
    release_hold();
  }

  uint64_t last_admit_ns(const CriticalSection& cs) const noexcept {
    const CriticalSectionGuard guard(cs);
    return last_admit_ns_;
  }

  bool admitted_ever(const CriticalSection& cs) const noexcept {
    const CriticalSectionGuard guard(cs);
    return ever_;
  }

  // Live reservations. Diagnostic, and the way a test proves that a failed
  // submission left nothing behind.
  uint16_t holds(const CriticalSection& cs) const noexcept {
    const CriticalSectionGuard guard(cs);
    return holds_;
  }

 private:
  void release_hold() noexcept {
    if (0U != holds_) {
      --holds_;
      if (0U == holds_) {
        hold_ns_ = 0U;
      }
    }
  }

  static constexpr uint16_t kMaxHolds = 0xFFFFU;

  uint64_t last_admit_ns_ = 0U;
  uint64_t hold_ns_ = 0U;
  uint16_t holds_ = 0U;
  bool ever_ = false;
};

static_assert(sizeof(ArrivalGate) == 3U * sizeof(uint64_t),
              "coact: ArrivalGate must be three plain fields, no atomics");

// ---------------------------------------------------------------------------
// ArrivalReservation: move-only RAII handle for a reserved arrival window.
// The submit path has six early returns after the gate; a hand-written
// `bool reserved` plus a rollback at each of them is exactly the kind of
// pairing that drifts. Holding the reservation in an object makes "reserve,
// maybe fail, hand the window back" the default and commit() the single
// explicit exception: every failure path rolls back by leaving scope.
// ---------------------------------------------------------------------------
class ArrivalReservation {
 public:
  ArrivalReservation() noexcept = default;

  ArrivalReservation(ArrivalGate& gate, const CriticalSection& cs, uint64_t now) noexcept
      : gate_(&gate), cs_(&cs), now_(now) {}

  ArrivalReservation(const ArrivalReservation&) = delete;
  ArrivalReservation& operator=(const ArrivalReservation&) = delete;

  ArrivalReservation(ArrivalReservation&& other) noexcept : gate_(other.gate_), cs_(other.cs_), now_(other.now_) {
    other.gate_ = nullptr;
  }

  ArrivalReservation& operator=(ArrivalReservation&& other) noexcept {
    if (this != &other) {
      rollback();
      gate_ = other.gate_;
      cs_ = other.cs_;
      now_ = other.now_;
      other.gate_ = nullptr;
    }
    return *this;
  }

  ~ArrivalReservation() noexcept { rollback(); }

  // The submission succeeded: this arrival counts and becomes the window.
  void commit() noexcept {
    if (nullptr != gate_) {
      gate_->commit(*cs_, now_);
      gate_ = nullptr;
    }
  }

  void rollback() noexcept {
    if (nullptr != gate_) {
      gate_->rollback(*cs_);
      gate_ = nullptr;
    }
  }

  bool active() const noexcept { return nullptr != gate_; }

 private:
  ArrivalGate* gate_ = nullptr;
  const CriticalSection* cs_ = nullptr;
  uint64_t now_ = 0U;
};

// Stand-in reservation for a coordinator built without admission. Empty and
// inert, so the disabled submit path carries no state and no instructions.
struct InertReservation {
  constexpr void commit() noexcept {}
  constexpr void rollback() noexcept {}
  constexpr bool active() const noexcept { return false; }
};

static_assert(std::is_empty<InertReservation>::value, "coact: InertReservation must be empty");

// ---------------------------------------------------------------------------
// Sync backends (design 4.1). One CriticalSection per policy object, built
// once from the PAL and shared by every gate, so a gate stays three fields.
// The board names the backend in the policy type and StaticAdmission has no
// default constructor, so forgetting to bind one is a compile error instead
// of a submit path that silently serializes nothing.
//
//   AdmissionIrqSync   single-core target. The PAL irq mask, the same hook
//                      EventPool and Staging bind. Requires a PAL whose
//                      irq_save/irq_restore actually mask interrupts; a
//                      POSIX PAL's hooks are a documented no-op, so host
//                      tests must not use this backend across threads.
//   AdmissionSpinSync  SMP host verification. A spinlock owned by the policy,
//                      for PALs without a real irq mask. Never use it where
//                      an ISR shares the policy: the ISR can preempt the
//                      lock holder on the same core and spin forever.
// ---------------------------------------------------------------------------
struct AdmissionIrqSync {
  CriticalSection cs{};

  template <typename PalT>
  void bind(PalT& pal) noexcept {
    cs = make_critical_section(pal);
  }
};

struct AdmissionSpinSync {
  SpinCriticalSection spin{};
  CriticalSection cs{};

  template <typename PalT>
  void bind(PalT& pal) noexcept {
    (void)pal;
    cs = make_spin_critical_section(spin);
  }
};

static_assert(sizeof(AdmissionIrqSync) == sizeof(CriticalSection),
              "coact: AdmissionIrqSync must be just the hook triple");

// ---------------------------------------------------------------------------
// Admission policies (design 7: no runtime table, no new abstraction layer).
// The coordinator selects one as a template parameter and branches with
// `if constexpr`, so the disabled policy leaves no code behind.
// ---------------------------------------------------------------------------

// Default: admission compiled out. The submit path is byte-for-byte the
// historical one.
struct NoAdmission {
  static constexpr bool kEnabled = false;
  using Reservation = InertReservation;

  NoAdmission() noexcept = default;
  // Same construction shape as StaticAdmission, so the coordinator binds
  // either from the PAL it already holds. Nothing to install here.
  template <typename PalT>
  explicit NoAdmission(PalT&) noexcept {}
};

// Static rule set. `RulesT` is a traits type exposing
// `static constexpr AdmissionRules<N> kRules`, mirroring how Config carries
// compile-time sizing. Every rule-table mistake below is a compile error at
// the instantiation site instead of a silently ignored row at boot.
// `Sync` selects the critical-section backend, see AdmissionIrqSync above.
template <typename RulesT, typename Config, typename Sync>
class StaticAdmission {
 public:
  static constexpr bool kEnabled = true;
  using Reservation = ArrivalReservation;
  using Rules = decltype(RulesT::kRules);
  using SyncBackend = Sync;

  static constexpr uint8_t kMaxAo = Config::kMaxAo;
  static constexpr std::size_t kRuleCount = Rules::kCount;
  static constexpr const Rules& kRules = RulesT::kRules;

  static_assert(kRuleCount <= static_cast<std::size_t>(kMaxAo),
                "coact: more admission rules than Config::kMaxAo targets");
  static_assert(RulesT::kRules.targets_in_range(kMaxAo), "coact: admission rule names a target outside 1..kMaxAo");
  static_assert(RulesT::kRules.no_duplicate_targets(), "coact: duplicate TargetId in the admission rule set");
  static_assert(RulesT::kRules.intervals_bounded(), "coact: min_arrival above kMaxMinArrivalNs");

  StaticAdmission() noexcept = delete;

  /* A copy would duplicate the gates while sharing the staging queue, which
     silently defeats the interval rule. The policy is owned by exactly one
     Coordinator. */
  StaticAdmission(const StaticAdmission&) = delete;
  StaticAdmission& operator=(const StaticAdmission&) = delete;

  // The only constructor: it installs the critical section every gate runs
  // under. A policy without one would still compile and would serialize
  // nothing, which is the failure mode this deletes.
  template <typename PalT>
  explicit StaticAdmission(PalT& pal) noexcept {
    sync_.bind(pal);
  }

  const CriticalSection& critical_section() const noexcept { return sync_.cs; }

  // O(1) lookup into the compile-time generated index. Slot 0 is never a
  // bound target, so an out-of-range id degrades to "no limits" instead of
  // reading past the array.
  constexpr const TargetLimits& limits_for(TargetId target) const noexcept {
    const uint8_t raw = target.raw();
    return (raw <= kMaxAo) ? kIndex[raw] : kIndex[0];
  }

  constexpr uint16_t backlog_cap(TargetId target) const noexcept { return limits_for(target).max_backlog.value; }

  // Reserve the arrival window for `target` at `now_ns`. Returns false when
  // the interval rule rejects this arrival; returns true with an inert
  // reservation when the target has no interval rule, so the caller has one
  // uniform success path and commits only what it reserved.
  bool reserve(TargetId target, uint64_t now_ns, ArrivalReservation& out) noexcept {
    const uint64_t min_ns = limits_for(target).min_arrival.value;
    if (0U == min_ns) {
      /* Drop any handle left over from a previous call: the caller is
         promised an inert reservation, not whatever it passed in. */
      out = ArrivalReservation{};
      return true;
    }
    ArrivalGate& gate = gate_for(target);
    if (false == gate.try_reserve(sync_.cs, now_ns, MinArrivalNs{min_ns})) {
      /* A rejected reserve must not leave a stale handle behind: the
         caller may roll back or destroy it on the failure path. */
      out = ArrivalReservation{};
      return false;
    }
    out = ArrivalReservation(gate, sync_.cs, now_ns);
    return true;
  }

  // Diagnostics and test access: the committed window of one target, read
  // under the same critical section the submit path uses.
  uint64_t last_admit_ns(TargetId target) const noexcept { return gate_for(target).last_admit_ns(sync_.cs); }

  bool admitted_ever(TargetId target) const noexcept { return gate_for(target).admitted_ever(sync_.cs); }

  uint16_t holds(TargetId target) const noexcept { return gate_for(target).holds(sync_.cs); }

  // Exposed for tests and for the Renode fixture: the gate state of one
  // target. Slot 0 is the dead-letter gate for out-of-range ids.
  ArrivalGate& gate_for(TargetId target) noexcept {
    const uint8_t raw = target.raw();
    return gates_[(raw <= kMaxAo) ? raw : 0U];
  }

  const ArrivalGate& gate_for(TargetId target) const noexcept {
    const uint8_t raw = target.raw();
    return gates_[(raw <= kMaxAo) ? raw : 0U];
  }

 private:
  // Rule rows are scanned once, here, at compile time. The submit path reads
  // a flat array instead of walking the table on every event.
  static constexpr std::array<TargetLimits, static_cast<std::size_t>(kMaxAo) + 1U> build_index() noexcept {
    std::array<TargetLimits, static_cast<std::size_t>(kMaxAo) + 1U> index{};
    for (std::size_t i = 0U; i < kRuleCount; ++i) {
      const TargetAdmission& row = kRules.rows[i];
      index[row.target.raw()] = TargetLimits{row.min_arrival, row.max_backlog};
    }
    return index;
  }

  static constexpr auto kIndex = build_index();

  Sync sync_;

  // One gate per 1-based TargetId, fixed storage, shared by the task and
  // ISR entry points, never allocated and holding no atomics.
  std::array<ArrivalGate, static_cast<std::size_t>(kMaxAo) + 1U> gates_{};
};

}  // namespace coact
