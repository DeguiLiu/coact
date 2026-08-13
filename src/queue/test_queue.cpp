// coact queue backend host tests.
// SPDX-License-Identifier: MIT
#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "coact/queue.hpp"
#include "test_harness.hpp"

namespace {

// ---------------------------------------------------------------------------
// Observable critical-section hooks used to verify that SingleCoreCriticalRing
// wraps every push/pop/size in a balanced save/restore pair with no nesting.
// ---------------------------------------------------------------------------
struct CsCounters {
    int saves = 0;
    int restores = 0;
    int depth = 0;
    int max_depth = 0;
};

CsCounters g_cs;

uintptr_t cs_save(void*) {
    g_cs.saves++;
    g_cs.depth++;
    if (g_cs.depth > g_cs.max_depth) {
        g_cs.max_depth = g_cs.depth;
    }
    return 0x1234ABCDu;
}

void cs_restore(void*, uintptr_t) {
    g_cs.restores++;
    g_cs.depth--;
}

coact::CriticalSection counting_cs() {
    coact::CriticalSection cs;
    cs.save = &cs_save;
    cs.restore = &cs_restore;
    return cs;
}

void reset_counters() {
    g_cs.saves = 0;
    g_cs.restores = 0;
    g_cs.depth = 0;
    g_cs.max_depth = 0;
}

// Non-default-constructible, move-only payload type.
struct MoveOnly {
    int value;
    explicit MoveOnly(int v) : value(v) {}
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;
};

// Move-only handle that visibly invalidates the source on move, so a failed
// push can prove the incoming object was NOT consumed while a successful push
// consumed it (std::exchange leaves owns==false and value==0 in the source).
struct MoveHandle {
    uint32_t value = 0U;
    bool owns = false;   // false == invalid (default or moved-from)

    MoveHandle() noexcept = default;
    explicit MoveHandle(uint32_t v) noexcept : value(v), owns(true) {}
    MoveHandle(MoveHandle&& o) noexcept
        : value(std::exchange(o.value, 0U)),
          owns(std::exchange(o.owns, false)) {}
    MoveHandle& operator=(MoveHandle&& o) noexcept
    {
        value = std::exchange(o.value, 0U);
        owns = std::exchange(o.owns, false);
        return *this;
    }
    MoveHandle(const MoveHandle&) = delete;
    MoveHandle& operator=(const MoveHandle&) = delete;
};

// ---------------------------------------------------------------------------
// MPSC stress driver. Each producer pushes a contiguous block of unique ids;
// the single consumer verifies total count, per-producer FIFO order, and that
// every id is popped exactly once (no loss, no duplication).
// ---------------------------------------------------------------------------
template <uint16_t Capacity>
void run_mpsc_stress(int producer_count, int items_per_producer, int rounds) {
    const int total = producer_count * items_per_producer;
    for (int r = 0; r < rounds; ++r) {
        coact::BoundedMpscQueue<int, Capacity> q;
        std::atomic<int> producers_done{0};
        std::vector<std::thread> producers;
        producers.reserve(static_cast<size_t>(producer_count));

        for (int p = 0; p < producer_count; ++p) {
            producers.emplace_back([p, items_per_producer, &q, &producers_done]() {
                const int base = p * items_per_producer;
                for (int i = 0; i < items_per_producer; ++i) {
                    while (!q.try_push(base + i)) {
                        std::this_thread::yield();
                    }
                }
                producers_done.fetch_add(1, std::memory_order_release);
            });
        }

        std::vector<int> last_per_producer(static_cast<size_t>(producer_count), -1);
        std::vector<unsigned char> seen(static_cast<size_t>(total), 0);
        int popped = 0;
        int violations = 0;

        for (;;) {
            int v = 0;
            if (q.try_pop(v)) {
                ++popped;
                if (v < 0 || v >= total || seen[static_cast<size_t>(v)] != 0U) {
                    ++violations;   // out of range or duplicate
                    continue;
                }
                seen[static_cast<size_t>(v)] = 1U;
                const int p = v / items_per_producer;
                const int i = v % items_per_producer;
                if (i != last_per_producer[static_cast<size_t>(p)] + 1) {
                    ++violations;   // per-producer FIFO broken
                }
                last_per_producer[static_cast<size_t>(p)] = i;
            }
            else if (producers_done.load(std::memory_order_acquire) == producer_count) {
                break;
            }
            else {
                std::this_thread::yield();
            }
        }

        int v = 0;
        while (q.try_pop(v)) {
            ++popped;
            if (v < 0 || v >= total || seen[static_cast<size_t>(v)] != 0U) {
                ++violations;
                continue;
            }
            seen[static_cast<size_t>(v)] = 1U;
            const int p = v / items_per_producer;
            const int i = v % items_per_producer;
            if (i != last_per_producer[static_cast<size_t>(p)] + 1) {
                ++violations;
            }
            last_per_producer[static_cast<size_t>(p)] = i;
        }

        for (std::thread& t : producers) {
            t.join();
        }

        CHECK_EQ(popped, total);
        CHECK_EQ(violations, 0);
        CHECK_EQ(q.size(), 0);
        int missing = 0;
        for (int i = 0; i < total; ++i) {
            if (seen[static_cast<size_t>(i)] == 0U) {
                ++missing;
            }
        }
        CHECK_EQ(missing, 0);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// BoundedMpscQueue: single-thread FIFO, full/empty, size bookkeeping.
// ---------------------------------------------------------------------------
COACT_TEST(mpsc_accepts_unified_critical_section_constructor) {
    coact::BoundedMpscQueue<uint32_t, 2U> q(counting_cs());
    CHECK(q.try_push(11U));

    uint32_t value = 0U;
    CHECK(q.try_pop(value));
    CHECK_EQ(value, 11U);
}

COACT_TEST(mpsc_fifo_single_thread) {
    coact::BoundedMpscQueue<int, 8> q;
    REQUIRE_EQ(q.capacity(), 8);
    REQUIRE_EQ(q.size(), 0);

    for (int i = 0; i < 8; ++i) {
        CHECK(q.try_push(i));
    }
    CHECK_EQ(q.size(), 8);
    CHECK(!q.try_push(99));   // full

    for (int i = 0; i < 8; ++i) {
        int v = -1;
        CHECK(q.try_pop(v));
        CHECK_EQ(v, i);
    }
    CHECK_EQ(q.size(), 0);
    int v = -1;
    CHECK(!q.try_pop(v));   // empty

    // refill after full drain
    CHECK(q.try_push(100));
    CHECK(q.try_pop(v));
    CHECK_EQ(v, 100);
    CHECK_EQ(q.size(), 0);
}

// ---------------------------------------------------------------------------
// BoundedMpscQueue: capacity-1 boundary (dedicated 3-state slot path).
// ---------------------------------------------------------------------------
COACT_TEST(mpsc_capacity_one) {
    coact::BoundedMpscQueue<int, 1> q;
    REQUIRE_EQ(q.capacity(), 1);
    REQUIRE_EQ(q.size(), 0);

    CHECK(q.try_push(11));
    CHECK_EQ(q.size(), 1);
    CHECK(!q.try_push(12));   // full

    int v = 0;
    CHECK(q.try_pop(v));
    CHECK_EQ(v, 11);
    CHECK_EQ(q.size(), 0);
    CHECK(!q.try_pop(v));   // empty

    // slot reuse after pop
    CHECK(q.try_push(13));
    CHECK(q.try_pop(v));
    CHECK_EQ(v, 13);
}

// ---------------------------------------------------------------------------
// BoundedMpscQueue: power-of-two capacities.
// ---------------------------------------------------------------------------
COACT_TEST(mpsc_power_of_two) {
    coact::BoundedMpscQueue<int, 16> q;
    for (int i = 0; i < 16; ++i) {
        CHECK(q.try_push(i));
    }
    CHECK(!q.try_push(16));
    for (int i = 0; i < 16; ++i) {
        int v = 0;
        CHECK(q.try_pop(v));
        CHECK_EQ(v, i);
    }
    CHECK_EQ(q.size(), 0);

    coact::BoundedMpscQueue<int, 2> q2;
    CHECK(q2.try_push(1));
    CHECK(q2.try_push(2));
    CHECK(!q2.try_push(3));
    int v = 0;
    CHECK(q2.try_pop(v));
    CHECK_EQ(v, 1);
    CHECK(q2.try_push(3));   // a slot was freed
    CHECK(q2.try_pop(v));
    CHECK_EQ(v, 2);
    CHECK(q2.try_pop(v));
    CHECK_EQ(v, 3);
}

// ---------------------------------------------------------------------------
// BoundedMpscQueue: move-only, non-default-constructible payload type.
// ---------------------------------------------------------------------------
COACT_TEST(mpsc_move_only) {
    coact::BoundedMpscQueue<MoveOnly, 2> q;
    CHECK(q.try_push(MoveOnly(1)));
    CHECK(q.try_push(MoveOnly(2)));
    CHECK(!q.try_push(MoveOnly(3)));   // full
    MoveOnly out(0);
    CHECK(q.try_pop(out));
    CHECK_EQ(out.value, 1);
    CHECK(q.try_pop(out));
    CHECK_EQ(out.value, 2);
    CHECK(!q.try_pop(out));
}

// ---------------------------------------------------------------------------
// BoundedMpscQueue: concurrent MPSC stress (N producers + 1 consumer).
// ---------------------------------------------------------------------------
COACT_TEST(mpsc_concurrent_stress) {
    run_mpsc_stress<64>(4, 4000, 3);
    run_mpsc_stress<4>(4, 2000, 3);
    run_mpsc_stress<1024>(2, 6000, 2);
    run_mpsc_stress<1>(4, 2000, 3);   // single-slot path under contention
}

// ---------------------------------------------------------------------------
// SingleCoreCriticalRing: FIFO, full/empty, size and lock-wrap counting.
// ---------------------------------------------------------------------------
COACT_TEST(ring_fifo_and_lock_counting) {
    reset_counters();
    coact::SingleCoreCriticalRing<int, 4> q(counting_cs());

    for (int i = 0; i < 4; ++i) {
        CHECK(q.try_push(int(i)));
    }
    CHECK(!q.try_push(99));   // full
    CHECK_EQ(q.size(), 4);

    for (int i = 0; i < 4; ++i) {
        int v = -1;
        CHECK(q.try_pop(v));
        CHECK_EQ(v, i);
    }
    int v = -1;
    CHECK(!q.try_pop(v));   // empty
    CHECK_EQ(q.size(), 0);

    // every push/pop/size was wrapped in a balanced save/restore, no nesting
    CHECK(g_cs.saves > 0);
    CHECK_EQ(g_cs.restores, g_cs.saves);
    CHECK_EQ(g_cs.depth, 0);
    CHECK_EQ(g_cs.max_depth, 1);
}

// ---------------------------------------------------------------------------
// SingleCoreCriticalRing: capacity-1 boundary.
// ---------------------------------------------------------------------------
COACT_TEST(ring_capacity_one) {
    reset_counters();
    coact::SingleCoreCriticalRing<int, 1> q(counting_cs());

    CHECK(q.try_push(7));
    CHECK(!q.try_push(8));   // full
    int v = 0;
    CHECK(q.try_pop(v));
    CHECK_EQ(v, 7);
    CHECK(q.try_push(9));    // reuse
    CHECK(q.try_pop(v));
    CHECK_EQ(v, 9);
    CHECK_EQ(q.size(), 0);
}

// ---------------------------------------------------------------------------
// SingleCoreCriticalRing: move-only payload type.
// ---------------------------------------------------------------------------
COACT_TEST(ring_move_only) {
    reset_counters();
    coact::SingleCoreCriticalRing<MoveOnly, 2> q(counting_cs());
    CHECK(q.try_push(MoveOnly(5)));
    MoveOnly out(0);
    CHECK(q.try_pop(out));
    CHECK_EQ(out.value, 5);
    CHECK_EQ(q.size(), 0);
}

// ---------------------------------------------------------------------------
// SingleCoreCriticalRing: fused try_push_observed (design 5.4). Success reports
// the fill level right after the push; a full failure reports the full level
// and must NOT consume the caller's value.
// ---------------------------------------------------------------------------
COACT_TEST(ring_try_push_observed_basic) {
    reset_counters();
    coact::SingleCoreCriticalRing<int, 4> q(counting_cs());

    coact::QueueResult r = q.try_push_observed(10);
    CHECK(r.success);
    CHECK_EQ(r.size_after, 1U);

    r = q.try_push_observed(11);
    CHECK(r.success);
    CHECK_EQ(r.size_after, 2U);

    r = q.try_push_observed(12);
    CHECK(r.success);
    CHECK_EQ(r.size_after, 3U);

    r = q.try_push_observed(13);
    CHECK(r.success);
    CHECK_EQ(r.size_after, 4U);

    // full: failure with size_after == capacity
    r = q.try_push_observed(99);
    CHECK(!r.success);
    CHECK_EQ(r.size_after, 4U);

    int v = 0;
    REQUIRE(q.try_pop(v));
    CHECK_EQ(v, 10);
    r = q.try_push_observed(20);
    CHECK(r.success);
    CHECK_EQ(r.size_after, 4U);
}

// ---------------------------------------------------------------------------
// SingleCoreCriticalRing: try_push_observed must run the whole fused operation
// inside ONE critical section (one save/restore pair, no nesting). This is the
// regression guard for the cmdfw DeliveryTx double-critical-section removal.
// ---------------------------------------------------------------------------
COACT_TEST(ring_try_push_observed_single_critical_section) {
    reset_counters();
    coact::SingleCoreCriticalRing<int, 2> q(counting_cs());

    coact::QueueResult r = q.try_push_observed(1);
    CHECK(r.success);
    CHECK_EQ(r.size_after, 1U);
    CHECK_EQ(g_cs.saves, 1);
    CHECK_EQ(g_cs.restores, 1);
    CHECK_EQ(g_cs.depth, 0);
    CHECK_EQ(g_cs.max_depth, 1);

    reset_counters();
    r = q.try_push_observed(2);
    CHECK(r.success);
    CHECK_EQ(r.size_after, 2U);
    CHECK_EQ(g_cs.saves, 1);
    CHECK_EQ(g_cs.restores, 1);
    CHECK_EQ(g_cs.max_depth, 1);

    reset_counters();
    r = q.try_push_observed(3);   // full
    CHECK(!r.success);
    CHECK_EQ(r.size_after, 2U);
    CHECK_EQ(g_cs.saves, 1);
    CHECK_EQ(g_cs.restores, 1);
    CHECK_EQ(g_cs.depth, 0);
    CHECK_EQ(g_cs.max_depth, 1);
}

// ---------------------------------------------------------------------------
// SingleCoreCriticalRing: move-only failed push keeps the input valid; a later
// successful push consumes it. Proves the fused operation only moves the
// payload once capacity is known to be available.
// ---------------------------------------------------------------------------
COACT_TEST(ring_try_push_observed_failed_push_keeps_input) {
    reset_counters();
    coact::SingleCoreCriticalRing<MoveHandle, 2> q(counting_cs());
    REQUIRE(q.try_push_observed(MoveHandle(1U)).success);
    REQUIRE(q.try_push_observed(MoveHandle(2U)).success);

    MoveHandle incoming(3U);
    coact::QueueResult r = q.try_push_observed(std::move(incoming));   // full
    CHECK(!r.success);
    CHECK_EQ(r.size_after, 2U);
    CHECK(incoming.owns);                       // NOT consumed
    CHECK_EQ(incoming.value, 3U);               // value intact

    MoveHandle out;
    REQUIRE(q.try_pop(out));                    // free a slot
    CHECK(out.owns);
    CHECK_EQ(out.value, 1U);

    r = q.try_push_observed(std::move(incoming));   // now consumes
    CHECK(r.success);
    CHECK_EQ(r.size_after, 2U);
    CHECK(!incoming.owns);                      // consumed (moved-from)

    // FIFO: the pre-existing 2 is next, then the consumed 3.
    MoveHandle out2;
    REQUIRE(q.try_pop(out2));
    CHECK(out2.owns);
    CHECK_EQ(out2.value, 2U);

    MoveHandle out3;
    REQUIRE(q.try_pop(out3));
    CHECK(out3.owns);
    CHECK_EQ(out3.value, 3U);
    CHECK_EQ(q.size(), 0);
    CHECK(!q.try_pop(out3));                    // empty
}

// ---------------------------------------------------------------------------
// SingleCoreCriticalRing: capacity-1 boundary for try_push_observed.
// ---------------------------------------------------------------------------
COACT_TEST(ring_try_push_observed_capacity_one) {
    reset_counters();
    coact::SingleCoreCriticalRing<int, 1> q(counting_cs());

    coact::QueueResult r = q.try_push_observed(7);
    CHECK(r.success);
    CHECK_EQ(r.size_after, 1U);

    r = q.try_push_observed(8);   // full
    CHECK(!r.success);
    CHECK_EQ(r.size_after, 1U);

    int v = 0;
    REQUIRE(q.try_pop(v));
    CHECK_EQ(v, 7);

    r = q.try_push_observed(9);   // empty slot reused
    CHECK(r.success);
    CHECK_EQ(r.size_after, 1U);
    CHECK_EQ(q.size(), 1);
}

// ---------------------------------------------------------------------------
// SingleCoreCriticalRing: try_push_observed fill-level bookkeeping must agree
// with the plain try_push / size() path on the same ring (regression).
// ---------------------------------------------------------------------------
COACT_TEST(ring_try_push_observed_matches_plain_push) {
    reset_counters();
    coact::SingleCoreCriticalRing<int, 4> q(counting_cs());

    CHECK(q.try_push(1));
    coact::QueueResult r = q.try_push_observed(2);
    CHECK(r.success);
    CHECK_EQ(r.size_after, 2U);
    CHECK(q.try_push(3));
    CHECK_EQ(q.size(), 3);

    int v = 0;
    REQUIRE(q.try_pop(v));
    CHECK_EQ(v, 1);
    CHECK_EQ(q.size(), 2);
    r = q.try_push_observed(4);
    CHECK(r.success);
    CHECK_EQ(r.size_after, 3U);
    CHECK_EQ(q.size(), 3);
}

// ---------------------------------------------------------------------------
// SingleCoreCriticalRing: try_push_observed across write_index_ wrap-around.
// A push/pop stream longer than one full turn of the slot index must keep
// reporting correct size_after and full/empty boundaries.
// ---------------------------------------------------------------------------
COACT_TEST(ring_try_push_observed_wrap) {
    reset_counters();
    coact::SingleCoreCriticalRing<uint32_t, 4> q(counting_cs());
    constexpr uint32_t kIters = 70000U;   // forces write_index_ to wrap often

    for (uint32_t i = 0U; i < kIters; ++i) {
        coact::QueueResult r = q.try_push_observed(std::move(i));
        REQUIRE(r.success);
        CHECK_EQ(r.size_after, 1U);
        uint32_t out = 0U;
        REQUIRE(q.try_pop(out));
        CHECK_EQ(out, i);
    }
    CHECK_EQ(q.size(), 0);

    // Fill to the brim right after the wrap boundary.
    for (uint32_t i = 0U; i < 4U; ++i) {
        coact::QueueResult r = q.try_push_observed(kIters + i);
        REQUIRE(r.success);
        CHECK_EQ(r.size_after, i + 1U);
    }
    coact::QueueResult r = q.try_push_observed(0xFFFFFFFFU);
    CHECK(!r.success);
    CHECK_EQ(r.size_after, 4U);

    uint32_t out = 0U;
    for (uint32_t i = 0U; i < 4U; ++i) {
        REQUIRE(q.try_pop(out));
        CHECK_EQ(out, kIters + i);
    }
    CHECK_EQ(q.size(), 0);
}

COACT_TEST_MAIN()
