// coact EventPool C++17 object-lifecycle tests (design §7.2 / §7.4):
// placement-new starts the coact::Event lifetime on every alloc; typed
// allocation over a composed EventBlockLayout (meta + payload) placement-news a
// trivial payload; the compile-time contract rejects non-trivial payloads, bad
// alignment, oversized layouts and derived-event (downcast) views.
// SPDX-License-Identifier: MIT

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

#include "coact/event.hpp"
#include "coact/pool.hpp"

#include "test/test_harness.hpp"

namespace {

constexpr std::uint16_t kCap = 4U;

// Application metadata region (cmdfw MessageMeta shape).
struct TestMeta {
    std::uint32_t request_id;
    std::uint32_t deadline_tick;
    std::uint16_t descriptor_index;
    std::uint16_t payload_size;
};

// Trivial POD payload for the typed allocation entry.
struct TestPayload {
    std::uint32_t a;
    std::uint32_t b;
    std::uint8_t flags;
};

constexpr size_t kPayloadBytes = 64U;
constexpr size_t kPayloadAlign = 8U;

using Layout = coact::EventBlockLayout<TestMeta, kPayloadBytes, kPayloadAlign>;

struct LayoutWithoutMeta {
    coact::Event event;
    std::uint32_t header;
    alignas(kPayloadAlign) std::byte payload[kPayloadBytes];
};

constexpr std::uint16_t kBlockSize =
    static_cast<std::uint16_t>(sizeof(Layout));

template <size_t Stride, std::uint16_t Capacity>
struct PoolStorage {
    alignas(64) std::uint8_t data[Stride * Capacity];
};

// ---------------------------------------------------------------------------
// Compile-time contract: positive cases must pass.
// ---------------------------------------------------------------------------
static_assert(coact::detail::is_trivially_poolable_v<TestPayload>,
              "TestPayload must be trivially poolable");
static_assert(std::is_trivially_copyable<TestPayload>::value,
              "TestPayload must be trivially copyable");
static_assert(std::is_trivially_destructible<TestPayload>::value,
              "TestPayload must be trivially destructible");
static_assert(std::is_standard_layout<Layout>::value,
              "Layout must be standard-layout");
static_assert(offsetof(Layout, event) == 0U,
              "coact::Event must sit at offset 0 of the layout");
static_assert(alignof(Layout) >= kPayloadAlign,
              "layout alignment must cover the payload alignment");
static_assert(coact::detail::event_layout_complies_v<
                  Layout, TestPayload, kPayloadAlign, kBlockSize>,
              "a valid layout + trivial payload must comply");
static_assert(coact::detail::event_layout_complies_v<
                  LayoutWithoutMeta, TestPayload, kPayloadAlign,
                  static_cast<std::uint16_t>(sizeof(LayoutWithoutMeta))>,
              "a trivial custom layout does not require a named meta member");

// ---------------------------------------------------------------------------
// Compile-time contract: violating payloads / layouts must be reported false
// by the pure trait (the alloc_typed entry turns these into hard compile
// errors, proven by the pool_lifecycle_neg negative-compile target).
// ---------------------------------------------------------------------------
struct NonTrivialPayload {
    std::uint32_t x;
    ~NonTrivialPayload() {}
};
static_assert(!coact::detail::is_trivially_poolable_v<NonTrivialPayload>,
              "non-trivial payload must be rejected");

struct OverAlignedPayload {
    alignas(32) std::uint32_t x;
};
static_assert(!coact::detail::event_layout_complies_v<
                  Layout, OverAlignedPayload, kPayloadAlign, kBlockSize>,
              "over-aligned payload must be rejected");

struct OversizedTrivialPayload {
    std::byte bytes[kPayloadBytes + 1U];
};
static_assert(coact::detail::is_trivially_poolable_v<OversizedTrivialPayload>,
              "oversized payload must otherwise remain trivially poolable");
static_assert(!coact::detail::event_layout_complies_v<
                  Layout, OversizedTrivialPayload, kPayloadAlign, kBlockSize>,
              "payload larger than the layout payload region must be rejected");

struct CustomDefaultPayload {
    std::uint32_t value;
    CustomDefaultPayload() noexcept : value(0x13579BDFU) {}
};
static_assert(coact::detail::is_trivially_poolable_v<CustomDefaultPayload>,
              "custom-default payload must otherwise remain trivially poolable");
static_assert(!std::is_trivially_default_constructible<
                  CustomDefaultPayload>::value,
              "custom-default payload must not be trivially default constructible");
static_assert(std::is_nothrow_default_constructible<
                  CustomDefaultPayload>::value,
              "custom-default payload construction must be noexcept");
static_assert(coact::detail::event_layout_complies_v<
                  Layout, CustomDefaultPayload, kPayloadAlign, kBlockSize>,
              "noexcept custom-default payload must comply");

struct CustomDefaultLayout {
    coact::Event event;
    TestMeta meta;
    alignas(kPayloadAlign) std::byte payload[kPayloadBytes];

    CustomDefaultLayout() noexcept
        : event{}, meta{0x12345678U, 0x87654321U, 0x55AAU, 0xAA55U},
          payload{} {}
};
static_assert(coact::detail::is_trivially_poolable_v<CustomDefaultLayout>,
              "custom-default layout must otherwise remain trivially poolable");
static_assert(!std::is_trivially_default_constructible<
                  CustomDefaultLayout>::value,
              "custom-default layout must not be trivially default constructible");
static_assert(std::is_nothrow_default_constructible<
                  CustomDefaultLayout>::value,
              "custom-default layout construction must be noexcept");
static_assert(coact::detail::event_layout_complies_v<
                  CustomDefaultLayout, TestPayload, kPayloadAlign,
                  static_cast<std::uint16_t>(sizeof(CustomDefaultLayout))>,
              "noexcept custom-default layout must comply");

using BigLayout = coact::EventBlockLayout<TestMeta, 128, kPayloadAlign>;
static_assert(!coact::detail::event_layout_complies_v<
                  BigLayout, TestPayload, kPayloadAlign, kBlockSize>,
              "layout larger than the pool block must be rejected");

// A layout whose event member is a DERIVED object is forbidden (no downcast).
struct DerivedEvent : coact::Event {
    std::uint32_t extra;
};
struct DerivedLayout {
    DerivedEvent event;
    TestMeta meta;
    alignas(kPayloadAlign) std::byte payload[kPayloadBytes];
};
static_assert(!coact::detail::event_layout_complies_v<
                  DerivedLayout, TestPayload, kPayloadAlign,
                  static_cast<std::uint16_t>(sizeof(DerivedLayout))>,
              "derived-event layout must be rejected");

}  // namespace

// ---------------------------------------------------------------------------
// Runtime: EventPool::alloc starts the Event lifetime (placement-new) and
// re-initializes signal/pool_id/ref_ctr on every alloc / reuse.
// ---------------------------------------------------------------------------
COACT_TEST(pool_lifecycle_alloc_placement_new)
{
    PoolStorage<16U, kCap> storage;
    coact::EventPool<16U, kCap> pool;
    pool.init(storage.data, sizeof(storage.data));

    coact::Event* e = pool.alloc(0x1234U);
    REQUIRE(e != nullptr);
    CHECK_EQ(e->signal, 0x1234U);
    CHECK(e->pool_id != 0U);
    CHECK_EQ(e->ref_ctr, 1U);

    coact::event_gc(e);
    CHECK_EQ(pool.used(), 0U);

    // the free list is LIFO: the same block is reused and its fields must be
    // re-initialized (no stale bytes leak into the new Event).
    coact::Event* e2 = pool.alloc(0x7777U);
    REQUIRE(e2 != nullptr);
    CHECK(e2 == e);
    CHECK_EQ(e2->signal, 0x7777U);
    CHECK(e2->pool_id != 0U);
    CHECK_EQ(e2->ref_ctr, 1U);

    coact::event_gc(e2);
}

// ---------------------------------------------------------------------------
// Runtime: typed allocation returns a composed-layout view with a live Event
// header and a zero-initialized payload; the codec-owned meta region can be
// written without corrupting header or payload.
// ---------------------------------------------------------------------------
COACT_TEST(pool_lifecycle_typed_alloc_basic)
{
    PoolStorage<kBlockSize, kCap> storage;
    std::memset(storage.data, 0xA5, sizeof(storage.data));
    coact::EventPool<kBlockSize, kCap> pool;
    pool.init(storage.data, sizeof(storage.data));

    Layout* layout =
        pool.alloc_typed<Layout, TestPayload, kPayloadAlign>(0x11U);
    REQUIRE(layout != nullptr);
    CHECK_EQ(layout->event.signal, 0x11U);
    CHECK(layout->event.pool_id != 0U);
    CHECK_EQ(layout->event.ref_ctr, 1U);
    CHECK_EQ(pool.used(), 1U);
    CHECK_EQ(layout->meta.request_id, 0U);
    CHECK_EQ(layout->meta.deadline_tick, 0U);
    CHECK_EQ(layout->meta.descriptor_index, 0U);
    CHECK_EQ(layout->meta.payload_size, 0U);

    // payload was placement-newed and zero-initialized (nanopb init_zero
    // semantics for a trivial POD).
    TestPayload* p = reinterpret_cast<TestPayload*>(&layout->payload[0]);
    CHECK_EQ(p->a, 0U);
    CHECK_EQ(p->b, 0U);
    CHECK_EQ(p->flags, 0U);

    // codec writes the meta region; header and payload stay untouched.
    layout->meta.request_id = 0xDEADU;
    layout->meta.payload_size = 4U;
    CHECK_EQ(layout->event.signal, 0x11U);
    CHECK_EQ(layout->event.ref_ctr, 1U);
    CHECK_EQ(p->a, 0U);

    coact::event_gc(&layout->event);
    CHECK_EQ(pool.used(), 0U);
}

// ---------------------------------------------------------------------------
// Runtime: reclaim + re-alloc reuses the same block and the typed entry
// re-zeroes the payload every time.
// ---------------------------------------------------------------------------
COACT_TEST(pool_lifecycle_typed_alloc_reuse)
{
    PoolStorage<kBlockSize, kCap> storage;
    coact::EventPool<kBlockSize, kCap> pool;
    pool.init(storage.data, sizeof(storage.data));

    Layout* l1 =
        pool.alloc_typed<Layout, TestPayload, kPayloadAlign>(0xAAU);
    REQUIRE(l1 != nullptr);
    TestPayload* p1 = reinterpret_cast<TestPayload*>(&l1->payload[0]);
    p1->a = 0x11223344U;
    p1->b = 0x55667788U;
    p1->flags = 0x7U;
    coact::event_gc(&l1->event);
    CHECK_EQ(pool.used(), 0U);

    Layout* l2 =
        pool.alloc_typed<Layout, TestPayload, kPayloadAlign>(0xBBU);
    REQUIRE(l2 != nullptr);
    CHECK(l2 == l1);                       // LIFO reuse of the same block
    CHECK_EQ(l2->event.signal, 0xBBU);
    CHECK_EQ(l2->event.ref_ctr, 1U);

    TestPayload* p2 = reinterpret_cast<TestPayload*>(&l2->payload[0]);
    CHECK_EQ(p2->a, 0U);                   // payload re-zeroed
    CHECK_EQ(p2->b, 0U);
    CHECK_EQ(p2->flags, 0U);

    coact::event_gc(&l2->event);
}

COACT_TEST(pool_lifecycle_typed_alloc_runs_noexcept_default_constructors)
{
    PoolStorage<kBlockSize, kCap> payload_storage;
    std::memset(payload_storage.data, 0xA5, sizeof(payload_storage.data));
    coact::EventPool<kBlockSize, kCap> payload_pool;
    payload_pool.init(payload_storage.data, sizeof(payload_storage.data));

    Layout* payload_layout =
        payload_pool.alloc_typed<Layout, CustomDefaultPayload, kPayloadAlign>(
            0x31U);
    REQUIRE(payload_layout != nullptr);
    const auto* const payload = reinterpret_cast<const CustomDefaultPayload*>(
        &payload_layout->payload[0]);
    CHECK_EQ(payload->value, 0x13579BDFU);
    coact::event_gc(&payload_layout->event);

    constexpr std::uint16_t kCustomBlockSize =
        static_cast<std::uint16_t>(sizeof(CustomDefaultLayout));
    PoolStorage<kCustomBlockSize, kCap> layout_storage;
    std::memset(layout_storage.data, 0xA5, sizeof(layout_storage.data));
    coact::EventPool<kCustomBlockSize, kCap> layout_pool;
    layout_pool.init(layout_storage.data, sizeof(layout_storage.data));

    CustomDefaultLayout* custom_layout =
        layout_pool.alloc_typed<CustomDefaultLayout, TestPayload,
                                kPayloadAlign>(0x32U);
    REQUIRE(custom_layout != nullptr);
    CHECK_EQ(custom_layout->meta.request_id, 0x12345678U);
    CHECK_EQ(custom_layout->meta.deadline_tick, 0x87654321U);
    CHECK_EQ(custom_layout->meta.descriptor_index, 0x55AAU);
    CHECK_EQ(custom_layout->meta.payload_size, 0xAA55U);
    coact::event_gc(&custom_layout->event);
}

// ---------------------------------------------------------------------------
// Runtime: pool exhaustion yields nullptr from the typed entry (caller can
// distinguish alloc failure from decode).
// ---------------------------------------------------------------------------
COACT_TEST(pool_lifecycle_typed_alloc_exhausted)
{
    PoolStorage<kBlockSize, 8U> storage;
    coact::EventPool<kBlockSize, 8U> pool;
    pool.init(storage.data, sizeof(storage.data));

    coact::Event* all[8];
    std::uint16_t got = 0U;
    for (std::uint16_t i = 0U; i < 8U; ++i) {
        Layout* l = pool.alloc_typed<Layout, TestPayload, kPayloadAlign>(
            static_cast<std::uint16_t>(i + 1U));
        if (nullptr == l) {
            break;
        }
        CHECK_EQ(l->event.signal, static_cast<std::uint16_t>(i + 1U));
        all[got++] = &l->event;
    }
    CHECK(got > 0U);

    // exhausted: alloc failure is a null return, not a throw / undefined value
    // (extra parens: the template-id commas would split the CHECK macro args)
    CHECK((pool.alloc_typed<Layout, TestPayload, kPayloadAlign>(0xEEU) == nullptr));

    for (std::uint16_t i = 0U; i < got; ++i) {
        coact::event_gc(all[i]);
    }
    CHECK_EQ(pool.used(), 0U);
}

// ---------------------------------------------------------------------------
// Runtime: the composed layout is a view with Event at offset 0; Event* and
// Layout* are pointer-interconvertible (no hidden derived base offset).
// ---------------------------------------------------------------------------
COACT_TEST(pool_lifecycle_layout_geometry)
{
    static_assert(offsetof(Layout, event) == 0U, "event must be first");
    CHECK_EQ(offsetof(Layout, event), 0U);
    CHECK_EQ(offsetof(Layout, meta), sizeof(coact::Event));
    CHECK_EQ(coact::event_block_meta_offset<Layout>(), sizeof(coact::Event));
    CHECK_EQ(coact::event_block_payload_offset<Layout>(),
             offsetof(Layout, payload));
    CHECK(offsetof(Layout, payload) % alignof(TestPayload) == 0U);
    CHECK(alignof(Layout) >= kPayloadAlign);

    PoolStorage<kBlockSize, kCap> storage;
    coact::EventPool<kBlockSize, kCap> pool;
    pool.init(storage.data, sizeof(storage.data));

    Layout* l = pool.alloc_typed<Layout, TestPayload, kPayloadAlign>(0x5AU);
    REQUIRE(l != nullptr);

    coact::Event* ev = reinterpret_cast<coact::Event*>(l);
    CHECK(reinterpret_cast<std::uintptr_t>(ev) ==
          reinterpret_cast<std::uintptr_t>(l));
    CHECK_EQ(ev->signal, 0x5AU);
    CHECK_EQ(ev->ref_ctr, 1U);

    coact::event_gc(ev);
    CHECK_EQ(pool.used(), 0U);
}

COACT_TEST_MAIN()
