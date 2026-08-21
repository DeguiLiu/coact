// coact reclaimer-strategy tests (design §7.4):
//   - ReclaimBatcher must NOT hard-code its per-batch pool capacity at 4 and
//     then COACT_ASSERT on legitimate traffic. Its capacity is
//     min(kBatchSizeMax, kMaxEventPools) at compile time, or it degrades to an
//     immediate single-block reclaim when the per-batch table is full.
//   - ImmediateReclaimer and the batched reclaimer behave identically for the
//     same traffic (both leave every pool used()==0).
//   - The reclaimer strategy is selected by the board profile
//     (RttSingleCoreProfile -> immediate, HostSmpProfile -> batched).
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <type_traits>

#include "coact/event.hpp"
#include "coact/pool.hpp"

#include "test/test_harness.hpp"

namespace {

constexpr std::uint16_t kBlock = 16U;
constexpr std::uint16_t kCap = 4U;

template <std::uint16_t Stride, std::uint16_t Capacity>
struct PoolStorage {
    alignas(64) std::uint8_t data[Stride * Capacity];
};

using HostPool = coact::EventPool<kBlock, kCap, coact::HostSmpProfile>;
using CorePool = coact::EventPool<kBlock, kCap, coact::RttSingleCoreProfile>;

}  // namespace

// ---------------------------------------------------------------------------
// Compile-time capacity derivation (design §7.4): min(kBatchSizeMax,
// kMaxEventPools); the default ReclaimBatcher keeps the historical 4.
// ---------------------------------------------------------------------------
namespace {
struct SmallBatchCfg {
    enum : uint8_t { kBatchSizeMax = 3U };
};
struct TinyPoolCfg {
    enum : uint8_t { kBatchSizeMax = 2U };
};
}  // namespace

static_assert(coact::detail::reclaimer_pool_capacity<coact::DefaultConfig>() == 8U,
              "min(8, 16) must be 8");
static_assert(coact::detail::reclaimer_pool_capacity<SmallBatchCfg>() == 3U,
              "batch-max smaller than kMaxEventPools wins");
static_assert(coact::detail::reclaimer_pool_capacity<TinyPoolCfg>() == 2U,
              "capacity must be a compile-time constant");
static_assert(coact::ReclaimBatcher<>::kMaxPending == 4U,
              "default ReclaimBatcher keeps the historical capacity 4");
static_assert(coact::BatchedReclaimer<>::kMaxPending == 4U,
              "default BatchedReclaimer aliases the historical capacity 4");

// ---------------------------------------------------------------------------
// Compile-time strategy selection by profile (design §7.4 table).
// ---------------------------------------------------------------------------
static_assert(std::is_same<coact::detail::select_reclaimer_t<
                               coact::RttSingleCoreProfile, 8>,
                           coact::ImmediateReclaimer>::value,
              "single-core profile starts with immediate reclaim");
static_assert(std::is_same<coact::detail::select_reclaimer_t<
                               coact::HostSmpProfile, 8>,
                           coact::BatchedReclaimer<8>>::value,
              "SMP profile uses the batched reclaimer");

// ---------------------------------------------------------------------------
// Five distinct pools released inside one batch. The historical hard-coded
// per-batch capacity of 4 would COACT_ASSERT on the fifth; the fix either
// derives the capacity from the config or degrades to an immediate reclaim.
// The test asserts no assert fires and every pool drains to used()==0.
// ---------------------------------------------------------------------------
COACT_TEST(reclaimer_batcher_five_pools_no_assert)
{
    PoolStorage<kBlock, kCap> storage[5];
    HostPool pools[5];
    for (int i = 0; i < 5; ++i) {
        pools[i].init(storage[i].data, sizeof(storage[i].data));
    }

    coact::Event* ev[5];
    for (int i = 0; i < 5; ++i) {
        ev[i] = pools[i].alloc(static_cast<std::uint16_t>(0x1000U + i));
        REQUIRE(ev[i] != nullptr);
    }

    coact::ReclaimBatcher batcher;     // default per-batch pool capacity (4)
    batcher.begin();
    for (int i = 0; i < 5; ++i) {
        batcher.release(ev[i]);
    }
    batcher.flush();

    for (int i = 0; i < 5; ++i) {
        CHECK_EQ(pools[i].used(), 0U);
    }
}

// ---------------------------------------------------------------------------
// ImmediateReclaimer and the batched reclaimer must behave identically for the
// same traffic. The same three pools are drained by each strategy in turn
// (immediate, config-capacity batched, and an under-sized batched whose table
// overflows to the immediate fallback) - every run ends used()==0.
// ---------------------------------------------------------------------------
namespace {

// Run one full scenario on `pools` with the given reclaimer strategy: alloc two
// events per pool, hold a reference, release all through the strategy, flush,
// and require every pool to drain.
template <typename Reclaimer>
void run_equivalent_scenario(HostPool* pools,
                             PoolStorage<kBlock, kCap>* storage)
{
    for (int i = 0; i < 3; ++i) {
        pools[i].init(storage[i].data, sizeof(storage[i].data));
    }
    coact::Event* ev[3][2];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 2; ++j) {
            ev[i][j] = pools[i].alloc(static_cast<std::uint16_t>(0x20U + i * 2U + j));
            REQUIRE(ev[i][j] != nullptr);
        }
    }

    Reclaimer reclaim;
    reclaim.begin();
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 2; ++j) {
            reclaim.release(ev[i][j]);
        }
    }
    reclaim.flush();

    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(pools[i].used(), 0U);
    }
}

}  // namespace

COACT_TEST(reclaimer_immediate_and_batched_equivalent)
{
    PoolStorage<kBlock, kCap> storage[3];
    HostPool pools[3];

    // Immediate: every final reference returns right away.
    run_equivalent_scenario<coact::ImmediateReclaimer>(pools, storage);
    // Batched at the config-derived capacity (8 for DefaultConfig).
    run_equivalent_scenario<
        coact::BatchedReclaimer<coact::detail::reclaimer_pool_capacity<
            coact::DefaultConfig>()>>(pools, storage);
    // Under-sized batched (capacity 2, three distinct pools): the third pool
    // overflows the table and degrades to an immediate single-block reclaim.
    run_equivalent_scenario<coact::BatchedReclaimer<2>>(pools, storage);
}

// ---------------------------------------------------------------------------
// Explicit table-full degradation: a batcher with capacity 2 receives events
// from three distinct pools in one batch - the third pool must be reclaimed
// immediately (no assert), and every pool drains to used()==0.
// ---------------------------------------------------------------------------
COACT_TEST(reclaimer_batcher_table_full_degrades_to_immediate)
{
    PoolStorage<kBlock, kCap> storage[3];
    HostPool pools[3];
    for (int i = 0; i < 3; ++i) {
        pools[i].init(storage[i].data, sizeof(storage[i].data));
    }

    coact::Event* ev[3];
    for (int i = 0; i < 3; ++i) {
        ev[i] = pools[i].alloc(static_cast<std::uint16_t>(0x30U + i));
        REQUIRE(ev[i] != nullptr);
    }

    coact::ReclaimBatcher<2> batcher;   // too small for three distinct pools
    batcher.begin();
    for (int i = 0; i < 3; ++i) {
        batcher.release(ev[i]);
    }
    batcher.flush();

    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(pools[i].used(), 0U);
    }
}

COACT_TEST(reclaimer_batcher_zero_reference_does_not_reclaim)
{
    PoolStorage<kBlock, kCap> storage;
    HostPool pool;
    pool.init(storage.data, sizeof(storage.data));

    coact::Event* event = pool.alloc(0x41U);
    REQUIRE(event != nullptr);
    event->ref_ctr = 0U;

    coact::ReclaimBatcher batcher;
    batcher.begin();
    batcher.release(event);
    batcher.flush();

    CHECK_EQ(pool.used(), 1U);

    event->ref_ctr = 1U;
    batcher.begin();
    batcher.release(event);
    batcher.flush();
    CHECK_EQ(pool.used(), 0U);
}

COACT_TEST(reclaimer_batcher_consumes_one_of_multiple_references)
{
    PoolStorage<kBlock, kCap> storage;
    HostPool pool;
    pool.init(storage.data, sizeof(storage.data));

    coact::Event* event = pool.alloc(0x42U);
    REQUIRE(event != nullptr);
    coact::event_ref_inc(event);
    CHECK_EQ(event->ref_ctr, 2U);

    coact::ReclaimBatcher batcher;
    batcher.begin();
    batcher.release(event);
    batcher.flush();

    CHECK_EQ(event->ref_ctr, 1U);
    CHECK_EQ(pool.used(), 1U);

    coact::event_gc(event);
    CHECK_EQ(pool.used(), 0U);
}

// ---------------------------------------------------------------------------
// Single-core start baseline (design §7.4): ImmediateReclaimer over an
// RttSingleCoreProfile pool returns every final reference immediately - each
// release brings used() down at once, flush() is a no-op.
// ---------------------------------------------------------------------------
COACT_TEST(reclaimer_immediate_single_core_drains)
{
    PoolStorage<kBlock, kCap> storage;
    CorePool pool;
    pool.init(storage.data, sizeof(storage.data));

    coact::Event* ev[2];
    for (int i = 0; i < 2; ++i) {
        ev[i] = pool.alloc(static_cast<std::uint16_t>(0x40U + i));
        REQUIRE(ev[i] != nullptr);
        CHECK_EQ(pool.used(), static_cast<std::uint16_t>(i + 1));
    }

    coact::ImmediateReclaimer reclaim;
    reclaim.begin();
    reclaim.release(ev[0]);
    CHECK_EQ(pool.used(), 1U);   // immediate: block already back
    reclaim.release(ev[1]);
    CHECK_EQ(pool.used(), 0U);
    reclaim.flush();             // no-op for the immediate strategy
    CHECK_EQ(pool.used(), 0U);
}

COACT_TEST_MAIN()
