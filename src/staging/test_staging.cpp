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
    coact::StagingSlot slot;
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

    CHECK(s.enqueue(1U, seq_event(0), coact::PriorityClass::High, 0U));
    CHECK(s.enqueue(2U, seq_event(1), coact::PriorityClass::Normal, 0U));
    CHECK(s.enqueue(3U, seq_event(2), coact::PriorityClass::Low, 0U));

    CHECK_EQ(s.size(coact::Partition::High), 1U);
    CHECK_EQ(s.size(coact::Partition::Normal), 1U);
    CHECK_EQ(s.size(coact::Partition::Low), 1U);

    // priority-first order drains High, then Normal, then Low
    std::vector<coact::StagingSlot> out;
    REQUIRE_EQ(drain_all(s, out), 3);
    CHECK_EQ(out[0].event, static_events + 0);
    CHECK_EQ(out[1].event, static_events + 1);
    CHECK_EQ(out[2].event, static_events + 2);
    CHECK_EQ(out[0].target, 1U);
    CHECK_EQ(out[1].target, 2U);
    CHECK_EQ(out[2].target, 3U);
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
        CHECK(s.enqueue(1U, seq_event(static_cast<uint8_t>(i)),
                       coact::PriorityClass::Normal, 0U));
    }
    CHECK_EQ(s.size(coact::Partition::Normal),
             static_cast<uint16_t>(TestConfig::kNormalCapacity));

    // the partition is full: the next enqueue returns false
    CHECK(!s.enqueue(1U, seq_event(9), coact::PriorityClass::Normal, 0U));
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

    s.enqueue(1U, seq_event(10), coact::PriorityClass::Low, 0U);
    s.enqueue(1U, seq_event(11), coact::PriorityClass::Normal, 0U);
    s.enqueue(1U, seq_event(12), coact::PriorityClass::High, 0U);

    std::vector<coact::StagingSlot> out;
    REQUIRE_EQ(drain_all(s, out), 3);
    CHECK_EQ(out[0].event->signal, 12U);   // High first
    CHECK_EQ(out[1].event->signal, 11U);   // then Normal
    CHECK_EQ(out[2].event->signal, 10U);   // then Low
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
    s.enqueue(3U, seq_event(20), coact::PriorityClass::Low, 0U);
    s.enqueue(1U, seq_event(21), coact::PriorityClass::High, 0U);
    s.enqueue(2U, seq_event(22), coact::PriorityClass::Normal, 0U);

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

    s.enqueue(3U, seq_event(30), coact::PriorityClass::Low, 0U);
    s.enqueue(2U, seq_event(31), coact::PriorityClass::Normal, 0U);

    coact::StagingSlot slot;
    s.begin_batch();
    s.tick(100U);   // 100 ns < 1ms budget: not aged
    CHECK(s.dequeue_one(slot));
    CHECK_EQ(slot.event->signal, 31U);   // Normal
    CHECK(s.dequeue_one(slot));
    CHECK_EQ(slot.event->signal, 30U);   // Low after Normal
}

// ---------------------------------------------------------------------------
// BatchSizeMax bound: a batch never draws more than Config-derived
// BatchSizeMax slots before begin_batch() opens a new batch.
// ---------------------------------------------------------------------------
COACT_TEST(staging_batch_size_max)
{
    BatchStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});

    for (uint16_t i = 0U; i < 8U; ++i) {
        CHECK(s.enqueue(1U, seq_event(static_cast<uint8_t>(i)),
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

    CHECK(s.enqueue(1U, seq_event(0), coact::PriorityClass::Normal, 0U));
    CHECK_EQ(s.watermark(coact::Partition::Normal), 25U);   // 1/4

    CHECK(s.enqueue(1U, seq_event(1), coact::PriorityClass::Normal, 0U));
    CHECK_EQ(s.watermark(coact::Partition::Normal), 50U);   // 2/4

    CHECK(s.enqueue(1U, seq_event(2), coact::PriorityClass::Normal, 0U));
    CHECK_EQ(s.watermark(coact::Partition::Normal), 75U);   // 3/4

    CHECK(s.enqueue(1U, seq_event(3), coact::PriorityClass::Normal, 0U));
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
    REQUIRE_EQ(e->ref_ctr, 0U);

    MpscStaging s(coact::CriticalSection{nullptr, nullptr, nullptr});
    REQUIRE(s.enqueue(9U, e, coact::PriorityClass::High, 0U));

    // enqueue must not have inc'ed the event
    CHECK_EQ(e->ref_ctr, 0U);

    coact::StagingSlot slot;
    s.begin_batch();
    REQUIRE(s.dequeue_one(slot));
    // staging hands the same pointer back without touching the count
    CHECK_EQ(slot.event, e);
    CHECK_EQ(slot.target, 9U);
    CHECK_EQ(e->ref_ctr, 0U);

    // the consumer does the lifecycle: one inc for the post, one gc to recycle
    coact::event_ref_inc(e);
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
        CHECK(s.enqueue(1U, seq_event(static_cast<uint8_t>(i + 40U)),
                       coact::PriorityClass::Low, 0U));
    }
    CHECK(!s.enqueue(1U, seq_event(60), coact::PriorityClass::Low, 0U));
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
        threads.emplace_back([&s, p, &done]() {
            const int base = p * kPerProducer;
            for (int i = 0; i < kPerProducer; ++i) {
                while (!s.enqueue(1U, seq_event(static_cast<uint8_t>(base + i)),
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

COACT_TEST_MAIN()
