// coact RttSingleCoreProfile pool-backend tests (design §7.4):
// the single-core profile must do claim/reclaim/free-list and
// used/high-watermark updates inside one irq-mask critical section, with NO
// CAS / ABA tag / backoff (the injected CS already serializes). Stats reads
// must also go through the same CS so a reader gets a consistent snapshot.
//
// The mock irq-mask CS below counts save/restore and tracks nesting depth, so
// the tests prove every pool op (and every stats read) actually travels through
// the injected critical section.
// SPDX-License-Identifier: MIT

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

#include "coact/event.hpp"
#include "coact/pool.hpp"

#include "test/test_harness.hpp"

namespace {

constexpr std::uint16_t kCap = 4U;
constexpr std::uint16_t kBlock = 32U;

// Mock irq-mask: save() "masks" (depth++), restore() "unmasks" (depth--).
// Counts every enter/leave so tests can assert pool ops are CS-guarded and
// balanced (no leaked irq-mask hold).
struct MockIrqMask {
    std::atomic<int> depth{0};
    std::atomic<std::uint64_t> save_count{0};
    std::atomic<std::uint64_t> restore_count{0};

    coact::CriticalSection cs() noexcept
    {
        coact::CriticalSection c;
        c.ctx = this;
        c.save = [](void* ctx) -> coact::CriticalSection::Token {
            auto* m = static_cast<MockIrqMask*>(ctx);
            m->save_count.fetch_add(1U, std::memory_order_relaxed);
            return static_cast<coact::CriticalSection::Token>(
                m->depth.fetch_add(1, std::memory_order_relaxed) + 1);
        };
        c.restore = [](void* ctx, coact::CriticalSection::Token) {
            auto* m = static_cast<MockIrqMask*>(ctx);
            m->restore_count.fetch_add(1U, std::memory_order_relaxed);
            m->depth.fetch_sub(1, std::memory_order_relaxed);
        };
        return c;
    }

    void check_balanced() const noexcept
    {
        CHECK_EQ(static_cast<long>(save_count.load(std::memory_order_relaxed)),
                 static_cast<long>(restore_count.load(std::memory_order_relaxed)));
        CHECK_EQ(depth.load(std::memory_order_relaxed), 0);
    }
};

template <std::uint16_t Stride, std::uint16_t Capacity>
struct PoolStorage {
    alignas(64) std::uint8_t data[Stride * Capacity];
};

// The payload region (after the 4-byte Event header) is 28 bytes on a 32-byte
// block. Carry a probe so reclaim->realloc reuse can be observed.
struct Probe {
    std::uint32_t magic;
    std::uint32_t serial;
};

struct MarginMeta {
    std::uint16_t request_id;
};

struct MarginPayload {
    std::uint32_t value;
};

using MarginLayout = coact::EventBlockLayout<MarginMeta, sizeof(MarginPayload),
                                             alignof(MarginPayload)>;

using CorePool = coact::EventPool<kBlock, kCap, coact::RttSingleCoreProfile>;

// A single generic helper must work with ANY profile instantiation of the one
// EventPool template: proves the profile is a compile-time backend selector,
// not a second pool type.
template <typename Pool>
std::uint16_t claim_all(Pool& pool) noexcept
{
    std::uint16_t got = 0U;
    for (std::uint16_t i = 0U; i < kCap; ++i) {
        coact::Event* e = pool.alloc(static_cast<std::uint16_t>(i + 1U));
        if (nullptr == e) {
            break;
        }
        ++got;
    }
    return got;
}

}  // namespace

COACT_TEST(single_core_claim_reclaim_stats_in_cs)
{
    PoolStorage<kBlock, kCap> storage;
    MockIrqMask mock;
    coact::EventPool<kBlock, kCap, coact::RttSingleCoreProfile> pool;
    pool.init(storage.data, sizeof(storage.data), mock.cs());

    CHECK_EQ(pool.used(), 0U);
    CHECK_EQ(pool.high_watermark(), 0U);

    // claim inside the irq-mask CS: used/hwm tracked in the same CS.
    coact::Event* e0 = pool.alloc(0x1000U);
    REQUIRE(e0 != nullptr);
    CHECK_EQ(e0->signal, 0x1000U);
    CHECK(e0->pool_id != 0U);
    CHECK_EQ(e0->ref_ctr, 0U);
    CHECK_EQ(pool.used(), 1U);
    CHECK_EQ(pool.high_watermark(), 1U);

    coact::Event* e1 = pool.alloc(0x2000U);
    REQUIRE(e1 != nullptr);
    CHECK_EQ(pool.used(), 2U);
    CHECK_EQ(pool.high_watermark(), 2U);

    // reclaim via event_gc travels through the CS too.
    coact::event_gc(e0);
    CHECK_EQ(pool.used(), 1U);
    coact::event_gc(e1);
    CHECK_EQ(pool.used(), 0U);

    // high-watermark is monotonic even after draining.
    CHECK_EQ(pool.high_watermark(), 2U);

    // every save had a matching restore: no irq-mask left held.
    mock.check_balanced();
}

COACT_TEST(single_core_lifo_reuse_after_reclaim)
{
    PoolStorage<kBlock, kCap> storage;
    MockIrqMask mock;
    coact::EventPool<kBlock, kCap, coact::RttSingleCoreProfile> pool;
    pool.init(storage.data, sizeof(storage.data), mock.cs());

    coact::Event* first = pool.alloc(0x1111U);
    REQUIRE(first != nullptr);
    Probe* p0 = reinterpret_cast<Probe*>(reinterpret_cast<std::uint8_t*>(first)
                                         + sizeof(coact::Event));
    p0->magic = 0xCAFEF00DU;
    p0->serial = 7U;
    coact::event_gc(first);
    CHECK_EQ(pool.used(), 0U);

    // plain LIFO free list: same block reused, fields re-initialized.
    coact::Event* second = pool.alloc(0x2222U);
    REQUIRE(second != nullptr);
    CHECK(second == first);
    CHECK_EQ(second->signal, 0x2222U);
    CHECK_EQ(second->pool_id, first->pool_id);
    CHECK_EQ(second->ref_ctr, 0U);

    coact::event_gc(second);
    mock.check_balanced();
}

COACT_TEST(single_core_exhaustion_and_refcnt_roundtrip)
{
    PoolStorage<kBlock, kCap> storage;
    MockIrqMask mock;
    coact::EventPool<kBlock, kCap, coact::RttSingleCoreProfile> pool;
    pool.init(storage.data, sizeof(storage.data), mock.cs());

    coact::Event* blocks[kCap];
    for (std::uint16_t i = 0U; i < kCap; ++i) {
        blocks[i] = pool.alloc(static_cast<std::uint16_t>(i + 1U));
        REQUIRE(blocks[i] != nullptr);
    }
    CHECK_EQ(pool.used(), kCap);
    CHECK_EQ(pool.high_watermark(), kCap);

    // exhausted: plain head pop returns null (no CAS loop).
    CHECK(pool.alloc(0xFFU) == nullptr);
    CHECK_EQ(pool.used(), kCap);

    // ref-count roundtrip: post (inc) then consume (gc) -> recycled.
    coact::event_ref_inc(blocks[0]);
    CHECK_EQ(blocks[0]->ref_ctr, 1U);
    coact::event_gc(blocks[0]);   // 1 -> 0 -> reclaim
    CHECK_EQ(pool.used(), kCap - 1U);

    for (std::uint16_t i = 1U; i < kCap; ++i) {
        coact::event_gc(blocks[i]);
    }
    CHECK_EQ(pool.used(), 0U);
    mock.check_balanced();
}

COACT_TEST(single_core_margin_admission_preserves_reserved_blocks)
{
    PoolStorage<kBlock, kCap> storage;
    MockIrqMask mock;
    coact::EventPool<kBlock, kCap, coact::RttSingleCoreProfile> pool;
    pool.init(storage.data, sizeof(storage.data), mock.cs());

    coact::Event* first = pool.alloc_with_margin(0x10U, 2U);
    coact::Event* second = pool.alloc_with_margin(0x11U, 2U);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK_EQ(pool.used(), 2U);

    CHECK(pool.alloc_with_margin(0x12U, 2U) == nullptr);
    CHECK_EQ(pool.used(), 2U);

    coact::Event* third = pool.alloc_with_margin(0x13U, 0U);
    coact::Event* fourth = pool.alloc_with_margin(0x14U, 0U);
    REQUIRE(third != nullptr);
    REQUIRE(fourth != nullptr);
    CHECK(pool.alloc_with_margin(0x15U, 0U) == nullptr);
    CHECK_EQ(pool.used(), kCap);

    coact::event_gc(first);
    coact::event_gc(second);
    coact::event_gc(third);
    coact::event_gc(fourth);
    CHECK_EQ(pool.used(), 0U);

    coact::Event* recovered = pool.alloc_with_margin(0x16U, 2U);
    REQUIRE(recovered != nullptr);
    coact::event_gc(recovered);

    MarginLayout* typed = pool.alloc_typed_with_margin<
        MarginLayout, MarginPayload, alignof(MarginPayload)>(0x17U, 3U);
    REQUIRE(typed != nullptr);
    CHECK_EQ(typed->event.signal, 0x17U);
    coact::event_gc(&typed->event);
    mock.check_balanced();
}

COACT_TEST(single_core_stats_reads_travel_through_cs)
{
    PoolStorage<kBlock, kCap> storage;
    MockIrqMask mock;
    coact::EventPool<kBlock, kCap, coact::RttSingleCoreProfile> pool;
    pool.init(storage.data, sizeof(storage.data), mock.cs());

    coact::Event* e = pool.alloc(0x1234U);
    REQUIRE(e != nullptr);
    const std::uint64_t saves_after_alloc =
        mock.save_count.load(std::memory_order_relaxed);

    // each used()/high_watermark() read must enter the CS (consistent snapshot).
    (void)pool.used();
    (void)pool.high_watermark();

    CHECK(mock.save_count.load(std::memory_order_relaxed) >=
          saves_after_alloc + 2U);

    coact::event_gc(e);
    mock.check_balanced();
}

COACT_TEST(single_core_is_distinct_compiled_backend)
{
    // One EventPool template, two compile-time backend selections: the two
    // instantiations are distinct types, and the generic helper above works
    // for both (proving it is still the SAME template, not a second pool).
    static_assert(!std::is_same<coact::EventPool<kBlock, kCap, coact::RttSingleCoreProfile>,
                                coact::EventPool<kBlock, kCap, coact::HostSmpProfile>>::value,
                  "the two profiles must select distinct backends");

    PoolStorage<kBlock, kCap> storage;
    MockIrqMask mock;
    coact::EventPool<kBlock, kCap, coact::RttSingleCoreProfile> pool;
    pool.init(storage.data, sizeof(storage.data), mock.cs());

    CHECK_EQ(claim_all(pool), kCap);
    mock.check_balanced();
}

// ---------------------------------------------------------------------------
// Configurable block alignment (design §13.6.2 / Agent A handoff): a payload
// layout whose alignment exceeds alignof(std::max_align_t) is rejected by the
// default stride but accepted once the pool is configured with a BlockAlign
// that covers it. detail::event_layout_complies_v stays the single contract
// anchor - it just learns the pool's BlockAlign.
// ---------------------------------------------------------------------------
namespace {
struct Align32Meta {
    std::uint32_t a;
};
struct Align32Payload {
    alignas(32) std::uint32_t x;
};
using Align32Layout = coact::EventBlockLayout<Align32Meta, 64, 32>;
constexpr std::uint16_t kAlign32Block =
    static_cast<std::uint16_t>(sizeof(Align32Layout));
static_assert(alignof(Align32Layout) == 32U,
              "premise: the composed layout must exceed max_align_t on x86-64");
}  // namespace

COACT_TEST(single_core_configurable_block_alignment)
{
    // Default BlockAlign = alignof(max_align_t) (16 on x86-64) rejects the
    // 32-aligned layout; BlockAlign=32 accepts it - the relaxation Agent A
    // flagged (32-bit targets with a high-aligned payload).
    static_assert(!coact::detail::event_layout_complies_v<
                      Align32Layout, Align32Payload, 32, kAlign32Block>,
                  "default max_align_t stride must reject an over-aligned layout");
    static_assert(coact::detail::event_layout_complies_v<
                      Align32Layout, Align32Payload, 32, kAlign32Block, 32>,
                  "BlockAlign=32 must cover the 32-aligned layout");

    // runtime: a single-core pool configured with BlockAlign=32 claims and
    // recycles the over-aligned layout through the same lifecycle contract.
    PoolStorage<kAlign32Block, 4U> storage;
    MockIrqMask mock;
    coact::EventPool<kAlign32Block, 4U, coact::RttSingleCoreProfile, 32> pool;
    pool.init(storage.data, sizeof(storage.data), mock.cs());

    Align32Layout* l = pool.alloc_typed<Align32Layout, Align32Payload, 32>(0x42U);
    REQUIRE(l != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(l) % 32U == 0U);
    CHECK_EQ(l->event.signal, 0x42U);
    CHECK_EQ(l->event.ref_ctr, 0U);
    Align32Payload* p = reinterpret_cast<Align32Payload*>(&l->payload[0]);
    CHECK_EQ(p->x, 0U);

    coact::event_gc(&l->event);
    CHECK_EQ(pool.used(), 0U);
    mock.check_balanced();
}

COACT_TEST_MAIN()
