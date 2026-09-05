// coact SpscRing prototype host tests (design 5.2 / 5.4, stage-1 hard
// checkpoint A). TDD: these tests were written against a missing header and
// failed to compile (RED) before include/coact/spsc_ring.hpp existed.
// SPDX-License-Identifier: MIT

#include "coact/spsc_ring.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>

#include "coact/config.hpp"
#include "coact/queue.hpp"
#include "test/rtthread_stub.h"
#include "test/test_harness.hpp"

namespace {

// ---------------------------------------------------------------------------
// Move-only handle that records whether it owns a value. Failed pushes must
// leave the incoming handle valid (not consumed); successful pushes and pops
// transfer ownership (source invalidated) via std::exchange.
// ---------------------------------------------------------------------------
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

// Payload that is NOT nothrow-move-assignable: must be rejected by the trait.
struct ThrowingMove {
    ThrowingMove() noexcept = default;
    ThrowingMove(ThrowingMove&&) noexcept(false) {}
    ThrowingMove& operator=(ThrowingMove&&) noexcept(false) { return *this; }
};

// Payload that is NOT nothrow-default-constructible: must be rejected.
struct NoDefault {
    NoDefault() = delete;
    explicit NoDefault(int32_t) {}
    NoDefault(NoDefault&&) noexcept = default;
    NoDefault& operator=(NoDefault&&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Design 5.2 capacity / type constraints as compile-time traits.
// ---------------------------------------------------------------------------
COACT_TEST(spsc_compile_time_constraints)
{
    static_assert(coact::is_spsc_ring_config<uint16_t, 2>::value, "cap 2 ok");
    static_assert(coact::is_spsc_ring_config<uint16_t, 4>::value, "cap 4 ok");
    static_assert(coact::is_spsc_ring_config<uint16_t, 8>::value, "cap 8 ok");
    static_assert(coact::is_spsc_ring_config<uint16_t, 16>::value, "cap 16 ok");
    static_assert(coact::is_spsc_ring_config<uint16_t, 16384>::value, "max pow2 ok");
    static_assert(coact::is_spsc_ring_config<MoveHandle, 4>::value, "move-only ok");

    static_assert(!coact::is_spsc_ring_config<uint16_t, 0>::value, "cap 0 rejected");
    static_assert(!coact::is_spsc_ring_config<uint16_t, 1>::value, "cap 1 rejected (<2)");
    static_assert(!coact::is_spsc_ring_config<uint16_t, 3>::value, "non-pow2 rejected");
    static_assert(!coact::is_spsc_ring_config<uint16_t, 6>::value, "non-pow2 rejected");
    static_assert(!coact::is_spsc_ring_config<uint16_t, 0x8000U>::value, "over 0x7FFF rejected");
    static_assert(!coact::is_spsc_ring_config<ThrowingMove, 4>::value, "throwing move rejected");
    static_assert(!coact::is_spsc_ring_config<NoDefault, 4>::value, "non-default-ctor rejected");
    CHECK(true);
}

// ---------------------------------------------------------------------------
// Single-thread FIFO, full/empty and size bookkeeping.
// ---------------------------------------------------------------------------
COACT_TEST(spsc_fifo_single_thread)
{
    coact::SpscRing<uint16_t, 4> q;
    REQUIRE_EQ(q.capacity(), 4);
    REQUIRE_EQ(q.size(), 0);

    for (uint16_t i = 0U; i < 4U; ++i) {
        CHECK(q.try_push(std::move(i)));
    }
    CHECK_EQ(q.size(), 4);
    CHECK(!q.try_push(99U));   // full

    for (uint16_t i = 0U; i < 4U; ++i) {
        uint16_t v = 0U;
        CHECK(q.try_pop(v));
        CHECK_EQ(v, i);
    }
    CHECK_EQ(q.size(), 0);
    uint16_t v = 0U;
    CHECK(!q.try_pop(v));   // empty

    // refill after full drain
    CHECK(q.try_push(100U));
    CHECK(q.try_pop(v));
    CHECK_EQ(v, 100U);
    CHECK_EQ(q.size(), 0);
}

// ---------------------------------------------------------------------------
// Empty pop must not touch the output.
// ---------------------------------------------------------------------------
COACT_TEST(spsc_empty_pop_leaves_output)
{
    coact::SpscRing<uint16_t, 4> q;
    uint16_t out = 0xAAAAU;
    CHECK(!q.try_pop(out));
    CHECK_EQ(out, 0xAAAAU);
    CHECK_EQ(q.size(), 0);
}

// ---------------------------------------------------------------------------
// Power-of-two capacities 2 / 4 / 8, full boundary and slot reuse.
// ---------------------------------------------------------------------------
COACT_TEST(spsc_capacity_power_of_two)
{
    uint16_t v = 0U;

    coact::SpscRing<uint16_t, 2> q2;
    CHECK(q2.try_push(1U));
    CHECK(q2.try_push(2U));
    CHECK(!q2.try_push(3U));   // full at cap 2
    CHECK(q2.try_pop(v));
    CHECK_EQ(v, 1U);
    CHECK(q2.try_push(3U));    // a slot was freed
    CHECK(q2.try_pop(v));
    CHECK_EQ(v, 2U);
    CHECK(q2.try_pop(v));
    CHECK_EQ(v, 3U);
    CHECK_EQ(q2.size(), 0);

    coact::SpscRing<uint16_t, 4> q4;
    for (uint16_t i = 0U; i < 4U; ++i) {
        CHECK(q4.try_push(std::move(i)));
    }
    CHECK(!q4.try_push(9U));
    for (uint16_t i = 0U; i < 4U; ++i) {
        CHECK(q4.try_pop(v));
        CHECK_EQ(v, i);
    }
    CHECK_EQ(q4.size(), 0);

    coact::SpscRing<uint16_t, 8> q8;
    for (uint16_t i = 0U; i < 8U; ++i) {
        CHECK(q8.try_push(std::move(i)));
    }
    CHECK_EQ(q8.size(), 8);
    CHECK(!q8.try_push(99U));
    for (uint16_t i = 0U; i < 8U; ++i) {
        CHECK(q8.try_pop(v));
        CHECK_EQ(v, i);
    }
    CHECK(!q8.try_pop(v));
}

// ---------------------------------------------------------------------------
// Sequence wrap across 2^16: push+pop a stream longer than one full turn of
// the uint16 sequence, then verify full/empty logic still works after wrap.
// ---------------------------------------------------------------------------
COACT_TEST(spsc_wrap_sequence_2pow16)
{
    coact::SpscRing<uint32_t, 4> q;
    REQUIRE_EQ(q.capacity(), 4);

    constexpr uint32_t kIters = 70000U;   // > 65536, forces head and tail to wrap
    uint32_t out = 0U;
    for (uint32_t i = 0U; i < kIters; ++i) {
        REQUIRE(q.try_push(std::move(i)));
        REQUIRE(q.try_pop(out));
        CHECK_EQ(out, i);
    }
    CHECK_EQ(q.size(), 0);

    // Fill to the brim right after the wrap boundary and re-check full/empty.
    for (uint32_t i = 0U; i < 4U; ++i) {
        REQUIRE(q.try_push(kIters + i));
    }
    CHECK(!q.try_push(0xFFFFU));
    CHECK_EQ(q.size(), 4);
    for (uint32_t i = 0U; i < 4U; ++i) {
        REQUIRE(q.try_pop(out));
        CHECK_EQ(out, kIters + i);
    }
    CHECK_EQ(q.size(), 0);
    CHECK(!q.try_pop(out));
}

// ---------------------------------------------------------------------------
// Move-only payload: failed push must NOT consume the incoming handle; a later
// successful push does. Repeated push/pop ownership transfer.
// ---------------------------------------------------------------------------
COACT_TEST(spsc_move_only_failed_push_keeps_input)
{
    coact::SpscRing<MoveHandle, 2> q;
    REQUIRE(q.try_push(MoveHandle(1U)));
    REQUIRE(q.try_push(MoveHandle(2U)));

    MoveHandle incoming(3U);
    CHECK(!q.try_push(std::move(incoming)));   // full
    CHECK(incoming.owns);                       // still owns
    CHECK_EQ(incoming.value, 3U);               // value intact

    MoveHandle out;
    REQUIRE(q.try_pop(out));                    // free a slot
    CHECK(out.owns);
    CHECK_EQ(out.value, 1U);

    CHECK(q.try_push(std::move(incoming)));     // now consumes
    CHECK(!incoming.owns);

    // FIFO: the pre-existing 2 is next, then the consumed 3.
    MoveHandle out2;
    REQUIRE(q.try_pop(out2));
    CHECK(out2.owns);
    CHECK_EQ(out2.value, 2U);

    MoveHandle out3;
    REQUIRE(q.try_pop(out3));
    CHECK(out3.owns);
    CHECK_EQ(out3.value, 3U);
    CHECK(!q.try_pop(out3));                    // empty
}

COACT_TEST(spsc_repeated_move)
{
    coact::SpscRing<MoveHandle, 4> q;
    for (uint32_t round = 0U; round < 3U; ++round) {
        for (uint32_t i = 0U; i < 4U; ++i) {
            REQUIRE(q.try_push(MoveHandle(round * 100U + i)));
        }
        for (uint32_t i = 0U; i < 4U; ++i) {
            MoveHandle out;
            REQUIRE(q.try_pop(out));
            CHECK(out.owns);
            CHECK_EQ(out.value, round * 100U + i);
        }
    }
    CHECK_EQ(q.size(), 0);

    // Moved-from slot is reusable after a pop.
    REQUIRE(q.try_push(MoveHandle(7U)));
    MoveHandle out;
    REQUIRE(q.try_pop(out));
    CHECK_EQ(out.value, 7U);
    CHECK_EQ(q.size(), 0);
}

// ---------------------------------------------------------------------------
// Design 5.4: fused push + size_after.
// ---------------------------------------------------------------------------
COACT_TEST(spsc_try_push_observed)
{
    coact::SpscRing<uint16_t, 4> q;
    coact::QueueResult r = q.try_push_observed(10U);
    CHECK(r.success);
    CHECK_EQ(r.size_after, 1U);

    r = q.try_push_observed(11U);
    CHECK(r.success);
    CHECK_EQ(r.size_after, 2U);

    r = q.try_push_observed(12U);
    CHECK(r.success);
    CHECK_EQ(r.size_after, 3U);

    r = q.try_push_observed(13U);
    CHECK(r.success);
    CHECK_EQ(r.size_after, 4U);

    // full: failure with size_after == capacity
    r = q.try_push_observed(99U);
    CHECK(!r.success);
    CHECK_EQ(r.size_after, 4U);

    uint16_t v = 0U;
    REQUIRE(q.try_pop(v));
    CHECK_EQ(v, 10U);
    r = q.try_push_observed(20U);
    CHECK(r.success);
    CHECK_EQ(r.size_after, 4U);
}

// ---------------------------------------------------------------------------
// Single-core ISR/task visibility (RT-Thread host stub): an ISR-context
// producer pushes and a task-context consumer pops. The lock-free ring must
// work from ISR nesting with no irq-mask and never lose the ISR-written items.
// ---------------------------------------------------------------------------
COACT_TEST(spsc_isr_producer_task_consumer)
{
    coact::SpscRing<uint16_t, 8> q;

    stub_set_isr_nest(1U);   // ISR context
    for (uint16_t i = 0U; i < 8U; ++i) {
        REQUIRE(q.try_push(std::move(i)));   // ISR pushes
    }
    CHECK(!q.try_push(0xFFU));   // ISR push on a full ring fails cleanly
    stub_set_isr_nest(0U);       // back to task context

    uint16_t v = 0U;
    for (uint16_t i = 0U; i < 8U; ++i) {
        REQUIRE(q.try_pop(v));   // task pops; ISR-written items must be visible
        CHECK_EQ(v, i);
    }
    CHECK(!q.try_pop(v));
    CHECK_EQ(q.size(), 0);
}

// ---------------------------------------------------------------------------
// Lock-free SMP validation: one producer thread + one consumer thread, strict
// FIFO, no loss / duplication / reorder. Exercises the release/acquire pair
// under real concurrency (also the primary TSAN target).
// ---------------------------------------------------------------------------
COACT_TEST(spsc_single_producer_single_consumer_stress)
{
    constexpr uint32_t kN = 200000U;
    coact::SpscRing<uint32_t, 64> q;
    std::atomic<bool> ok{true};
    std::atomic<uint32_t> consumed{0U};

    std::thread consumer([&q, &ok, &consumed]() {
        uint32_t v = 0U;
        uint32_t expected = 0U;
        uint32_t count = 0U;
        while (count < kN) {
            if (q.try_pop(v)) {
                if (v != expected) {
                    ok.store(false, std::memory_order_relaxed);
                }
                ++expected;
                ++count;
            }
        }
        consumed.store(count, std::memory_order_relaxed);
    });

    for (uint32_t i = 0U; i < kN; ++i) {
        while (!q.try_push(std::move(i))) {
            std::this_thread::yield();
        }
    }
    consumer.join();

    CHECK(ok.load());
    CHECK_EQ(consumed.load(), kN);
    CHECK_EQ(q.size(), 0);
}

// ---------------------------------------------------------------------------
// Consumer batch drain: pop_batch transfers at most max_count elements in
// FIFO order with a single acquire load of head (single-drain discipline).
// ---------------------------------------------------------------------------
COACT_TEST(spsc_pop_batch_empty_returns_zero)
{
    coact::SpscRing<uint16_t, 4> q;
    uint16_t out[4] = {0U, 0U, 0U, 0U};
    CHECK_EQ(q.pop_batch(out, 4U), 0U);
    CHECK_EQ(q.size(), 0U);
}

COACT_TEST(spsc_pop_batch_transfers_in_order)
{
    coact::SpscRing<uint16_t, 4> q;
    for (uint16_t i = 0U; i < 3U; ++i) {
        REQUIRE(q.try_push(std::move(i)));
    }
    uint16_t out[5] = {0U, 0U, 0U, 0U, 0U};
    CHECK_EQ(q.pop_batch(out, 5U), 3U);
    CHECK_EQ(out[0], 0U);
    CHECK_EQ(out[1], 1U);
    CHECK_EQ(out[2], 2U);
    CHECK_EQ(q.size(), 0U);
    CHECK_EQ(q.pop_batch(out, 5U), 0U);   // drained
}

COACT_TEST(spsc_pop_batch_respects_max)
{
    coact::SpscRing<uint16_t, 4> q;
    for (uint16_t i = 0U; i < 4U; ++i) {
        REQUIRE(q.try_push(std::move(i)));
    }
    uint16_t out[2] = {0U, 0U};
    CHECK_EQ(q.pop_batch(out, 2U), 2U);
    CHECK_EQ(q.size(), 2U);
    CHECK_EQ(q.pop_batch(out, 2U), 2U);   // remaining 2 still poppable
    CHECK_EQ(out[0], 2U);
    CHECK_EQ(out[1], 3U);
    CHECK_EQ(q.size(), 0U);
}

// ---------------------------------------------------------------------------
// discard: drop without reading, keeping FIFO semantics for the survivors
// and the same slot-destruction discipline as try_pop.
// ---------------------------------------------------------------------------
COACT_TEST(spsc_discard_drops_without_read)
{
    coact::SpscRing<uint16_t, 4> q;
    for (uint16_t i = 0U; i < 3U; ++i) {
        REQUIRE(q.try_push(std::move(i)));
    }
    CHECK_EQ(q.discard(2U), 2U);
    CHECK_EQ(q.size(), 1U);
    uint16_t v = 0U;
    REQUIRE(q.try_pop(v));
    CHECK_EQ(v, 2U);   // FIFO preserved: the survivor is the 3rd pushed value
}

COACT_TEST(spsc_discard_empty_returns_zero)
{
    coact::SpscRing<uint16_t, 4> q;
    CHECK_EQ(q.discard(2U), 0U);
    CHECK_EQ(q.size(), 0U);
}

// ---------------------------------------------------------------------------
// peek: observe the front element without consuming it.
// ---------------------------------------------------------------------------
COACT_TEST(spsc_peek_does_not_consume)
{
    coact::SpscRing<uint16_t, 4> q;
    REQUIRE(q.try_push(10U));
    REQUIRE(q.try_push(11U));

    uint16_t v = 0U;
    REQUIRE(q.peek(v));
    CHECK_EQ(v, 10U);
    REQUIRE(q.peek(v));   // idempotent: same front, nothing consumed
    CHECK_EQ(v, 10U);
    CHECK_EQ(q.size(), 2U);

    REQUIRE(q.try_pop(v));
    CHECK_EQ(v, 10U);
    REQUIRE(q.try_pop(v));
    CHECK_EQ(v, 11U);
}

COACT_TEST(spsc_peek_empty_returns_false)
{
    coact::SpscRing<uint16_t, 4> q;
    uint16_t v = 0xAAAAU;
    CHECK(!q.peek(v));
    CHECK_EQ(v, 0xAAAAU);   // untouched on failure
    CHECK_EQ(q.size(), 0U);
}

// ---------------------------------------------------------------------------
// Producer batch publish: push_batch clamps to free capacity, keeps partial
// success (no rollback), and publishes the whole batch with one release
// store of head.
// ---------------------------------------------------------------------------
COACT_TEST(spsc_push_batch_full_capacity_clamps)
{
    coact::SpscRing<uint16_t, 4> q;
    const uint16_t values[5] = {10U, 11U, 12U, 13U, 14U};
    CHECK_EQ(q.push_batch(values, 5U), 4U);
    CHECK_EQ(q.size(), 4U);

    uint16_t out[4] = {0U, 0U, 0U, 0U};
    CHECK_EQ(q.pop_batch(out, 4U), 4U);
    CHECK_EQ(out[0], 10U);
    CHECK_EQ(out[3], 13U);   // the 5th value was never pushed
}

COACT_TEST(spsc_push_batch_partial_after_partial_drain)
{
    coact::SpscRing<uint16_t, 4> q;
    for (uint16_t i = 0U; i < 3U; ++i) {
        REQUIRE(q.try_push(std::move(i)));   // 0, 1, 2
    }
    uint16_t drained[2] = {0U, 0U};
    REQUIRE_EQ(q.pop_batch(drained, 2U), 2U);   // 0, 1 gone; 2 remains

    const uint16_t values[3] = {20U, 21U, 22U};
    CHECK_EQ(q.push_batch(values, 3U), 3U);   // exactly 3 free slots
    CHECK_EQ(q.size(), 4U);

    uint16_t out[4] = {0U, 0U, 0U, 0U};
    REQUIRE_EQ(q.pop_batch(out, 4U), 4U);
    CHECK_EQ(out[0], 2U);    // FIFO across batches: survivor first
    CHECK_EQ(out[1], 20U);
    CHECK_EQ(out[2], 21U);
    CHECK_EQ(out[3], 22U);
}

COACT_TEST(spsc_push_batch_empty_input)
{
    coact::SpscRing<uint16_t, 4> q;
    const uint16_t values[1] = {0U};
    CHECK_EQ(q.push_batch(nullptr, 0U), 0U);
    CHECK_EQ(q.push_batch(values, 0U), 0U);
    CHECK_EQ(q.push_batch(nullptr, 3U), 0U);   // defensive: null + count>0
    CHECK_EQ(q.size(), 0U);
}

// Boundary hardening (review R2 follow-up): pop_batch/peek must reject a null
// output buffer, and a max_count larger than the capacity must clamp to the
// ring contents, never over-run the caller's storage or wrap the sequences.
COACT_TEST(spsc_pop_batch_null_out_returns_zero)
{
    coact::SpscRing<uint16_t, 4> q;
    uint16_t v = 1U;
    REQUIRE(q.try_push(std::move(v)));
    CHECK_EQ(q.pop_batch(nullptr, 4U), 0U);
    CHECK_EQ(q.size(), 1U);   // nothing consumed
}

COACT_TEST(spsc_pop_batch_max_count_over_capacity_clamps)
{
    coact::SpscRing<uint16_t, 4> q;
    for (uint16_t i = 0U; i < 4U; ++i) {
        REQUIRE(q.try_push(std::move(i)));
    }
    /* 100 > capacity 4 and > available 4: must clamp to 4, not 100. */
    CHECK_EQ(q.pop_batch(nullptr, 100U), 0U);
    uint16_t out[4] = {0U, 0U, 0U, 0U};
    CHECK_EQ(q.pop_batch(out, 100U), 4U);
    CHECK_EQ(out[0], 0U);
    CHECK_EQ(out[3], 3U);
    CHECK_EQ(q.size(), 0U);
}

// peek takes a reference, so a null buffer is unrepresentable at the API
// level; its empty-contract is already covered by spsc_peek_empty_returns_
// false. discard's max_count clamp mirrors pop_batch: over-capacity requests
// discard exactly the available contents.
COACT_TEST(spsc_discard_max_count_over_capacity_clamps)
{
    coact::SpscRing<uint16_t, 4> q;
    for (uint16_t i = 0U; i < 3U; ++i) {
        REQUIRE(q.try_push(std::move(i)));
    }
    CHECK_EQ(q.discard(100U), 3U);
    CHECK_EQ(q.size(), 0U);
}

// ---------------------------------------------------------------------------
// Batch API under real SMP concurrency: a producer thread pushes in batches,
// a consumer thread drains in batches. Total count must be conserved and the
// global FIFO order preserved (batches never reorder elements).
// ---------------------------------------------------------------------------
COACT_TEST(spsc_batch_producer_consumer_threaded)
{
    constexpr uint32_t kRounds = 2000U;
    constexpr uint16_t kBatch = 4U;
    constexpr uint32_t kTotal = kRounds * kBatch;
    coact::SpscRing<uint32_t, 16> q;
    std::atomic<bool> ok{true};
    std::atomic<uint32_t> consumed{0U};

    std::thread consumer([&q, &ok, &consumed]() {
        uint32_t buf[kBatch];
        uint32_t expected = 0U;
        uint32_t count = 0U;
        while (count < kTotal) {
            const uint16_t n = q.pop_batch(buf, kBatch);
            for (uint16_t i = 0U; i < n; ++i) {
                if (buf[i] != expected) {
                    ok.store(false, std::memory_order_relaxed);
                }
                ++expected;
            }
            count += n;
            if (0U == n) {
                std::this_thread::yield();
            }
        }
        consumed.store(count, std::memory_order_relaxed);
    });

    for (uint32_t round = 0U; round < kRounds; ++round) {
        uint32_t sent = 0U;
        while (sent < kBatch) {
            const uint32_t base = round * kBatch;
            uint32_t values[kBatch];
            for (uint16_t i = 0U; i < kBatch; ++i) {
                values[i] = base + i;
            }
            sent += q.push_batch(values, kBatch);
            if (sent < kBatch) {
                std::this_thread::yield();
            }
        }
    }
    consumer.join();

    CHECK(ok.load());
    CHECK_EQ(consumed.load(), kTotal);
    CHECK_EQ(q.size(), 0U);
}

}  // namespace

COACT_TEST_MAIN()

