// coact worst-case dispatch latency evidence for full partitions.
// SPDX-License-Identifier: MIT
#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/coordinator.hpp"
#include "coact/dispatcher.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/monitor.hpp"
#include "coact/pal_posix.hpp"
#include "coact/pool.hpp"
#include "coact/runtime.hpp"
#include "coact/staging.hpp"
#include "test/test_harness.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>

namespace {

struct LatencyCfg {
  enum : uint8_t { kMaxAo = 3U, kMaxStateDepth = 6U, kMaxDirectDepth = 4U, kBatchSizeMax = 8U };
  enum : uint16_t {
    kHighCapacity = 16U,
    kNormalCapacity = 16U,
    kLowCapacity = 16U,
    kCooldownCycles = 100U,
    kHighCriticalReserve = 0U,
    kNormalReservedCapacity = 0U
  };
  enum : uint32_t {
    kBatchTimeoutMs = 5U,
    kLowMaxWaitMs = 20U,
    kShutdownDrainTimeoutMs = 0U,
    kDispatcherStackBytes = 4096U
  };
  enum : uint64_t { kDirectBudgetNs = 50000ULL, kRtcBudgetNs = 5000000ULL };
};

struct Ctx {};
static void noop_entry(Ctx&) {}
static void noop_exit(Ctx&) {}
static bool always(const Ctx&, const coact::Event&) {
  return true;
}

static constexpr int kWcetUs = 200;
static std::atomic<int> g_high_n{0};
static std::atomic<int> g_normal_n{0};
static std::atomic<int> g_low_n{0};
static std::atomic<int64_t> g_high_start_ns{0};
static std::atomic<int64_t> g_low_start_ns{0};

static void busy_wcet() {
  const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(kWcetUs);
  while (std::chrono::steady_clock::now() < until) {
  }
}

static void action_high(Ctx&, const coact::Event& e) {
  if (e.signal == 2U) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    g_high_start_ns.store(ns, std::memory_order_release);
  }
  busy_wcet();
  g_high_n.fetch_add(1, std::memory_order_relaxed);
}

static void action_normal(Ctx&, const coact::Event&) {
  busy_wcet();
  g_normal_n.fetch_add(1, std::memory_order_relaxed);
}

static void action_low(Ctx&, const coact::Event& e) {
  if (e.signal == 2U) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    g_low_start_ns.store(ns, std::memory_order_release);
  }
  busy_wcet();
  g_low_n.fetch_add(1, std::memory_order_relaxed);
}

static const coact::StateDef<Ctx> kStates[] = {
    {-1, nullptr, nullptr},
    {0, noop_entry, noop_exit},
};
static const coact::TransitionDef<Ctx> kTransH[] = {
    {1, 1U, 1, coact::TransitionKind::Internal, always, action_high},
    {1, 2U, 1, coact::TransitionKind::Internal, always, action_high},
};
static const coact::TransitionDef<Ctx> kTransN[] = {
    {1, 1U, 1, coact::TransitionKind::Internal, always, action_normal},
};
static const coact::TransitionDef<Ctx> kTransL[] = {
    {1, 1U, 1, coact::TransitionKind::Internal, always, action_low},
    {1, 2U, 1, coact::TransitionKind::Internal, always, action_low},
};

struct TraitsH {
  static coact::LogicalPrio logical_prio() { return 30U; }
  static coact::PriorityClass priority_class() { return coact::PriorityClass::High; }
  static bool direct_eligible() { return false; }
  static bool isr_direct_safe() { return false; }
  static constexpr uint64_t kRtcBudgetNs = 5000000ULL;
};
struct TraitsN {
  static coact::LogicalPrio logical_prio() { return 20U; }
  static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
  static bool direct_eligible() { return false; }
  static bool isr_direct_safe() { return false; }
  static constexpr uint64_t kRtcBudgetNs = 5000000ULL;
};
struct TraitsL {
  static coact::LogicalPrio logical_prio() { return 10U; }
  static coact::PriorityClass priority_class() { return coact::PriorityClass::Low; }
  static bool direct_eligible() { return false; }
  static bool isr_direct_safe() { return false; }
  static constexpr uint64_t kRtcBudgetNs = 5000000ULL;
};

using HsmT = coact::Hsm<Ctx>;
using AoH = coact::Ao<Ctx, HsmT, TraitsH>;
using AoN = coact::Ao<Ctx, HsmT, TraitsN>;
using AoL = coact::Ao<Ctx, HsmT, TraitsL>;

static constexpr uint16_t kBS = 16U;
static constexpr uint16_t kCAP = 128U;
using PoolT = coact::EventPool<kBS, kCAP>;
alignas(16) static unsigned char g_pool_storage[kBS * kCAP + kBS];

static int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

COACT_TEST(high_dispatch_latency_within_batch_budget) {
  g_high_n.store(0);
  g_normal_n.store(0);
  g_low_n.store(0);
  g_high_start_ns.store(0);

  AoH ao_h(kStates, 2U, kTransH, 2U, 1, 4U);
  AoN ao_n(kStates, 2U, kTransN, 1U, 1, 4U);
  AoL ao_l(kStates, 2U, kTransL, 2U, 1, 4U);
  coact::Event init_e{};
  ao_h.init(init_e);
  ao_n.init(init_e);
  ao_l.init(init_e);

  PoolT pool;
  pool.init(g_pool_storage, sizeof(g_pool_storage));

  coact::pal::Posix pal;
  coact::Runtime<LatencyCfg, coact::pal::Posix> rt(pal);
  CHECK(rt.bind(&ao_h));
  CHECK(rt.bind(&ao_n));
  CHECK(rt.bind(&ao_l));
  CHECK(rt.initialize());
  CHECK(rt.start());
  usleep(20000);

  coact::EventQos qos{false, false};
  for (int i = 0; i < 8; ++i) {
    coact::Event* e = pool.alloc(1U);
    REQUIRE(e != nullptr);
    REQUIRE_EQ(rt.coordinator().submit_from_task(coact::TargetId(2U), e, qos).disposition,
               coact::SubmitDisposition::Queued);
  }
  for (int i = 0; i < 8; ++i) {
    coact::Event* e = pool.alloc(1U);
    REQUIRE(e != nullptr);
    REQUIRE_EQ(rt.coordinator().submit_from_task(coact::TargetId(3U), e, qos).disposition,
               coact::SubmitDisposition::Queued);
  }
  for (int i = 0; i < 7; ++i) {
    coact::Event* e = pool.alloc(1U);
    REQUIRE(e != nullptr);
    REQUIRE_EQ(rt.coordinator().submit_from_task(coact::TargetId(1U), e, qos).disposition,
               coact::SubmitDisposition::Queued);
  }
  coact::Event* measured = pool.alloc(2U);
  REQUIRE(measured != nullptr);
  const int64_t t0 = now_ns();
  REQUIRE_EQ(rt.coordinator().submit_from_task(coact::TargetId(1U), measured, qos).disposition,
             coact::SubmitDisposition::Queued);

  const int64_t deadline = t0 + 200000000LL;
  while (g_high_start_ns.load(std::memory_order_acquire) == 0 && now_ns() < deadline) {
    std::this_thread::yield();
  }
  const int64_t t1 = g_high_start_ns.load();
  rt.stop();

  CHECK(t1 != 0);
  const int64_t bound_ns = static_cast<int64_t>(LatencyCfg::kBatchSizeMax) * kWcetUs * 1000LL + 5000000LL;
  CHECK((t1 - t0) <= bound_ns);
}

COACT_TEST(low_dispatch_latency_within_aging_budget) {
  g_high_n.store(0);
  g_low_n.store(0);
  g_low_start_ns.store(0);

  AoH ao_h(kStates, 2U, kTransH, 2U, 1, 4U);
  AoN ao_n(kStates, 2U, kTransN, 1U, 1, 4U);
  AoL ao_l(kStates, 2U, kTransL, 2U, 1, 4U);
  coact::Event init_e{};
  ao_h.init(init_e);
  ao_n.init(init_e);
  ao_l.init(init_e);

  PoolT pool;
  pool.init(g_pool_storage, sizeof(g_pool_storage));

  coact::pal::Posix pal;
  coact::Runtime<LatencyCfg, coact::pal::Posix> rt(pal);
  CHECK(rt.bind(&ao_h));
  CHECK(rt.bind(&ao_n));
  CHECK(rt.bind(&ao_l));
  CHECK(rt.initialize());
  CHECK(rt.start());
  usleep(20000);

  std::atomic<bool> run{true};
  std::thread feeder([&]() {
    coact::EventQos qos{false, false};
    while (run.load(std::memory_order_acquire)) {
      coact::Event* e = pool.alloc(1U);
      if (e == nullptr) {
        std::this_thread::yield();
        continue;
      }
      (void)rt.coordinator().submit_from_task(coact::TargetId(1U), e, qos);
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  });

  usleep(2000);
  coact::Event* low = pool.alloc(2U);
  REQUIRE(low != nullptr);
  const int64_t t0 = now_ns();
  REQUIRE_EQ(rt.coordinator().submit_from_task(coact::TargetId(3U), low, coact::EventQos{false, false}).disposition,
             coact::SubmitDisposition::Queued);

  const int64_t deadline = t0 + 200000000LL;
  while (g_low_start_ns.load(std::memory_order_acquire) == 0 && now_ns() < deadline) {
    std::this_thread::yield();
  }
  const int64_t t1 = g_low_start_ns.load();
  run.store(false, std::memory_order_release);
  feeder.join();
  rt.stop();

  CHECK(t1 != 0);
  const int64_t bound_ns =
      static_cast<int64_t>(LatencyCfg::kLowMaxWaitMs) * 1000000LL +
      static_cast<int64_t>(LatencyCfg::kBatchSizeMax) * kWcetUs * 1000LL + 5000000LL;
  CHECK((t1 - t0) <= bound_ns);
}

}  // namespace

COACT_TEST_MAIN()
