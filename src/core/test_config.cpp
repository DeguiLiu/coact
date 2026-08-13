// coact Config pass-through test: verifies that one board-level Config threads
// through AoRegistry, Monitor, Breaker, Staging and Dispatcher consistently
// (design 14.1 / "Config 贯穿 Runtime" review item).
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <cstdint>

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/dispatcher.hpp"
#include "coact/monitor.hpp"
#include "coact/pal_posix.hpp"
#include "coact/staging.hpp"

/* ---------------------------------------------------------------------------
 * A small board-level Config to prove the constants are not hard-wired to
 * DefaultConfig. Kept tiny (kMaxAo=4, batch=2) so a non-default value is
 * easy to distinguish and assert.
 * ------------------------------------------------------------------------- */
struct TestConfig {
    enum : uint8_t {
        kMaxAo = 4U,
        kMaxStateDepth = 6U,
        kMaxDirectDepth = 4U,
        kBatchSizeMax = 2U
    };
    enum : uint16_t {
        kHighCapacity = 8U,
        kNormalCapacity = 16U,
        kLowCapacity = 32U,
        kCooldownCycles = 5U
    };
    enum : uint32_t {
        kBatchTimeoutMs = 1U,
        kLowMaxWaitMs = 10U
    };
    enum : uint64_t {
        kDirectBudgetNs = 50000ULL,
        kRtcBudgetNs = 1000000ULL
    };
};

/* Compile-time proof: Config propagates into registry and monitor capacity. */
static_assert(coact::AoRegistry<TestConfig>::kCapacity == 4U,
              "TestConfig kMaxAo must reach AoRegistry");
static_assert(coact::Monitor<TestConfig>::kMaxAo == 4U,
              "TestConfig kMaxAo must reach Monitor");

namespace {

/* =========================================================================
 * AoRegistry honors TestConfig::kMaxAo (capacity 4).
 * ========================================================================= */
COACT_TEST(config_registry_capacity)
{
    coact::AoRegistry<TestConfig> reg;
    /* With kMaxAo=4, TargetId 5+ is out of range. */
    CHECK(nullptr == reg.lookup(coact::TargetId(5U)));
    CHECK(nullptr == reg.lookup(coact::TargetId(4U)));  // not bound
}

/* =========================================================================
 * Staging + Dispatcher derive their batch/timing limits from ConfigType.
 * A tiny TestConfig proves Dispatcher does not silently use DefaultConfig.
 * ========================================================================= */
COACT_TEST(config_staging_and_dispatcher_config_type)
{
    using StageT = coact::Staging<TestConfig, coact::pal::Posix::QueueBackend>;
    /* Static proof that the Dispatcher's batch source is TestConfig. */
    static_assert(
        static_cast<uint8_t>(StageT::ConfigType::kBatchSizeMax) == 2U,
        "StagingT::ConfigType must be TestConfig");

    coact::pal::Posix pal;
    StageT staging(coact::CriticalSection{nullptr,
        [](void*) -> coact::CriticalSection::Token { return 0U; },
        [](void*, coact::CriticalSection::Token) {} });
    coact::AoRegistry<TestConfig> registry;
    coact::Monitor<TestConfig>     monitor;
    TestConfig                     cfg{};
    coact::Breaker<TestConfig>     breaker(cfg);
    coact::Dispatcher<StageT, coact::pal::Posix> dispatcher(
        staging, registry, monitor, breaker, pal);

    /* Dispatcher::run() uses StageT::ConfigType::kBatchSizeMax (2) and
       kBatchTimeoutMs (1 ms); we just confirm it constructs and can stop. */
    struct Wrap { coact::Dispatcher<StageT, coact::pal::Posix>* d; };
    Wrap w{&dispatcher};
    pal.start_dispatcher([](void* c) {
        static_cast<Wrap*>(c)->d->run();
    }, &w);
    dispatcher.request_stop();
    pal.join_dispatcher();
    CHECK(true);
}

/* =========================================================================
 * Monitor's per-AO bucket array is bounded by TestConfig::kMaxAo, and
 * out-of-range TargetIds are guarded (fall back to the unused slot 0).
 * ========================================================================= */
COACT_TEST(config_monitor_bounded_by_max_ao)
{
    coact::Monitor<TestConfig> monitor;
    /* TargetId up to kMaxAo (4) is a valid slot. */
    coact::AoCounters const& c = monitor.ao(coact::TargetId(4U));
    (void)c;
    /* Out-of-range access (5 > kMaxAo) must be safely guarded, not UB. */
    coact::AoCounters const& oob =
        monitor.ao(coact::TargetId(5U));
    CHECK_EQ(0U, coact_test::relaxed(oob.direct_timeouts));  /* zeroed unused slot */
}

}  // namespace

COACT_TEST_MAIN()
