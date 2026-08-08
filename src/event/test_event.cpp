// coact event module host tests: QP-style ref-count ownership, pool routing,
// exhaustion, block reuse and zero-heap proof. See contract 4.1.
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "coact/event.hpp"
#include "coact/pool.hpp"

#include "test/test_harness.hpp"

// --- global allocator hook (host only): counts every heap allocation --------
namespace {

std::uint64_t g_alloc_count = 0U;
std::uint64_t g_free_count = 0U;

}  // namespace

void* operator new(std::size_t n)
{
    ++g_alloc_count;
    if (void* p = std::malloc(n)) {
        return p;
    }
    std::abort();
}

void* operator new[](std::size_t n)
{
    ++g_alloc_count;
    if (void* p = std::malloc(n)) {
        return p;
    }
    std::abort();
}

void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
    ++g_alloc_count;
    return std::malloc(n);
}

void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
    ++g_alloc_count;
    return std::malloc(n);
}

void operator delete(void* p) noexcept
{
    ++g_free_count;
    std::free(p);
}

void operator delete[](void* p) noexcept
{
    ++g_free_count;
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept
{
    ++g_free_count;
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
    ++g_free_count;
    std::free(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept
{
    ++g_free_count;
    std::free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept
{
    ++g_free_count;
    std::free(p);
}

// --- test support ------------------------------------------------------------

namespace {

constexpr std::uint16_t kCap = 8U;

// External storage for a pool of `Capacity` blocks, stride-aligned so the
// pool's internal base alignment never reduces the effective capacity.
template <std::size_t Stride, std::uint16_t Capacity>
struct PoolStorage {
    alignas(64) std::uint8_t data[Stride * Capacity];
};

}  // namespace

// --- tests -------------------------------------------------------------------

COACT_TEST(event_alloc_basic)
{
    PoolStorage<16U, kCap> storage;
    coact::EventPool<16U, kCap> pool;
    pool.init(storage.data, sizeof(storage.data));

    CHECK_EQ(pool.used(), 0U);
    CHECK_EQ(pool.high_watermark(), 0U);

    coact::Event* e = pool.alloc(0x1234U);
    REQUIRE(e != nullptr);
    CHECK_EQ(e->signal, 0x1234U);
    CHECK(e->pool_id != 0U);
    CHECK_EQ(e->ref_ctr, 0U);
    CHECK_EQ(pool.used(), 1U);
    CHECK_EQ(pool.high_watermark(), 1U);

    // the registry routes this pool_id back to this pool's record
    coact::PoolRecord* rec = coact::pool_record(e->pool_id);
    REQUIRE(rec != nullptr);
    CHECK_EQ(rec->block_size, 16U);
    CHECK_EQ(rec->capacity, kCap);

    coact::event_gc(e);
    CHECK_EQ(pool.used(), 0U);
    CHECK_EQ(pool.high_watermark(), 1U);
}

COACT_TEST(event_single_consumer)
{
    PoolStorage<16U, kCap> storage;
    coact::EventPool<16U, kCap> pool;
    pool.init(storage.data, sizeof(storage.data));

    coact::Event* e = pool.alloc(0x0001U);
    REQUIRE(e != nullptr);

    coact::event_ref_inc(e);   // one post
    CHECK_EQ(e->ref_ctr, 1U);
    CHECK_EQ(pool.used(), 1U);

    coact::event_gc(e);        // consumer done: ref 1 -> 0 -> recycled
    CHECK_EQ(pool.used(), 0U);
}

COACT_TEST(event_multicast_refcnt)
{
    PoolStorage<16U, kCap> storage;
    coact::EventPool<16U, kCap> pool;
    pool.init(storage.data, sizeof(storage.data));

    coact::Event* e = pool.alloc(0x7777U);
    REQUIRE(e != nullptr);
    CHECK_EQ(e->ref_ctr, 0U);

    // post the same event to two consumers: two incs
    coact::event_ref_inc(e);
    coact::event_ref_inc(e);
    CHECK_EQ(e->ref_ctr, 2U);
    CHECK_EQ(pool.used(), 1U);

    // first consumer finishes: ref 2 -> 1, still outstanding
    coact::event_gc(e);
    CHECK_EQ(e->ref_ctr, 1U);
    CHECK_EQ(pool.used(), 1U);

    // second consumer finishes: ref 1 -> 0, recycled
    coact::event_gc(e);
    CHECK_EQ(pool.used(), 0U);
    CHECK_EQ(pool.high_watermark(), 1U);
}

COACT_TEST(event_static_not_recycled)
{
    // static (non-pool) event: inc/gc are both no-ops
    coact::Event sev{0xABCDU, 0U, 0U};
    coact::event_ref_inc(&sev);
    coact::event_ref_inc(&sev);
    CHECK_EQ(sev.ref_ctr, 0U);        // inc is a no-op for static events
    coact::event_gc(&sev);
    coact::event_gc(&sev);
    CHECK_EQ(sev.signal, 0xABCDU);    // untouched
    CHECK_EQ(sev.pool_id, 0U);
    CHECK_EQ(sev.ref_ctr, 0U);
}

COACT_TEST(event_pool_exhausted)
{
    PoolStorage<16U, kCap> storage;
    coact::EventPool<16U, kCap> pool;
    pool.init(storage.data, sizeof(storage.data));

    coact::Event* blocks[kCap];
    for (std::uint16_t i = 0U; i < kCap; ++i) {
        blocks[i] = pool.alloc(static_cast<std::uint16_t>(i + 1U));
        REQUIRE(blocks[i] != nullptr);
        CHECK_EQ(pool.used(), static_cast<std::uint16_t>(i + 1U));
    }
    CHECK_EQ(pool.high_watermark(), kCap);

    // pool is full: next alloc returns nullptr
    CHECK(pool.alloc(0xFFU) == nullptr);
    CHECK_EQ(pool.used(), kCap);

    // gc one block, then alloc succeeds again
    coact::event_gc(blocks[0]);
    CHECK_EQ(pool.used(), kCap - 1U);
    coact::Event* fresh = pool.alloc(0xEEU);
    REQUIRE(fresh != nullptr);
    CHECK_EQ(pool.used(), kCap);
    CHECK_EQ(fresh->ref_ctr, 0U);

    // drain everything
    coact::event_gc(fresh);
    for (std::uint16_t i = 1U; i < kCap; ++i) {
        coact::event_gc(blocks[i]);
    }
    CHECK_EQ(pool.used(), 0U);
}

COACT_TEST(event_cross_pool_routing)
{
    PoolStorage<16U, kCap> storage_a;
    PoolStorage<16U, kCap> storage_b;
    coact::EventPool<16U, kCap> pool_a;
    coact::EventPool<16U, kCap> pool_b;
    pool_a.init(storage_a.data, sizeof(storage_a.data));
    pool_b.init(storage_b.data, sizeof(storage_b.data));

    coact::Event* ea = pool_a.alloc(0xAAAAU);
    coact::Event* eb = pool_b.alloc(0xBBBBU);
    REQUIRE(ea != nullptr);
    REQUIRE(eb != nullptr);

    // distinct pool ids and distinct blocks in distinct storage
    CHECK_NE(ea->pool_id, eb->pool_id);
    CHECK_NE(ea, eb);
    CHECK(coact::pool_record(ea->pool_id) != nullptr);
    CHECK(coact::pool_record(eb->pool_id) != nullptr);

    // each event routes back to its own pool, never the other one
    coact::event_gc(ea);
    CHECK_EQ(pool_a.used(), 0U);
    CHECK_EQ(pool_b.used(), 1U);

    coact::event_gc(eb);
    CHECK_EQ(pool_a.used(), 0U);
    CHECK_EQ(pool_b.used(), 0U);
}

COACT_TEST(event_block_reuse_after_gc)
{
    PoolStorage<16U, kCap> storage;
    coact::EventPool<16U, kCap> pool;
    pool.init(storage.data, sizeof(storage.data));

    coact::Event* first = pool.alloc(0x1111U);
    REQUIRE(first != nullptr);
    const std::uintptr_t first_addr = reinterpret_cast<std::uintptr_t>(first);
    const std::uintptr_t lo = reinterpret_cast<std::uintptr_t>(storage.data);
    const std::uintptr_t hi =
        reinterpret_cast<std::uintptr_t>(storage.data + sizeof(storage.data));
    CHECK(first_addr >= lo);
    CHECK(first_addr < hi);

    coact::event_gc(first);
    CHECK_EQ(pool.used(), 0U);

    // the free list is LIFO, so the next alloc reuses the same block
    coact::Event* second = pool.alloc(0x2222U);
    REQUIRE(second != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(second) == first_addr);
    CHECK_EQ(second->signal, 0x2222U);
    CHECK_EQ(second->ref_ctr, 0U);
    CHECK_EQ(pool.used(), 1U);

    coact::event_gc(second);
    CHECK_EQ(pool.used(), 0U);
}

COACT_TEST(event_payload_storage)
{
    // a pool whose blocks carry payload beyond the Event base
    PoolStorage<32U, 4U> storage;
    coact::EventPool<32U, 4U> pool;
    pool.init(storage.data, sizeof(storage.data));

    coact::Event* e0 = pool.alloc(0x100U);
    coact::Event* e1 = pool.alloc(0x200U);
    REQUIRE(e0 != nullptr);
    REQUIRE(e1 != nullptr);

    // consecutive blocks are stride-apart (32-byte aligned)
    const std::uintptr_t a0 = reinterpret_cast<std::uintptr_t>(e0);
    const std::uintptr_t a1 = reinterpret_cast<std::uintptr_t>(e1);
    CHECK_EQ(a1 - a0, 32U);

    // the payload area after the Event base is usable
    const std::uint32_t payload = 0xDEADBEEFU;
    std::uint8_t* payload_slot =
        reinterpret_cast<std::uint8_t*>(e0) + sizeof(coact::Event);
    std::memcpy(payload_slot, &payload, sizeof(payload));
    std::uint32_t readback = 0U;
    std::memcpy(&readback, payload_slot, sizeof(readback));
    CHECK_EQ(readback, 0xDEADBEEFU);

    coact::event_gc(e0);
    coact::event_gc(e1);
    CHECK_EQ(pool.used(), 0U);
}

COACT_TEST(event_zero_heap)
{
    // prove the hook is live: the harness registry allocated during static
    // initialization, before main() runs
    CHECK(g_alloc_count > 0U);

    const std::uint64_t alloc_before = g_alloc_count;
    const std::uint64_t free_before = g_free_count;

    PoolStorage<16U, kCap> storage;
    coact::EventPool<16U, kCap> pool;
    pool.init(storage.data, sizeof(storage.data));

    coact::Event* blocks[kCap];
    for (std::uint16_t round = 0U; round < 8U; ++round) {
        for (std::uint16_t i = 0U; i < kCap; ++i) {
            blocks[i] = pool.alloc(static_cast<std::uint16_t>(round + 1U));
            REQUIRE(blocks[i] != nullptr);
        }
        for (std::uint16_t i = 0U; i < kCap; ++i) {
            coact::event_ref_inc(blocks[i]);
            coact::event_gc(blocks[i]);
        }
        REQUIRE_EQ(pool.used(), 0U);
    }

    // the whole alloc/inc/gc/reclaim loop never touched the heap
    CHECK_EQ(g_alloc_count, alloc_before);
    CHECK_EQ(g_free_count, free_before);
}

COACT_TEST(event_registry_queries)
{
    CHECK(coact::pool_record(0U) == nullptr);       // static-event id
    CHECK(coact::pool_record(0xFFU) == nullptr);    // unknown id
    CHECK(coact::pool_record(static_cast<std::uint8_t>(coact::kMaxEventPools + 1U)) == nullptr);
    CHECK(coact::register_pool(nullptr) == 0U);

    // register a record and look it back up by the assigned id
    static coact::PoolRecord probe_record{};
    const std::uint8_t id = coact::register_pool(&probe_record);
    REQUIRE(id != 0U);
    CHECK(coact::pool_record(id) == &probe_record);

    // idempotent re-registration returns the same id
    CHECK_EQ(coact::register_pool(&probe_record), id);
}

COACT_TEST_MAIN()
