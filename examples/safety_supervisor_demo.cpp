// coact example: BSP-level safety supervisor.
//
// coact owns the detection mechanisms: the Breaker state machine, Monitor
// counters and SubmitResult codes. It does not own the evidence that feeds
// them, because only the board knows what a stuck worker, a saturated queue
// or an exhausted reserve looks like in its product. This example is the BSP
// half, kept outside coact core:
//
//   watermark     staging occupancy -> Monitor::sample_watermark and
//                 BreakerBank::broadcast_watermark (sustained >80% -> L2)
//   watchdog      no worker progress while work is pending ->
//                 BreakerBank::broadcast_watchdog (-> Safe)
//   key reserve   a critical submit rejected while the High reserve is
//                 empty -> broadcast_key_reserve_exhausted (-> Safe)
//   recovery      product safe-restore -> broadcast_external_safe_restore,
//                 then controlled probe dispatches -> on_probe_success and
//                 the dispatcher's own dispatch cycles -> Normal
//
// The scenario is self-verifying: every expected transition is asserted and
// the process exits non-zero on the first broken expectation. Host only
// (POSIX PAL); on target the same supervisor runs against the FreeRTOS or
// RT-Thread PAL, with the watchdog driver behind PAL::watchdog_progress.
//
// SPDX-License-Identifier: MIT

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/monitor.hpp"
#include "coact/pal_posix.hpp"
#include "coact/pool.hpp"
#include "coact/runtime.hpp"

#include <cstdint>
#include <cstdio>

#include <atomic>
#include <unistd.h>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
  std::printf("  [%s] %s\n", condition ? "ok" : "FAIL", what);
  if (!condition) {
    ++g_failures;
  }
}

/* Two High cells stay reserved for critical events, so ordinary traffic
   cannot consume them; a critical submit that still fails means the reserve
   itself is gone. Eight cooldown cycles keep the demo short, a product uses
   the real recovery window. */
struct SupervisorConfig : coact::DefaultConfig {
  enum : uint16_t { kCooldownCycles = 8U, kHighCriticalReserve = 2U };
};

enum : uint16_t { kWork = 1U, kProbe = 2U };

/* Worker state. `block` is the BSP-side fault injection: while it is set the
   handler does not return, which is what the watchdog is for. Atomics because
   the dispatcher thread and the supervisor thread both touch it. */
struct WorkContext {
  std::atomic<bool> block{false};
  std::atomic<uint32_t> handled{0U};
  std::atomic<uint32_t> probes{0U};
};

void work_action(WorkContext& ctx, const coact::Event&) {
  ctx.handled.fetch_add(1U, std::memory_order_relaxed);
  while (ctx.block.load(std::memory_order_acquire)) {
    ::usleep(200U);  // stuck handler: the dispatcher is inside one dispatch
  }
}

void probe_action(WorkContext& ctx, const coact::Event&) {
  ctx.handled.fetch_add(1U, std::memory_order_relaxed);
  ctx.probes.fetch_add(1U, std::memory_order_relaxed);
}

constexpr int8_t kRoot = 0;
constexpr int8_t kIdle = 1;

const coact::StateDef<WorkContext> kStates[] = {
    {-1, nullptr, nullptr, "Root", kIdle},
    {kRoot, nullptr, nullptr, "Idle"},
};

const coact::TransitionDef<WorkContext> kTransitions[] = {
    {kIdle, kWork, kIdle, coact::TransitionKind::Internal, nullptr, work_action},
    {kIdle, kProbe, kIdle, coact::TransitionKind::Internal, nullptr, probe_action},
};

struct WorkerTraits {
  static coact::LogicalPrio logical_prio() { return 10U; }
  static coact::PriorityClass priority_class() { return coact::PriorityClass::High; }
  static bool direct_eligible() { return false; }
  static bool isr_direct_safe() { return false; }
  static constexpr uint64_t kRtcBudgetNs = coact::DefaultConfig::kRtcBudgetNs;
};

using Rt = coact::Runtime<SupervisorConfig, coact::pal::Posix>;
using WorkerAo = coact::Ao<WorkContext, coact::Hsm<WorkContext>, WorkerTraits>;
using coact::BreakerLevel;
using coact::SubmitDisposition;

/* BSP supervisor. No allocation, no threads of its own: the product calls
   these entry points from its own health-monitor task and from the paths that
   already know a submit failed. */
class SafetySupervisor {
 public:
  static constexpr uint64_t kWatchdogTimeoutNs = 5U * 1000U * 1000U;

  SafetySupervisor(Rt& rt, coact::TargetId target) noexcept : rt_(rt), target_(target) {}

  /* Per-partition occupancy goes to Monitor for the record; the maximum goes
     to every breaker, which models one backlog rather than three queues. */
  void sample_watermarks() noexcept {
    const uint8_t high = rt_.partition_watermark(coact::PriorityClass::High);
    const uint8_t normal = rt_.partition_watermark(coact::PriorityClass::Normal);
    const uint8_t low = rt_.partition_watermark(coact::PriorityClass::Low);
    rt_.monitor().sample_watermark(coact::PriorityClass::High, high);
    rt_.monitor().sample_watermark(coact::PriorityClass::Normal, normal);
    rt_.monitor().sample_watermark(coact::PriorityClass::Low, low);
    uint8_t overall = high;
    if (normal > overall) {
      overall = normal;
    }
    if (low > overall) {
      overall = low;
    }
    rt_.breakers().broadcast_watermark(overall);
    ++samples_;
  }

  /* Worker progress is the only liveness evidence the BSP has. No progress
     for kWatchdogTimeoutNs while work is pending means a handler did not
     return; the independent watchdog covers the case where this supervisor
     itself stops running. */
  bool service_watchdog(uint32_t handled_now, bool work_pending, uint64_t now_ns) noexcept {
    if (!work_pending || (handled_now != last_handled_)) {
      last_handled_ = handled_now;
      last_progress_ns_ = now_ns;
      return false;
    }
    if ((now_ns - last_progress_ns_) < kWatchdogTimeoutNs) {
      return false;
    }
    last_progress_ns_ = now_ns;
    ++watchdog_fires_;
    rt_.breakers().broadcast_watchdog();
    return true;
  }

  /* The reserve holds cells back from ordinary traffic for exactly these
     events, so a rejected critical submit means the guaranteed lane is gone
     too. The coordinator has already reported L2; this is the BSP escalation
     to Safe. */
  void on_submit(const coact::SubmitResult& result, const coact::EventQos& qos) noexcept {
    if (qos.critical && (SubmitDisposition::RejectedFull == result.disposition)) {
      rt_.breakers().broadcast_key_reserve_exhausted();
    }
  }

  void request_safe_restore() noexcept { rt_.breakers().broadcast_external_safe_restore(); }

  void note_probe_success() noexcept { rt_.breakers().on_probe_success(target_); }

  coact::TargetId target() const noexcept { return target_; }
  uint32_t samples() const noexcept { return samples_; }
  uint32_t watchdog_fires() const noexcept { return watchdog_fires_; }

 private:
  Rt& rt_;
  coact::TargetId target_;
  uint64_t last_progress_ns_ = 0U;
  uint32_t last_handled_ = 0U;
  uint32_t samples_ = 0U;
  uint32_t watchdog_fires_ = 0U;
};

/* Block until the AO has nothing outstanding, bounded by ~2 s. */
bool drain(WorkerAo& ao) {
  for (int i = 0; i < 2000; ++i) {
    if (0U == ao.pending().load()) {
      return true;
    }
    ::usleep(1000U);
  }
  return false;
}

/* Controlled probes: each dispatched probe is one healthy window for the
   breaker. Windows are consumed by the dispatcher's own dispatch cycles, so
   one extra event may be needed after the last probe. */
bool drive_probes(Rt& rt, coact::pal::Posix& pal, coact::EventPool<16U, 64U>& pool, WorkerAo& ao,
                  SafetySupervisor& supervisor) {
  const coact::EventQos critical{true, false};
  for (int round = 0; round < 24; ++round) {
    if (BreakerLevel::Normal == rt.breakers().level(supervisor.target())) {
      return true;
    }
    coact::Event* event = pool.alloc(kProbe);
    if (nullptr == event) {
      return false;
    }
    const uint32_t before = ao.context().probes.load(std::memory_order_relaxed);
    const coact::SubmitResult result = rt.coordinator().submit_from_task(supervisor.target(), event, critical);
    if (SubmitDisposition::Queued != result.disposition) {
      return false;
    }
    for (int wait = 0; wait < 200; ++wait) {
      if (ao.context().probes.load(std::memory_order_relaxed) != before) {
        break;
      }
      ::usleep(500U);
    }
    if (ao.context().probes.load(std::memory_order_relaxed) == before) {
      return false;
    }
    supervisor.note_probe_success();
    supervisor.sample_watermarks();  // recovery keeps requiring <50%
    pal.watchdog_progress(1U);       // BSP watchdog kick on a healthy round
  }
  return BreakerLevel::Normal == rt.breakers().level(supervisor.target());
}

}  // namespace

int main() {
  coact::pal::Posix pal;

  constexpr uint16_t kBlock = 16U;
  constexpr uint16_t kCapacity = 64U;
  alignas(16) static unsigned char storage[(kBlock * kCapacity) + kBlock];
  static coact::EventPool<kBlock, kCapacity> pool;
  pool.init(storage, sizeof(storage), coact::make_critical_section(pal));

  static WorkerAo ao(kStates, static_cast<uint16_t>(sizeof(kStates) / sizeof(kStates[0])), kTransitions,
                     static_cast<uint16_t>(sizeof(kTransitions) / sizeof(kTransitions[0])), kIdle, 2U);

  static Rt rt(pal);
  if (!rt.bind(&ao)) {
    std::printf("bind failed\n");
    return 1;
  }
  rt.initialize();
  if (!rt.start()) {
    std::printf("dispatcher start failed\n");
    return 1;
  }

  SafetySupervisor supervisor(rt, coact::TargetId(1U));
  const coact::EventQos ordinary{false, false};
  const coact::EventQos critical{true, false};

  auto submit = [&](uint16_t signal, coact::EventQos qos) -> coact::SubmitResult {
    coact::Event* event = pool.alloc(signal);
    if (nullptr == event) {
      return coact::SubmitResult{SubmitDisposition::RejectedFull, 0U};
    }
    return rt.coordinator().submit_from_task(supervisor.target(), event, qos);
  };

  std::printf("=== coact safety supervisor: bounded, detectable, recoverable ===\n");

  /* ---- Phase 1: bounded queue, watermark detection --------------------- */
  std::printf("\n[1] watermark -> L2\n");
  ao.context().block.store(true, std::memory_order_release);
  /* Pin the dispatcher inside one handler first. Only then is the queue
     occupancy stable: a dispatch in flight has already released its claim,
     so sampling before the handler blocks would race with the dequeue. */
  submit(kWork, ordinary);
  for (int i = 0; (i < 2000) && (0U == ao.context().handled.load(std::memory_order_relaxed)); ++i) {
    ::usleep(500U);
  }
  check(1U == ao.context().handled.load(std::memory_order_relaxed),
        "dispatcher is pinned inside one handler that does not return");
  for (int i = 0; i < 30; ++i) {
    submit(kWork, ordinary);
  }
  for (int i = 0; (i < 500) && (31U != ao.pending().load()); ++i) {
    ::usleep(1000U);
  }
  check(31U == ao.pending().load(), "one event in the handler, 30 more accepted in the High queue");
  check(rt.partition_watermark(coact::PriorityClass::High) >= 80U,
        "High occupancy is above the 80% band and cannot grow past capacity");
  for (int i = 0; i < 3; ++i) {
    supervisor.sample_watermarks();
  }
  check(BreakerLevel::BrokenL2 == rt.breakers().level(supervisor.target()),
        "three sustained >80% samples degrade the target to L2");

  const uint32_t used_before = pool.used();
  check(SubmitDisposition::DroppedOverload == submit(kWork, ordinary).disposition,
        "L2 drops non-critical submits at the overload guard");
  check(pool.used() == used_before, "a dropped submit returns its pool block, so usage stays bounded");

  /* ---- Phase 2: key reserve exhaustion -------------------------------- */
  std::printf("\n[2] exhausted key reserve -> Safe\n");
  check(SubmitDisposition::Queued == submit(kWork, critical).disposition,
        "critical 1 takes a High cell the reserve kept free");
  check(SubmitDisposition::Queued == submit(kWork, critical).disposition, "critical 2 takes the last High cell");
  const coact::SubmitResult exhausted = submit(kWork, critical);
  supervisor.on_submit(exhausted, critical);
  check(SubmitDisposition::RejectedFull == exhausted.disposition,
        "critical 3 is rejected: no High cell is left for safety traffic");
  check(BreakerLevel::Safe == rt.breakers().level(supervisor.target()),
        "BSP escalates a rejected critical submit to Safe");
  check(rt.breakers().safe_events_only(supervisor.target()), "Safe accepts safety events only");

  /* ---- Phase 3: safe restore, probes, back to Normal ------------------ */
  std::printf("\n[3] safe restore + controlled probes -> Normal\n");
  ao.context().block.store(false, std::memory_order_release);
  check(drain(ao), "queued work drains once the handler returns");
  supervisor.request_safe_restore();
  check(BreakerLevel::Recovering == rt.breakers().level(supervisor.target()),
        "external safe restore moves Safe -> Recovering");
  check(drive_probes(rt, pal, pool, ao, supervisor), "three healthy probe windows return the target to Normal");
  check(0U == ao.pending().load(), "no event is left outstanding");

  /* ---- Phase 4: watchdog ---------------------------------------------- */
  std::printf("\n[4] watchdog -> Safe\n");
  ao.context().block.store(true, std::memory_order_release);
  submit(kWork, ordinary);
  bool fired = false;
  for (int i = 0; (i < 200) && !fired; ++i) {
    ::usleep(1000U);
    fired = supervisor.service_watchdog(ao.context().handled.load(std::memory_order_relaxed), ao.pending().load() > 0U,
                                        pal.monotonic_ns());
  }
  check(fired, "no worker progress while work is pending trips the watchdog");
  check(BreakerLevel::Safe == rt.breakers().level(supervisor.target()), "watchdog input drives the target to Safe");
  check(1U == supervisor.watchdog_fires(), "the watchdog fired exactly once for this stall");

  /* ---- Phase 5: the system comes back --------------------------------- */
  std::printf("\n[5] recovery after the watchdog trip\n");
  ao.context().block.store(false, std::memory_order_release);
  check(drain(ao), "stalled work drains after the watchdog trip");
  supervisor.request_safe_restore();
  check(drive_probes(rt, pal, pool, ao, supervisor), "probes bring the target back to Normal");

  rt.stop();

  const coact::GlobalCounters& global = rt.monitor().global();
  const coact::AoCounters& ao_counters = rt.monitor().ao(supervisor.target());
  std::printf("\n=== evidence ===\n");
  std::printf("final level:            %d (0=Normal)\n", static_cast<int>(rt.breakers().level(supervisor.target())));
  std::printf("watermark samples:      %u\n", static_cast<unsigned>(supervisor.samples()));
  std::printf("watchdog fires:         %u\n", static_cast<unsigned>(supervisor.watchdog_fires()));
  std::printf("high-water samples:     %u\n",
              static_cast<unsigned>(global.high_water_count[static_cast<size_t>(coact::PriorityClass::High)].load()));
  std::printf("overflow reports:       %u\n", static_cast<unsigned>(global.overflow.load()));
  std::printf("overload drops:         %u\n", static_cast<unsigned>(global.disposition_overload.load()));
  std::printf(
      "rejections at backlog:  %u\n",
      static_cast<unsigned>(ao_counters.rejections[static_cast<size_t>(coact::RejectReason::kC9BacklogCap)].load()));
  std::printf("pool blocks in use:     %u\n", static_cast<unsigned>(pool.used()));
  std::printf("probes dispatched:      %u\n", static_cast<unsigned>(ao.context().probes.load()));

  check(0U == pool.used(), "every event returned to the pool");
  check(BreakerLevel::Normal == rt.breakers().level(supervisor.target()), "system ends in Normal");

  std::printf("\n%s\n", (0 == g_failures) ? "PASS" : "FAIL");
  return (0 == g_failures) ? 0 : 1;
}
