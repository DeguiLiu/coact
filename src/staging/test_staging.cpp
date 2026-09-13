// coact staging module host tests: partition routing, full-false negative,
// priority-first batching, Low aging exception, BatchSizeMax bound, watermark
// bands, empty-false and concurrent multi-producer no-loss. See contract 4.7.
// SPDX-License-Identifier: MIT

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "coact/config.hpp"
#include "coact/event.hpp"
#include "coact/pool.hpp"
#include "coact/queue.hpp"
#include "coact/staging.hpp"

#include "test/test_harness.hpp"

namespace {

// ---------------------------------------------------------------------------
// A Config with distinct capacities and an older Low aging budget, so tests
// trigger aging with small (non-millisecond) clock deltas and scale watermark
// bands cheaply.
// ---------------------------------------------------------------------------
struct TestConfig {
    enum : uint8_t {
        kBatchSizeMax = 4U
    };
    enum : uint16_t {
        kHighCapacity = 4U,
        kNormalCapacity = 4U,
        kLowCapacity = 4U
    };
    enum : uint32_t {
        kBatchTimeoutMs = 1U,
        kLowMaxWaitMs = 1U      // 1 ms aging budget -> 1e6 ns wait
    };
};

using MpscStaging = coact::Staging<TestConfig, coact::BoundedMpscQueue>;
using RingStaging = coact::Staging<TestConfig, coact::SingleCoreCriticalRing>;

// A config whose partition capacity comfortably exceeds BatchSizeMax, so the
// batch-size bound is observable independently of the buffered count.
struct BatchCfg {
    enum : uint8_t {
        kBatchSizeMax = 4U
    };
    enum : uint16_t {
        kHighCapacity = 8U,
        kNormalCapacity = 8U,
        kLowCapacity = 8U
    };
    enum : uint32_t {
        kBatchTimeoutMs = 1U,
        kLowMaxWaitMs = 1U
    };
};
using BatchStaging = coact::Staging<BatchCfg, coact::BoundedMpscQueue>;

// A Config that opens both reservations. High physically holds 4 cells but
// ordinary submissions may claim only 3, so one cell is always reachable by a
// critical High event. Normal reserves 2 of its 4 cells for the ReservedNormal
// lane. Each limit sums to its partition capacity, which is what makes the
// claim counters a complete account of physical occupancy.
struct ReserveCfg {
    enum : uint8_t {
        kBatchSizeMax = 8U
    };
    enum : uint16_t {
        kHighCapacity = 4U,
        kNormalCapacity = 4U,
        kLowCapacity = 4U,
        kHighCriticalReserve = 1U,
        kNormalReservedCapacity = 2U
    };
    enum : uint32_t {
        kBatchTimeoutMs = 1U,
        kLowMaxWaitMs = 1U
    };
};
using ReserveStaging = coact::Staging<ReserveCfg, coact::BoundedMpscQueue>;

// Declares ONLY the High reserve. A probe that required both constant names at
// once would not match this Config and would silently zero the reserve that IS
// declared, so this shape is the regression guard for that failure mode.
struct HighOnlyReserveCfg {
    enum : uint8_t {
        kBatchSizeMax = 8U
    };
    enum : uint16_t {
        kHighCapacity = 4U,
        kNormalCapacity = 4U,
        kLowCapacity = 4U,
        kHighCriticalReserve = 2U
    };
    enum : uint32_t {
        kBatchTimeoutMs = 1U,
        kLowMaxWaitMs = 1U
    };
};
using HighOnlyReserveStaging =
    coact::Staging<HighOnlyReserveCfg, coact::BoundedMpscQueue>;

// Declares ONLY the Normal lane capacity, the mirror image of the above.
struct NormalOnlyReserveCfg {
    enum : uint8_t {
        kBatchSizeMax = 8U
    };
    enum : uint16_t {
        kHighCapacity = 4U,
        kNormalCapacity = 4U,
        kLowCapacity = 4U,
        kNormalReservedCapacity = 1U
    };
    enum : uint32_t {
        kBatchTimeoutMs = 1U,
        kLowMaxWaitMs = 1U
    };
};
using NormalOnlyReserveStaging =
    coact::Staging<NormalOnlyReserveCfg, coact::BoundedMpscQueue>;

// Each constant must be picked up on its own, and the one that is absent must
// read as zero rather than taking the declared one down with it.
static_assert(coact::detail::StagingReserveConfig<HighOnlyReserveCfg>::kHighCritical == 2U,
              "a Config declaring only kHighCriticalReserve lost its reserve");
static_assert(coact::detail::StagingReserveConfig<HighOnlyReserveCfg>::kNormalReserved == 0U,
              "an undeclared kNormalReservedCapacity must read as zero");
static_assert(coact::detail::StagingReserveConfig<NormalOnlyReserveCfg>::kHighCritical == 0U,
              "an undeclared kHighCriticalReserve must read as zero");
static_assert(coact::detail::StagingReserveConfig<NormalOnlyReserveCfg>::kNormalReserved == 1U,
              "a Config declaring only kNormalReservedCapacity lost its lane");
static_assert(HighOnlyReserveStaging::ReserveConfig::kHighCritical == 2U,
              "Staging did not inherit the single declared reserve");

// Deliberately use distinct capacities so this probe can hold High in a
// reserved-but-unpublished state while Normal is published. It models the
// observable BoundedMpscQueue contract from mpsc_reserved_slot_is_not_ready.
struct ReservationGapCfg {
    enum : uint8_t {
        kBatchSizeMax = 4U
    };
    enum : uint16_t {
        kHighCapacity = 2U,
        kNormalCapacity = 3U,
        kLowCapacity = 4U
    };
    enum : uint32_t {
        kBatchTimeoutMs = 1U,
        kLowMaxWaitMs = 1U
    };
};

template <typename T, uint16_t Capacity>
class ReservationGapQueue {
public:
    explicit ReservationGapQueue(coact::CriticalSection) noexcept {}

    bool try_push(T&& value) noexcept
    {
        if (occupied_) {
            return false;
        }
        slot_ = std::move(value);
        occupied_ = true;
        published_ = (Capacity != ReservationGapCfg::kHighCapacity);
        return true;
    }

    bool try_pop(T& out) noexcept
    {
        if (!published_) {
            return false;
        }
        out = std::move(slot_);
        occupied_ = false;
        published_ = false;
        return true;
    }

    bool front(T& out) const noexcept
    {
        if (!published_) {
            return false;
        }
        out = slot_;
        return true;
    }

    uint16_t size() const noexcept
    {
        return occupied_ ? 1U : 0U;
    }

    bool has_ready() const noexcept
    {
        return published_;
    }

private:
    T slot_{};
    bool occupied_ = false;
    bool published_ = false;
};

using ReservationGapStaging = coact::Staging<ReservationGapCfg, ReservationGapQueue>;

// Models the narrow but real MPSC window between release-publishing a Low slot
// and returning to Staging::enqueue(). The producer is held in try_push() only
// after its Low slot becomes observable, so the consumer can verify that aging
// follows the published head payload rather than a later side-band update.
struct PublishedLowGapCfg {
    enum : uint8_t {
        kBatchSizeMax = 4U
    };
    enum : uint16_t {
        kHighCapacity = 2U,
        kNormalCapacity = 3U,
        kLowCapacity = 4U
    };
    enum : uint32_t {
        kBatchTimeoutMs = 1U,
        kLowMaxWaitMs = 1U
    };
};

struct PublishedLowGapControl {
    std::atomic<bool> low_published{false};
    std::atomic<bool> release_producer{false};
};

PublishedLowGapControl g_published_low_gap;

template <typename T, uint16_t Capacity>
class PublishedLowGapQueue {
public:
    explicit PublishedLowGapQueue(coact::CriticalSection) noexcept {}

    bool try_push(T&& value) noexcept
    {
        if (occupied_.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }
        slot_ = std::move(value);
        published_.store(true, std::memory_order_release);

        if constexpr (Capacity == PublishedLowGapCfg::kLowCapacity) {
            g_published_low_gap.low_published.store(true, std::memory_order_release);
            while (!g_published_low_gap.release_producer.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        return true;
    }

    bool try_pop(T& out) noexcept
    {
        if (!published_.load(std::memory_order_acquire)) {
            return false;
        }
        out = std::move(slot_);
        published_.store(false, std::memory_order_release);
        occupied_.store(false, std::memory_order_release);
        return true;
    }

    bool front(T& out) const noexcept
    {
        if (!published_.load(std::memory_order_acquire)) {
            return false;
        }
        out = slot_;
        return true;
    }

    uint16_t size() const noexcept
    {
        return occupied_.load(std::memory_order_acquire) ? 1U : 0U;
    }

    bool has_ready() const noexcept
    {
        return published_.load(std::memory_order_acquire);
    }

private:
    T slot_{};
    std::atomic<bool> occupied_{false};
    std::atomic<bool> published_{false};
};

using PublishedLowGapStaging = coact::Staging<PublishedLowGapCfg, PublishedLowGapQueue>;

// Counting critical-section pair used to drive the single-core ring backend.
struct CsCounters {
    int saves = 0;
    int restores = 0;
};
CsCounters g_cs;

uintptr_t cs_save(void*)
{
    ++g_cs.saves;
    return 0x0F0F0F0Fu;
}

void cs_restore(void*, uintptr_t)
{
    ++g_cs.restores;
}

coact::CriticalSection counting_cs()
{
    coact::CriticalSection cs;
    cs.save = &cs_save;
    cs.restore = &cs_restore;
    return cs;
}

// Fixed static events (pool_id == 0) whose signal doubles as a sequence tag.
// inc/gc are no-ops on static events, so draining a batch never leaks and the
// events remain usable across rounds.
coact::Event static_events[256];   // index == signal tag; < 256 distinct seqs

coact::Event* seq_event(uint8_t seq)
{
    static_events[seq].signal = seq;
    static_events[seq].pool_id = 0U;
    static_events[seq].ref_ctr = 0U;
    return &static_events[seq];
}

// ---------------------------------------------------------------------------
// Drain slots from a staging view into `out` until it reports empty (or the
// current batch refills), returning how many were taken. Works for either
// queue backend.
// ---------------------------------------------------------------------------
template <typename S>
int drain_all(S& s, std::vector<coact::StagingSlot>& out)
{
    out.clear();
    coact::StagingSlot slot{};
    int n = 0;
    s.begin_batch();
    while (s.dequeue_one(slot)) {
        out.push_back(slot);
        ++n;
    }
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// Partition routing: each PriorityClass lands in its own partition.
// ---------------------------------------------------------------------------
COACT_TEST(staging_partition_routing)
{
    MpscStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});

    CHECK(s.enqueue(coact::TargetId(1U), seq_event(0), coact::PriorityClass::High, 0U));
    CHECK(s.enqueue(coact::TargetId(2U), seq_event(1), coact::PriorityClass::Normal, 0U));
    CHECK(s.enqueue(coact::TargetId(3U), seq_event(2), coact::PriorityClass::Low, 0U));

    CHECK_EQ(s.size(coact::Partition::High), 1U);
    CHECK_EQ(s.size(coact::Partition::Normal), 1U);
    CHECK_EQ(s.size(coact::Partition::Low), 1U);

    // priority-first order drains High, then Normal, then Low
    std::vector<coact::StagingSlot> out;
    REQUIRE_EQ(drain_all(s, out), 3);
    CHECK_EQ(out[0].event, static_events + 0);
    CHECK_EQ(out[1].event, static_events + 1);
    CHECK_EQ(out[2].event, static_events + 2);
    CHECK_EQ(out[0].target, coact::TargetId(1U));
    CHECK_EQ(out[1].target, coact::TargetId(2U));
    CHECK_EQ(out[2].target, coact::TargetId(3U));
    CHECK_EQ(s.size(coact::Partition::High), 0U);
    CHECK_EQ(s.size(coact::Partition::Normal), 0U);
    CHECK_EQ(s.size(coact::Partition::Low), 0U);
}

// ---------------------------------------------------------------------------
// Full partitions return false without losing the reference (a full Low
// leaves already-buffered slots untouched and its Event* ungced).
// ---------------------------------------------------------------------------
COACT_TEST(staging_full_returns_false)
{
    MpscStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});

    for (uint16_t i = 0U; i < TestConfig::kHighCapacity; ++i) {
        CHECK(s.enqueue(coact::TargetId(1U), seq_event(static_cast<uint8_t>(i)),
                       coact::PriorityClass::Normal, 0U));
    }
    CHECK_EQ(s.size(coact::Partition::Normal),
             static_cast<uint16_t>(TestConfig::kNormalCapacity));

    // the partition is full: the next enqueue returns false
    CHECK(!s.enqueue(coact::TargetId(1U), seq_event(9), coact::PriorityClass::Normal, 0U));
    // no write happened and no reference was consumed
    CHECK_EQ(s.size(coact::Partition::Normal),
             static_cast<uint16_t>(TestConfig::kNormalCapacity));

    // still all servable after the rejected enqueue
    std::vector<coact::StagingSlot> out;
    REQUIRE_EQ(drain_all(s, out),
               static_cast<int>(TestConfig::kNormalCapacity));
}

// ---------------------------------------------------------------------------
// Priority order: High beats Normal beats Low when no aging applies.
// ---------------------------------------------------------------------------
COACT_TEST(staging_high_priority_first)
{
    MpscStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});

    s.enqueue(coact::TargetId(1U), seq_event(10), coact::PriorityClass::Low, 0U);
    s.enqueue(coact::TargetId(1U), seq_event(11), coact::PriorityClass::Normal, 0U);
    s.enqueue(coact::TargetId(1U), seq_event(12), coact::PriorityClass::High, 0U);

    std::vector<coact::StagingSlot> out;
    REQUIRE_EQ(drain_all(s, out), 3);
    CHECK_EQ(out[0].event->signal, 12U);   // High first
    CHECK_EQ(out[1].event->signal, 11U);   // then Normal
    CHECK_EQ(out[2].event->signal, 10U);   // then Low
}

// A reserved High head must not block a published Normal head. Queue size is
// admission/watermark state; dispatch selection must use published readiness.
COACT_TEST(staging_skips_unpublished_high_for_ready_normal)
{
    ReservationGapStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});
    CHECK(s.enqueue(coact::TargetId(1U), seq_event(13), coact::PriorityClass::High, 0U));
    CHECK(s.enqueue(coact::TargetId(2U), seq_event(14), coact::PriorityClass::Normal, 0U));

    coact::StagingSlot slot{};
    s.begin_batch();
    CHECK(s.dequeue_one(slot));
    CHECK_EQ(slot.event->signal, 14U);
}

// ---------------------------------------------------------------------------
// Low aging exception: a Low head that waited past LowMaxWaitMs is force-served
// even while higher-priority events are buffered. After the aged event is
// served, priority order resumes.
// ---------------------------------------------------------------------------
COACT_TEST(staging_low_aging_exception)
{
    MpscStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});
    const uint64_t wait_ns =
        static_cast<uint64_t>(TestConfig::kLowMaxWaitMs) * 1000000ULL;

    // buffer a Low head at t0, then a High and a Normal at t0
    s.enqueue(coact::TargetId(3U), seq_event(20), coact::PriorityClass::Low, 0U);
    s.enqueue(coact::TargetId(1U), seq_event(21), coact::PriorityClass::High, 0U);
    s.enqueue(coact::TargetId(2U), seq_event(22), coact::PriorityClass::Normal, 0U);

    coact::StagingSlot slot;

    // before aging (now - t0 < wait): priority order serves High
    s.begin_batch();
    s.tick(0U);
    CHECK(s.dequeue_one(slot));
    CHECK_EQ(slot.event->signal, 21U);

    // after the Low head aged past the budget: forced Low, despite High queued
    s.begin_batch();
    s.tick(wait_ns + 1U);
    CHECK(s.dequeue_one(slot));
    CHECK_EQ(slot.event->signal, 20U);

    // once the aged Low is drained, priority order resumes (High before Normal)
    s.begin_batch();
    s.tick(wait_ns + 1U);
    CHECK(s.dequeue_one(slot));
    CHECK_EQ(slot.event->signal, 22U);
}

// ---------------------------------------------------------------------------
// Aging never preempts when Low still waits within budget on an empty High:
// Normal is served, then Low.
// ---------------------------------------------------------------------------
COACT_TEST(staging_low_aging_within_budget_no_force)
{
    MpscStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});

    s.enqueue(coact::TargetId(3U), seq_event(30), coact::PriorityClass::Low, 0U);
    s.enqueue(coact::TargetId(2U), seq_event(31), coact::PriorityClass::Normal, 0U);

    coact::StagingSlot slot;
    s.begin_batch();
    s.tick(100U);   // 100 ns < 1ms budget: not aged
    CHECK(s.dequeue_one(slot));
    CHECK_EQ(slot.event->signal, 31U);   // Normal
    CHECK(s.dequeue_one(slot));
    CHECK_EQ(slot.event->signal, 30U);   // Low after Normal
}

// ---------------------------------------------------------------------------
// Mid-batch Low arrival must NOT be force-served by unsigned underflow.
//
// The Dispatcher samples `now_ns` ONCE at batch start, then feeds that same
// value to every dequeue_one() in the batch. A Low event enqueued DURING the
// batch carries enqueue_ns LATER than the batch-start now; aging computes
// (batch_start_now - late_arrival) and underflows, spuriously aging the Low
// head and force-serving it ahead of higher-priority events that were queued
// first. Regression for the coact dispatcher underflow gap (T3 finding).
// ---------------------------------------------------------------------------
COACT_TEST(staging_low_late_arrival_no_underflow_force_serve)
{
    MpscStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});

    // Batch-start now = 1000. An older High arrives at 1000 (before batch start).
    s.enqueue(coact::TargetId(1U), seq_event(40), coact::PriorityClass::High, 1000U);
    s.begin_batch();
    s.tick(1000U);

    // Mid-batch: a Low arrives with enqueue_ns = 5000, LATER than the
    // batch-start now (1000). This can happen when a producer enqueues while
    // the Dispatcher is already draining the batch.
    CHECK(s.enqueue(coact::TargetId(3U), seq_event(41), coact::PriorityClass::Low, 5000U));

    coact::StagingSlot slot;
    // The Low has only just arrived relative to real time; it is NOT aged.
    // Underflow would make batch_now - arrival wrap to a huge value and
    // force-serve Low ahead of the earlier High. Correct behavior: serve High.
    CHECK(s.dequeue_one(slot));
    CHECK_EQ(slot.event->signal, 40U);
}

// A Low producer can publish its slot before Staging::enqueue() records the
// side-band low_head_arrival_ns_. During that window, aging must use the
// published slot's enqueue_ns: a newly published Low must not preempt High.
COACT_TEST(staging_low_aging_uses_published_head_timestamp)
{
    g_published_low_gap.low_published.store(false, std::memory_order_relaxed);
    g_published_low_gap.release_producer.store(false, std::memory_order_relaxed);

    PublishedLowGapStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});
    std::thread low_producer([&s]() {
        CHECK(s.enqueue(coact::TargetId(3U), seq_event(50), coact::PriorityClass::Low,
                        1000000U));
    });

    while (!g_published_low_gap.low_published.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    CHECK(s.enqueue(coact::TargetId(1U), seq_event(51), coact::PriorityClass::High,
                    1000000U));

    coact::StagingSlot slot{};
    s.begin_batch();
    CHECK(s.dequeue_one(slot, 1000020U));
    CHECK_EQ(slot.event->signal, 51U);

    g_published_low_gap.release_producer.store(true, std::memory_order_release);
    low_producer.join();
}

COACT_TEST(staging_batch_size_max)
{
    BatchStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});

    for (uint16_t i = 0U; i < 8U; ++i) {
        CHECK(s.enqueue(coact::TargetId(1U), seq_event(static_cast<uint8_t>(i)),
                       coact::PriorityClass::Normal, 0U));
    }

    coact::StagingSlot slot;
    s.begin_batch();
    uint8_t first = 0U;
    while (s.dequeue_one(slot) && first < 20U) {
        ++first;
    }
    // exactly BatchSizeMax slots served in a batch, then dequeue reports full
    CHECK_EQ(first, BatchCfg::kBatchSizeMax);
    CHECK_EQ(s.batch_used(), BatchCfg::kBatchSizeMax);
    CHECK(!s.dequeue_one(slot));   // batch is full, even with slots left
    CHECK_EQ(s.size(coact::Partition::Normal),
             static_cast<uint16_t>(BatchCfg::kNormalCapacity)
             - static_cast<uint16_t>(BatchCfg::kBatchSizeMax));

    // a new batch resumes draining
    s.begin_batch();
    CHECK(s.dequeue_one(slot));
    CHECK_EQ(s.batch_used(), 1U);
}

// ---------------------------------------------------------------------------
// Watermark bands: 0% empty, 25%, 50%, 75%, 100% full, capped at 100.
// ---------------------------------------------------------------------------
COACT_TEST(staging_watermark_bands)
{
    MpscStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});

    CHECK_EQ(s.watermark(coact::Partition::Normal), 0U);

    CHECK(s.enqueue(coact::TargetId(1U), seq_event(0), coact::PriorityClass::Normal, 0U));
    CHECK_EQ(s.watermark(coact::Partition::Normal), 25U);   // 1/4

    CHECK(s.enqueue(coact::TargetId(1U), seq_event(1), coact::PriorityClass::Normal, 0U));
    CHECK_EQ(s.watermark(coact::Partition::Normal), 50U);   // 2/4

    CHECK(s.enqueue(coact::TargetId(1U), seq_event(2), coact::PriorityClass::Normal, 0U));
    CHECK_EQ(s.watermark(coact::Partition::Normal), 75U);   // 3/4

    CHECK(s.enqueue(coact::TargetId(1U), seq_event(3), coact::PriorityClass::Normal, 0U));
    CHECK_EQ(s.watermark(coact::Partition::Normal), 100U);  // 4/4

    // the other partitions remain independent
    CHECK_EQ(s.watermark(coact::Partition::High), 0U);
    CHECK_EQ(s.watermark(coact::Partition::Low), 0U);
}

// ---------------------------------------------------------------------------
// Non-full load that cannot change the reference count: staging stores the
// pointer as-is (ref_ctr stays whatever the producer left), and a consumer
// drains then event_gc exactly once - proving staging neither incs nor gcs.
// ---------------------------------------------------------------------------
COACT_TEST(staging_never_changes_refcount)
{
    constexpr std::uint16_t kCap = 4U;
    alignas(64) std::uint8_t storage[16U * 4U];
    coact::EventPool<16U, kCap> pool;
    pool.init(storage, sizeof(storage));

    coact::Event* e = pool.alloc(0x1234U);
    REQUIRE(e != nullptr);
    REQUIRE_EQ(e->ref_ctr, 1U);

    MpscStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});
    REQUIRE(s.enqueue(coact::TargetId(9U), e, coact::PriorityClass::High, 0U));

    // enqueue must not have inc'ed the event
    CHECK_EQ(e->ref_ctr, 1U);

    coact::StagingSlot slot;
    s.begin_batch();
    REQUIRE(s.dequeue_one(slot));
    // staging hands the same pointer back without touching the count
    CHECK_EQ(slot.event, e);
    CHECK_EQ(slot.target, coact::TargetId(9U));
    CHECK_EQ(e->ref_ctr, 1U);

    // the consumer releases the allocation reference it received.
    CHECK_EQ(e->ref_ctr, 1U);
    coact::event_gc(e);
    // after gc the block is back on the free list (memory reused as link), so
    // only the pool state remains observable: exactly one recycle, no leak
    CHECK_EQ(pool.used(), 0U);
}

// ---------------------------------------------------------------------------
// dequeue_one on a completely empty staging returns false.
// ---------------------------------------------------------------------------
COACT_TEST(staging_dequeue_empty_false)
{
    MpscStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});
    coact::StagingSlot slot;
    s.begin_batch();
    CHECK(!s.dequeue_one(slot));
    CHECK_EQ(s.size(coact::Partition::High), 0U);
    CHECK_EQ(s.size(coact::Partition::Normal), 0U);
    CHECK_EQ(s.size(coact::Partition::Low), 0U);
}

// ---------------------------------------------------------------------------
// Single-core ring backend: same semantics through the injected critical
// section.
// ---------------------------------------------------------------------------
COACT_TEST(staging_ring_backend)
{
    g_cs.saves = 0;
    g_cs.restores = 0;
    RingStaging s(counting_cs());

    for (uint16_t i = 0U; i < TestConfig::kLowCapacity; ++i) {
        CHECK(s.enqueue(coact::TargetId(1U), seq_event(static_cast<uint8_t>(i + 40U)),
                       coact::PriorityClass::Low, 0U));
    }
    CHECK(!s.enqueue(coact::TargetId(1U), seq_event(60), coact::PriorityClass::Low, 0U));
    CHECK_EQ(s.size(coact::Partition::Low),
             static_cast<uint16_t>(TestConfig::kLowCapacity));
    CHECK_EQ(g_cs.saves, g_cs.restores);   // balanced save/restore

    std::vector<coact::StagingSlot> out;
    REQUIRE_EQ(drain_all(s, out),
               static_cast<int>(TestConfig::kLowCapacity));
    for (uint16_t i = 0U; i < TestConfig::kLowCapacity; ++i) {
        CHECK_EQ(out[i].event->signal,
                 static_cast<uint16_t>(i + 40U));
    }
}

// ---------------------------------------------------------------------------
// Concurrent multi-producer enqueue must not lose a slot: every pushed
// sequence comes back exactly once. Distinct sequences are limited to <256 so
// the uint8_t signal tag stays unambiguous; producers block-retry on a full
// partition, so in-flight slots never exceed the Normal capacity while the
// total distinct count only needs to fit the drain sequence.
// ---------------------------------------------------------------------------
COACT_TEST(staging_concurrent_no_loss)
{
    constexpr int kProducers = 3;
    constexpr int kPerProducer = 80;          // 240 distinct tags, fits uint8_t
    constexpr int kSlots = kProducers * kPerProducer;

    MpscStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});
    std::atomic<int> done{0};
    std::vector<std::thread> threads;
    std::vector<unsigned char> seen(static_cast<size_t>(kSlots), 0U);

    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&s, p, &done, kPerProducer]() {
            const int base = p * kPerProducer;
            for (int i = 0; i < kPerProducer; ++i) {
                while (!s.enqueue(coact::TargetId(1U), seq_event(static_cast<uint8_t>(base + i)),
                                  coact::PriorityClass::Normal, 0U)) {
                    std::this_thread::yield();
                }
            }
            done.fetch_add(1, std::memory_order_release);
        });
    }

    std::vector<coact::StagingSlot> out;
    out.reserve(static_cast<size_t>(kSlots));
    coact::StagingSlot slot;
    s.begin_batch();
    s.tick(0U);
    for (;;) {
        if (s.dequeue_one(slot)) {
            out.push_back(slot);
            if (out.size() == static_cast<size_t>(kSlots)) {
                break;
            }
            continue;
        }
        // a false return means either an empty partition set or a full batch;
        // reset the batch counter so a lingering batch limit cannot hide work
        s.begin_batch();
        if (done.load(std::memory_order_acquire) < kProducers) {
            std::this_thread::yield();
            continue;
        }
        // all producers have finished publishing: one more call after the
        // batch reset confirms the partitions are genuinely drained
        if (!s.dequeue_one(slot)) {
            break;
        }
        out.push_back(slot);
        if (out.size() == static_cast<size_t>(kSlots)) {
            break;
        }
    }

    for (std::thread& t : threads) {
        t.join();
    }

    CHECK_EQ(static_cast<int>(out.size()), kSlots);
    int dup = 0;
    int oob = 0;
    for (const coact::StagingSlot& v : out) {
        const unsigned int sig = v.event->signal;
        if (sig >= static_cast<unsigned int>(kSlots)) {
            ++oob;
            continue;
        }
        unsigned char& mark = seen[sig];
        if (mark != 0U) {
            ++dup;
        }
        mark = 1U;
    }
    int missing = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (seen[static_cast<size_t>(i)] == 0U) {
            ++missing;
        }
    }
    CHECK_EQ(dup, 0);
    CHECK_EQ(oob, 0);
    CHECK_EQ(missing, 0);
}

// ---------------------------------------------------------------------------
// High critical reserve: ordinary High submissions are capped at
// kHighCapacity - kHighCriticalReserve even while a physical cell is still
// free, so one cell always stays reachable by a critical High event. The
// reserve is a claim on capacity, not a fenced region, so once that critical
// event takes the last cell the partition is genuinely full for everyone.
// ---------------------------------------------------------------------------
COACT_TEST(staging_high_critical_reserve_holds_a_cell)
{
    ReserveStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});
    const auto kOrdinaryLimit =
        ReserveCfg::kHighCapacity - ReserveCfg::kHighCriticalReserve;

    for (auto i = 0; i < kOrdinaryLimit; ++i) {
        CHECK(s.enqueue(coact::TargetId(1U), seq_event(static_cast<uint8_t>(i)),
                        coact::PriorityClass::High, 0U));
    }
    CHECK_EQ(s.size(coact::Partition::High),
             static_cast<uint16_t>(kOrdinaryLimit));

    // ordinary traffic is capped one cell below physical capacity
    CHECK(!s.enqueue(coact::TargetId(1U), seq_event(70),
                     coact::PriorityClass::High, 0U));
    CHECK_EQ(s.size(coact::Partition::High),
             static_cast<uint16_t>(kOrdinaryLimit));

    // a critical High event is the only path to the held-back cell
    CHECK(s.enqueue(coact::TargetId(1U), seq_event(71),
                    coact::PriorityClass::High, 0U, /*critical=*/true,
                    coact::StagingAdmission::Ordinary));
    CHECK_EQ(s.size(coact::Partition::High),
             static_cast<uint16_t>(ReserveCfg::kHighCapacity));

    // the reserve did not add capacity: the partition is now truly full
    CHECK(!s.enqueue(coact::TargetId(1U), seq_event(72),
                     coact::PriorityClass::High, 0U, /*critical=*/true,
                     coact::StagingAdmission::Ordinary));
}

// ---------------------------------------------------------------------------
// The claim must be returned when the slot is popped, not merely when it is
// admitted. Repeat fill/drain rounds: if dequeue_one leaked the claim, the
// High counter would stay saturated and the very first ordinary submit of
// round 2 would fail. Passing all rounds pins the release path.
// ---------------------------------------------------------------------------
COACT_TEST(staging_reservation_released_on_pop)
{
    ReserveStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});
    const auto kOrdinaryLimit =
        ReserveCfg::kHighCapacity - ReserveCfg::kHighCriticalReserve;
    std::vector<coact::StagingSlot> out;

    for (int round = 0; round < 3; ++round) {
        for (auto i = 0; i < kOrdinaryLimit; ++i) {
            CHECK(s.enqueue(coact::TargetId(1U),
                            seq_event(static_cast<uint8_t>(round * 8 + i)),
                            coact::PriorityClass::High, 0U));
        }
        CHECK(!s.enqueue(coact::TargetId(1U),
                         seq_event(static_cast<uint8_t>(round * 8 + 7)),
                         coact::PriorityClass::High, 0U));
        REQUIRE_EQ(drain_all(s, out), static_cast<int>(kOrdinaryLimit));
        CHECK_EQ(s.size(coact::Partition::High), 0U);
    }
}

// ---------------------------------------------------------------------------
// The Normal reserved lane is a separate, bounded claim counter: ordinary
// Normal traffic cannot dip into it, and ReservedNormal cannot exceed it. Both
// limits together sum to kNormalCapacity, so the reserved submissions fill the
// partition exactly and one more is refused.
// ---------------------------------------------------------------------------
COACT_TEST(staging_normal_reserved_lane_is_bounded)
{
    ReserveStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});
    const auto kOrdinaryLimit =
        ReserveCfg::kNormalCapacity - ReserveCfg::kNormalReservedCapacity;

    for (auto i = 0; i < kOrdinaryLimit; ++i) {
        CHECK(s.enqueue(coact::TargetId(2U), seq_event(static_cast<uint8_t>(i)),
                        coact::PriorityClass::Normal, 0U));
    }
    // ordinary Normal stops at its own limit, leaving the lane untouched
    CHECK(!s.enqueue(coact::TargetId(2U), seq_event(80),
                     coact::PriorityClass::Normal, 0U));
    CHECK_EQ(s.size(coact::Partition::Normal),
             static_cast<uint16_t>(kOrdinaryLimit));

    // the lane admits exactly its own bound, completing the partition
    for (auto i = 0;
         i < static_cast<int>(ReserveCfg::kNormalReservedCapacity); ++i) {
        CHECK(s.enqueue(coact::TargetId(2U),
                        seq_event(static_cast<uint8_t>(81 + i)),
                        coact::PriorityClass::Normal, 0U, /*critical=*/false,
                        coact::StagingAdmission::ReservedNormal));
    }
    CHECK_EQ(s.size(coact::Partition::Normal),
             static_cast<uint16_t>(ReserveCfg::kNormalCapacity));

    // the lane is bounded: one more ReservedNormal is refused
    CHECK(!s.enqueue(coact::TargetId(2U), seq_event(83),
                     coact::PriorityClass::Normal, 0U, /*critical=*/false,
                     coact::StagingAdmission::ReservedNormal));

    std::vector<coact::StagingSlot> out;
    REQUIRE_EQ(drain_all(s, out),
               static_cast<int>(ReserveCfg::kNormalCapacity));
    CHECK_EQ(s.size(coact::Partition::Normal), 0U);
}

// ---------------------------------------------------------------------------
// Backward compatibility: a Config that declares no reservation constants must
// behave exactly as before. The SFINAE helper yields zero for both, so ordinary
// High reaches physical capacity with no cell held back, and the ReservedNormal
// lane does not exist - such a submission is rejected outright rather than
// silently consuming an ordinary cell.
// ---------------------------------------------------------------------------
COACT_TEST(staging_without_reservation_config_is_unchanged)
{
    MpscStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});

    for (uint16_t i = 0U; i < TestConfig::kHighCapacity; ++i) {
        CHECK(s.enqueue(coact::TargetId(1U), seq_event(static_cast<uint8_t>(i)),
                        coact::PriorityClass::High, 0U));
    }
    CHECK_EQ(s.size(coact::Partition::High),
             static_cast<uint16_t>(TestConfig::kHighCapacity));
    CHECK(!s.enqueue(coact::TargetId(1U), seq_event(90),
                     coact::PriorityClass::High, 0U));

    CHECK(!s.enqueue(coact::TargetId(2U), seq_event(91),
                     coact::PriorityClass::Normal, 0U, /*critical=*/false,
                     coact::StagingAdmission::ReservedNormal));
    CHECK_EQ(s.size(coact::Partition::Normal), 0U);
}

// ---------------------------------------------------------------------------
// Rollback path: a claim TAKEN and then refused by try_push must be returned.
//
// The other tests only reach the claim-exhausted path (refused at try_claim).
// This one forces the rarer ordering the claim/resident split makes possible:
// ordinary High claims are capped at kHighCapacity - kHighCriticalReserve (3),
// but critical High claims are uncapped, so critical events can fill the ring
// beyond what ordinary claims account for. Fill 2 ordinary + 2 critical High:
// ring physically FULL (4/4) while the ordinary claim counter is still only 2.
// A 3rd ordinary High then passes try_claim (2 -> 3 == limit) but fails
// try_push (no free cell), driving the `if (!queued) release_claim(claim)` path.
// ---------------------------------------------------------------------------
COACT_TEST(staging_claim_rolled_back_when_push_fails)
{
    ReserveStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});
    const auto kOrdinaryLimit =
        ReserveCfg::kHighCapacity - ReserveCfg::kHighCriticalReserve;

    // 2 ordinary High: each takes a HighOrdinary claim. Counter = 2, resident 2.
    for (auto i = 0; i < 2; ++i) {
        CHECK(s.enqueue(coact::TargetId(1U), seq_event(static_cast<uint8_t>(i)),
                        coact::PriorityClass::High, 0U));
    }
    CHECK_EQ(s.size(coact::Partition::High), 2U);

    // 2 critical High: claim None (uncapped) so both are admitted despite the
    // ordinary limit; resident climbs to 4 == kHighCapacity, ring is FULL while
    // the ordinary claim counter is still only 2.
    for (auto i = 0; i < 2; ++i) {
        CHECK(s.enqueue(coact::TargetId(1U), seq_event(static_cast<uint8_t>(10 + i)),
                        coact::PriorityClass::High, 0U, /*critical=*/true,
                        coact::StagingAdmission::Ordinary));
    }
    CHECK_EQ(s.size(coact::Partition::High),
             static_cast<uint16_t>(ReserveCfg::kHighCapacity));

    // 3rd ordinary High: try_claim succeeds (2 -> 3, 3 is the limit) but the
    // ring has no free cell, so try_push fails and enqueue returns false. The
    // rollback must release the just-taken claim.
    CHECK(!s.enqueue(coact::TargetId(1U), seq_event(20),
                     coact::PriorityClass::High, 0U));

    // The failed push left no phantom slot: occupancy is still exactly full.
    CHECK_EQ(s.size(coact::Partition::High),
             static_cast<uint16_t>(ReserveCfg::kHighCapacity));

    // Drain. Popping the 2 ordinary residents releases their claims; the 2
    // critical residents hold claim None and release nothing.
    std::vector<coact::StagingSlot> out;
    REQUIRE_EQ(drain_all(s, out), static_cast<int>(ReserveCfg::kHighCapacity));
    CHECK_EQ(s.size(coact::Partition::High), 0U);

    // Leak-vs-rollback: after the failed push a leaked claim would sit at 3.
    // The drain returns 2 of them, leaving 1, so a refill could admit only
    // 2 ordinary submissions before hitting limit 3. With correct rollback the
    // drain returns the counter to 0 and ALL kOrdinaryLimit (3) submissions
    // succeed. Asserting the exact success count (not a boolean) is what
    // distinguishes rolled-back from leaked-by-one.
    int admitted = 0;
    for (auto i = 0; i < kOrdinaryLimit; ++i) {
        if (s.enqueue(coact::TargetId(1U), seq_event(static_cast<uint8_t>(30 + i)),
                      coact::PriorityClass::High, 0U)) {
            ++admitted;
        }
    }
    CHECK_EQ(admitted, static_cast<int>(kOrdinaryLimit));
    CHECK_EQ(s.size(coact::Partition::High),
             static_cast<uint16_t>(kOrdinaryLimit));
}

// ---------------------------------------------------------------------------
// The reserved Normal lane bound must come from its own claim counter, not from
// the ring running out of cells. Fill ONLY the reserved lane on an EMPTY Normal
// partition so the two are distinguishable: with ordinary(2)+reserved(2) both
// summing to kNormalCapacity, staging_normal_reserved_lane_is_bounded cannot
// tell them apart. Here the ring still has kNormalCapacity - kNormalReserved
// free cells, and the strict size inequality below is asserted explicitly: if
// the bound were widened to kNormalCapacity this test fails at the refused
// enqueue (the extra ReservedNormal would land in a free cell), whereas the
// existing test would still pass because its ring is already physically full.
// ---------------------------------------------------------------------------
COACT_TEST(staging_normal_reserved_lane_bound_is_not_ring_full)
{
    ReserveStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});

    for (unsigned i = 0U; i < ReserveCfg::kNormalReservedCapacity; ++i) {
        CHECK(s.enqueue(coact::TargetId(2U), seq_event(static_cast<uint8_t>(100U + i)),
                        coact::PriorityClass::Normal, 0U, /*critical=*/false,
                        coact::StagingAdmission::ReservedNormal));
    }
    CHECK_EQ(s.size(coact::Partition::Normal),
             static_cast<uint16_t>(ReserveCfg::kNormalReservedCapacity));

    // The lane is exhausted while the partition is demonstrably NOT full.
    CHECK(s.size(coact::Partition::Normal) < ReserveCfg::kNormalCapacity);
    CHECK(!s.enqueue(coact::TargetId(2U), seq_event(120U),
                     coact::PriorityClass::Normal, 0U, /*critical=*/false,
                     coact::StagingAdmission::ReservedNormal));
    CHECK(s.size(coact::Partition::Normal) < ReserveCfg::kNormalCapacity);
}

// ---------------------------------------------------------------------------
// Concurrent claim machinery. N producers mix ordinary and critical High
// submissions against ReserveStaging while one consumer drains, so the atomic
// claim counters, the rollback on a failed try_push and the release on pop all
// run under contention. Every enqueue is retried until it succeeds, so the
// successful-enqueue count and the popped count must match exactly; a leaked or
// double-released claim either trips release_claim_counter's assert (the test
// aborts, non-zero exit) or leaves the ordinary counter short of zero, which
// the post-drain refill detects. Deterministic: an atomic start gate, bounded
// per-producer work, no sleeps, and count-based invariants only.
// ---------------------------------------------------------------------------
COACT_TEST(staging_reserved_claims_hold_under_concurrency)
{
    constexpr int kProducers = 3;
    constexpr int kPerProducer = 80;                    // 240 distinct tags, fits uint8_t
    constexpr int kTotal = kProducers * kPerProducer;   // even -> equal mix
    constexpr int kOrdinaryLimit =
        static_cast<int>(ReserveCfg::kHighCapacity - ReserveCfg::kHighCriticalReserve);

    ReserveStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});
    std::atomic<int> done{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;

    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&s, &done, &start, p, kPerProducer]() {
            const int base = p * kPerProducer;   // disjoint tag range per producer
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kPerProducer; ++i) {
                const bool critical = (0 != (i & 1));
                // Retry until admitted: an ordinary claim refused by the bound,
                // or a try_push refused because a critical event filled the
                // last cell, must be retried and must not leak its claim.
                while (!s.enqueue(coact::TargetId(1U),
                                  seq_event(static_cast<uint8_t>(base + i)),
                                  coact::PriorityClass::High, 0U, critical,
                                  coact::StagingAdmission::Ordinary)) {
                    std::this_thread::yield();
                }
            }
            done.fetch_add(1, std::memory_order_release);
        });
    }

    s.begin_batch();
    s.tick(0U);
    start.store(true, std::memory_order_release);

    int popped = 0;
    bool producers_done = false;
    coact::StagingSlot slot;
    for (;;) {
        if (s.dequeue_one(slot)) {
            ++popped;
            s.begin_batch();
            continue;
        }
        s.begin_batch();
        if (!producers_done) {
            if (done.load(std::memory_order_acquire) == kProducers) {
                producers_done = true;
            }
            else {
                std::this_thread::yield();
                continue;
            }
        }
        if (!s.dequeue_one(slot)) {
            break;   // producers done and a full drain pass found nothing
        }
        ++popped;
        s.begin_batch();
    }
    for (std::thread& t : threads) {
        t.join();
    }

    CHECK_EQ(popped, kTotal);
    CHECK_EQ(s.size(coact::Partition::High), 0U);

    // The drain returned every ordinary claim: the empty partition must accept
    // exactly kOrdinaryLimit fresh ordinary submissions. A single leaked claim
    // leaves the counter at the limit and admits only kOrdinaryLimit - 1.
    int admitted = 0;
    for (int i = 0; i < kTotal; ++i) {
        if (s.enqueue(coact::TargetId(1U), seq_event(static_cast<uint8_t>(i)),
                      coact::PriorityClass::High, 0U)) {
            ++admitted;
        }
    }
    CHECK_EQ(admitted, kOrdinaryLimit);
}

// A Config declaring only ONE of the two reservation constants must have that
// one honoured. If the probe demanded both names, the declared reserve would be
// silently zeroed and ordinary traffic would fill the whole partition - so the
// observable cap below is the proof that the probe is per-constant.
COACT_TEST(staging_single_declared_reserve_is_honoured)
{
    HighOnlyReserveStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});
    const auto kOrdinaryLimit = HighOnlyReserveCfg::kHighCapacity
                              - HighOnlyReserveCfg::kHighCriticalReserve;   // 2

    for (uint16_t i = 0U; i < kOrdinaryLimit; ++i) {
        REQUIRE(s.enqueue(coact::TargetId(1U), seq_event(static_cast<uint8_t>(i)),
                          coact::PriorityClass::High, 0U));
    }
    // Ordinary traffic stops at capacity minus the declared reserve...
    CHECK(!s.enqueue(coact::TargetId(1U), seq_event(30U),
                     coact::PriorityClass::High, 0U));
    // ...while those two cells stay reachable for critical traffic.
    CHECK(s.enqueue(coact::TargetId(1U), seq_event(31U),
                    coact::PriorityClass::High, 0U, /*critical=*/true,
                    coact::StagingAdmission::Ordinary));
    CHECK(s.enqueue(coact::TargetId(1U), seq_event(32U),
                    coact::PriorityClass::High, 0U, /*critical=*/true,
                    coact::StagingAdmission::Ordinary));
    CHECK_EQ(s.size(coact::Partition::High), HighOnlyReserveCfg::kHighCapacity);
    CHECK(!s.enqueue(coact::TargetId(1U), seq_event(33U),
                     coact::PriorityClass::High, 0U, /*critical=*/true,
                     coact::StagingAdmission::Ordinary));

    // The Normal lane was never declared, so it must have no capacity at all.
    CHECK(!s.enqueue(coact::TargetId(2U), seq_event(34U),
                     coact::PriorityClass::Normal, 0U, /*critical=*/false,
                     coact::StagingAdmission::ReservedNormal));

    std::vector<coact::StagingSlot> out;
    REQUIRE_EQ(drain_all(s, out), HighOnlyReserveCfg::kHighCapacity);
}

COACT_TEST_MAIN()
