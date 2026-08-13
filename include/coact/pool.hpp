// coact EventPool - fixed-capacity mutable-event pool, plus the QP-style
// reference-count helpers event_ref_inc/event_gc. See design 6 and
// implementation contract 4.1.
//
// The synchronization backend is selected at compile time by a profile
// (design §7.4):
//   - RttSingleCoreProfile (100 MHz single-core RT-Thread product): plain
//     index/head inside ONE irq-mask critical section (rt_hw_interrupt_disable/
//     enable). No CAS / ABA tag / backoff; used & high-watermark update in the
//     same CS and stats reads go through the same CS.
//   - HostSmpProfile (POSIX TSan / SMP semantics): 32-bit tagged atomic head
//     ([15:0] free block index, [31:16] ABA tag) + CAS with acquire/release
//     publication - acquire when allocating, release when reclaiming. Requires
//     a lock-free 32-bit CAS (design §13.1) and, on an SMP host, a real
//     serializing CriticalSection (make_spin_critical_section, design §7.3).
//
// The block stride / base alignment is configurable via EventPool's BlockAlign
// parameter (default alignof(std::max_align_t)) so a 32-bit target can stride
// to a payload alignment larger than max_align_t.
//
// The index + CAS algorithm is adapted from newosp include/osp/data_dispatcher.hpp
// (MIT, Copyright (c) 2024 liudegui, [15:0] index / [31:16] tag packing for
// 32-bit ARM) and the reference-count semantics follow QP/C++ QF gc()/newRef_
// (src/qf/qf_dyn.cpp). Project license: MIT.
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

#include "coact/assert.hpp"
#include "coact/event.hpp"
#include "coact/pal.hpp"

namespace coact {

// ---------------------------------------------------------------------------
// Compile-time pool synchronization profiles (design §7.4). The board config
// instantiates pool / staging / reclaimer / monitor with ONE profile; mixing
// profiles across components is forbidden. Both profiles select the backend of
// the SAME EventPool template (third template parameter); they only change the
// internal synchronization algorithm, never the public lifecycle semantics.
// ---------------------------------------------------------------------------

// 100 MHz single-core RT-Thread product: claim / reclaim / free-list update /
// used & high-watermark all happen inside ONE irq-mask critical section, so
// the backend does no CAS, no ABA tag, no backoff and does not rely on 64 B
// cache-line padding. Stats reads also go through the same critical section.
//
// Reclaimer strategy (design §7.4): the first product baseline uses IMMEDIATE
// reclaim (no batch chaining) to lower first-round correctness risk; a board
// may switch to batched after proving a stable real-device win.
struct RttSingleCoreProfile {
    static constexpr bool kUseBatchedReclaim = false;
};

// POSIX SMP / TSan semantics verification: tagged atomic head + CAS. The
// free-list head is acquired when allocating (loading the head) and released
// when reclaiming (storing the head); it must not rely on relaxed ordering or
// one CPU's accidental behavior (design §7.4). Requires a lock-free 32-bit
// atomic (design §13.1). On SMP hosts the pool must be bound to a real
// serializing CriticalSection (make_spin_critical_section, design §7.3).
//
// Reclaimer strategy: BATCHED, matching the concurrent verification profile.
struct HostSmpProfile {
    static constexpr bool kUseBatchedReclaim = true;
};

// coact::Event is the raw block header the pool placement-news and recycles
// with no destroy hook: it must stay trivially copyable and destructible.
static_assert(std::is_trivially_copyable<Event>::value &&
                  std::is_trivially_destructible<Event>::value,
              "coact::Event must remain trivially copyable/destructible");

namespace detail {

// Selects the single-core plain (irq-mask-guarded) backend for a Profile.
template <typename Profile>
inline constexpr bool is_single_core_pool_profile_v =
    std::is_same<Profile, coact::RttSingleCoreProfile>::value;

// ReclaimBatcher per-batch pool capacity (design §7.4): min(kBatchSizeMax,
// kMaxEventPools), never a hard-coded 4. A dispatch batch dequeues at most
// Config::kBatchSizeMax events, each belonging to one pool, and only
// kMaxEventPools pools can exist, so this is the maximum number of distinct
// pools a single batch can touch.
template <typename Config>
inline constexpr uint16_t reclaimer_pool_capacity() noexcept
{
    const uint16_t batch = static_cast<uint16_t>(Config::kBatchSizeMax);
    const uint16_t pools = static_cast<uint16_t>(kMaxEventPools);
    return (batch < pools) ? batch : pools;
}

inline constexpr uint32_t pool_index_invalid = 0xFFFFU;  // [15:0] empty sentinel
inline constexpr uint32_t pool_tag_shift = 16U;
// Reclaim is batched in the Dispatcher to collapse many free_head CAS ops into
// a single splice (see ReclaimBatcher). Events are staged in a per-pool chain
// and spliced once per kReclaimBatchCap reclaimed blocks, or at batch end.
inline constexpr uint16_t kReclaimBatchCap = 16U;

// Historical ReclaimBatcher per-batch pool capacity, kept as the default for
// callers that do not introduce a Config (design §7.4: the capacity is
// normally min(kBatchSizeMax, kMaxEventPools) when a Config is present).
inline constexpr uint8_t kDefaultReclaimBatcherPools = 4U;

inline uint32_t pool_pack_head(uint32_t index, uint32_t tag) noexcept
{
    return ((tag & 0xFFFFU) << pool_tag_shift) | (index & 0xFFFFU);
}
inline uint32_t pool_head_index(uint32_t head) noexcept
{
    return head & 0xFFFFU;
}
inline uint32_t pool_head_tag(uint32_t head) noexcept
{
    return (head >> pool_tag_shift) & 0xFFFFU;
}

// Block address model is integer arithmetic (uintptr_t) over the aligned base:
// the fixed-stride free list maps index <-> address with native uint32 CAS on
// 32-bit MCUs. integer<->pointer conversions here (MISRA C++ 5-2-8 class) are
// a recorded allocator deviation; object byte-views elsewhere go through void*.
inline uintptr_t pool_block_base(uintptr_t base, uint32_t index,
                                 uint32_t stride) noexcept
{
    return base + static_cast<uintptr_t>(index) * stride;
}

// Bounded backoff between failed CAS attempts on the shared free-list head.
// A contended Treiber head turns every loser into an immediate re-CAS, which
// multiplies exclusive cache-line traffic on SMP hosts. Spinning on relaxed
// loads (sharable) instead of CAS (exclusive) lets the winner's write land
// and the line re-quiesce before retrying. Pure std::atomic, so it compiles
// on single-core RT-Thread targets where it stays effectively a no-op.
// The head is a 32-bit packed index+tag (native CAS on x86 and ARM Cortex-M).
inline void pool_backoff(uint32_t& head,
                         const std::atomic<uint32_t>& free_head,
                         uint32_t& backoff_rounds) noexcept
{
    ++backoff_rounds;
    if (backoff_rounds > 3U) {   // cap at 2^3=8 relaxed loads: longer spins just spin on
        backoff_rounds = 3U;     // a hot contended line, multiplying cache misses
    }
    const uint32_t width = 1U << backoff_rounds;
    for (uint32_t i = 0U; i < width; ++i) {
        head = free_head.load(std::memory_order_relaxed);
    }
}

// Round a block size up to a multiple of `Align` (the pool's stride
// alignment). The pool's BlockAlign template parameter defaults to
// alignof(std::max_align_t) and is configurable so a 32-bit target can stride
// to a payload alignment larger than max_align_t (design §13.6.2).
template <size_t Align>
inline constexpr size_t pool_block_align(size_t size) noexcept
{
    static_assert((Align & (Align - 1U)) == 0U,
                  "pool alignment must be a power of two");
    return (size + Align - 1U) & ~(Align - 1U);
}
inline constexpr size_t pool_block_align(size_t size) noexcept
{
    return pool_block_align<alignof(std::max_align_t)>(size);
}

inline void pool_store_next(uintptr_t addr, uint32_t next) noexcept
{
    std::memcpy(reinterpret_cast<void*>(addr), &next, sizeof(next));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) uintptr_t block address -> void* (recorded allocator deviation)
}

// Splice a caller-linked chain [first..last] (count blocks, all belonging to
// the pool `rec`) back onto the free list in ONE CAS on the shared head. The
// blocks are linked via their own `next` fields (set by the caller). This is
// the batching primitive: N reclaims collapse to a single head RMW, which is
// the core win against single-shared-head contention (multi-producer, one
// reclaimer). Lock-free, wrapped in the pool's injected CriticalSection.
inline void pool_reclaim_chain(PoolRecord* rec, uint32_t first,
                               uint32_t last, uint16_t count) noexcept
{
    const CriticalSection::Token tok = rec->cs.save(rec->cs.ctx);

    uint32_t head = rec->free_head.load(std::memory_order_relaxed);
    uint32_t backoff_rounds = 0U;
    uint32_t new_head = 0U;
    for (;;) {
        const uintptr_t last_addr = pool_block_base(rec->base, last, rec->block_size);
        pool_store_next(last_addr, pool_head_index(head));
        new_head = pool_pack_head(first, pool_head_tag(head) + 1U);
        // Release publishes the chain's `next`-field writes so an acquire-popping
        // alloc observes them (design §7.4: no relaxed ordering).
        if (rec->free_head.compare_exchange_weak(
                head, new_head,
                std::memory_order_release, std::memory_order_relaxed)) {
            break;
        }
        pool_backoff(head, rec->free_head, backoff_rounds);
    }
    rec->used.fetch_sub(count, std::memory_order_relaxed);

    rec->cs.restore(rec->cs.ctx, tok);
}

// Default no-op critical section for host-only tests. Production pools bind the
// RT-Thread irq mask (or POSIX CAS) hook; single-threaded unit tests inject the
// no-op so the pool still works without a real interrupt gate.
inline CriticalSection noop_cs() noexcept
{
    CriticalSection cs;
    cs.save = [](void*) -> CriticalSection::Token { return 0U; };
    cs.restore = [](void*, CriticalSection::Token) {};
    return cs;
}

// ---------------------------------------------------------------------------
// C++17 object-lifecycle contract for the typed allocation entry (design §7.2 /
// §7.4, interfaces §5.3). The pool has no payload destroy hook, so only
// trivially copyable + trivially destructible payloads may enter a block.
// ---------------------------------------------------------------------------

// Payload gate: trivial types only. Non-trivial types are forbidden because
// reclaim cannot run a payload destructor.
template <typename Payload>
inline constexpr bool is_trivially_poolable_v =
    std::is_trivially_copyable<Payload>::value &&
    std::is_trivially_destructible<Payload>::value;

// Pure compile-time evaluation of the composed-layout contract (no asserts).
// `Layout` is a standard-layout composition whose first member is exactly
// coact::Event (offset 0), with a `payload` region that can hold `Payload` in a
// pool of `BlockSize` bytes whose stride is aligned to `BlockAlign` (defaults to
// alignof(std::max_align_t)). Returns false (not a hard error) so tests can
// assert both directions; EventPool::alloc_typed turns it into a static_assert.
// This trait is the single contract anchor for the typed allocation entry.
//
// offsetof is only evaluated for standard-layout layouts: the if-constexpr
// guards keep offsetof off non-standard-layout types (which would trigger
// -Winvalid-offsetof) and short-circuit the contract to false instead.
template <typename Layout>
inline constexpr bool layout_event_at_zero() noexcept
{
    if constexpr (std::is_standard_layout<Layout>::value) {
        return offsetof(Layout, event) == 0U;
    }
    return false;
}

template <typename Layout>
inline constexpr bool layout_payload_aligned(size_t payload_align) noexcept
{
    if constexpr (std::is_standard_layout<Layout>::value) {
        return (offsetof(Layout, payload) % payload_align) == 0U;
    }
    return false;
}

template <typename Layout, typename Payload, size_t PayloadAlign,
          size_t BlockSize, size_t BlockAlign = alignof(std::max_align_t)>
inline constexpr bool event_layout_complies_v =
    std::is_standard_layout<Layout>::value &&
    layout_event_at_zero<Layout>() &&
    std::is_same<decltype(Layout::event), Event>::value &&
    (sizeof(Layout) <= BlockSize) &&
    (alignof(Layout) <= BlockAlign) &&
    (alignof(Layout) >= PayloadAlign) &&
    (alignof(Payload) <= PayloadAlign) &&
    layout_payload_aligned<Layout>(alignof(Payload)) &&
    is_trivially_poolable_v<Payload>;

}  // namespace detail

// Composed event-block layout blueprint (design §7.2 / interfaces §5.3):
// coact::Event owns the first bytes of the block, then an application meta
// region, then an aligned payload buffer. Standard-layout, event at offset 0.
// Used ONLY for compile-time size/alignment/offset computation and as the
// typed-alloc return view; never instantiated as an object array. Access
// meta/payload through this view; down-casting coact::Event* to a derived
// event object is forbidden (checked by event_layout_complies_v).
template <typename Meta, size_t PayloadBytes, size_t PayloadAlign>
struct EventBlockLayout {
    Event event;
    Meta meta;
    alignas(PayloadAlign) std::byte payload[PayloadBytes];
};

// Byte offset of the application meta region inside a composed layout.
template <typename Layout>
inline constexpr size_t event_block_meta_offset() noexcept
{
    return offsetof(Layout, meta);
}

// Byte offset of the payload region inside a composed layout.
template <typename Layout>
inline constexpr size_t event_block_payload_offset() noexcept
{
    return offsetof(Layout, payload);
}

// Reference-count helpers (QP QF semantics, see contract 4.1). Call
// event_ref_inc before every post (queue/direct) and event_gc after a
// consumer finishes dispatch. The last gc (ref_ctr reaching 0) returns a
// pool event to its original pool; static events (pool_id == 0) are never
// touched.
inline void event_ref_inc(Event* e) noexcept
{
    COACT_ASSERT(e != nullptr);
    if (e->pool_id != 0U) {
        COACT_ASSERT(e->ref_ctr < 0xFFU);
        ++e->ref_ctr;
    }
}

inline void event_gc(Event* e) noexcept
{
    COACT_ASSERT(e != nullptr);
    if (COACT_UNLIKELY(e->pool_id == 0U)) {
        return;  // static event: never recycled
    }
    if (COACT_LIKELY(e->ref_ctr > 0U)) {
        --e->ref_ctr;
    }
    if (COACT_LIKELY(e->ref_ctr == 0U)) {
        PoolRecord* rec = pool_record(e->pool_id);
        COACT_ASSERT(rec != nullptr);
        rec->reclaim(e);
    }
}

// Fixed-capacity mutable-event pool. The synchronization backend is selected at
// compile time by `Profile` (design §7.4):
//   - RttSingleCoreProfile: plain index/head inside one irq-mask critical
//     section; no CAS / ABA tag / backoff. The injected CriticalSection
//     serializes claim/reclaim and the used/high-watermark updates.
//   - HostSmpProfile: tagged atomic head + CAS with acquire/release
//     publication (acquire on alloc, release on reclaim), lock-free 32-bit CAS
//     required (design §13.1).
// `BlockAlign` is the pool's stride/base alignment (default max_align_t),
// configurable for payload alignments larger than max_align_t.
//
// alloc() and reclaim() are safe concurrently from multiple producers, and the
// head RMW is guarded by an injected CriticalSection (irq mask on single-core,
// spinlock on SMP host) so the backend never interleaves mid-operation.
template <uint16_t BlockSize, uint16_t Capacity,
          class Profile = HostSmpProfile,
          size_t BlockAlign = alignof(std::max_align_t)>
class EventPool {
public:
    static_assert(std::is_same<Profile, RttSingleCoreProfile>::value ||
                      std::is_same<Profile, HostSmpProfile>::value,
                  "coact: unknown pool synchronization profile (design 7.4)");
    static_assert(!std::is_same<Profile, HostSmpProfile>::value ||
                      std::atomic<uint32_t>::is_always_lock_free,
                  "coact: HostSmpProfile requires a lock-free 32-bit CAS "
                  "(design 13.1)");
    static_assert(Capacity > 0U, "EventPool requires a non-zero capacity");
    static_assert(Capacity < 0xFFFFU,
                  "EventPool capacity must fit the 16-bit free-list index");
    static_assert(BlockSize >= sizeof(Event),
                  "EventPool block must fit an Event");
    static_assert(detail::pool_block_align<BlockAlign>(BlockSize) <= 0xFFFFU,
                  "aligned block stride must fit uint16_t");

    EventPool() noexcept
        : record_{}, cs_{nullptr, nullptr, nullptr}
    {
    }

    // Initialize the lock-free indexed free list from external storage, bind
    // the platform critical-section hook, and register this pool. The injected
    // CriticalSection is the same hook the single-core queue backend uses:
    // RT-Thread maps it to irq mask, POSIX tests inject no-ops.
    void init(void* storage, size_t bytes,
              CriticalSection cs = detail::noop_cs()) noexcept
    {
        const size_t stride = detail::pool_block_align<BlockAlign>(BlockSize);
        const size_t align = BlockAlign;

        const uintptr_t raw_base = reinterpret_cast<uintptr_t>(storage);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) object -> integer for alignment math (recorded allocator deviation)
        const uintptr_t begin = (raw_base + align - 1U) & ~(align - 1U);
        const uintptr_t end = raw_base + bytes;

        size_t count = 0U;
        if (storage != nullptr && end >= begin) {
            count = static_cast<size_t>(end - begin) / stride;
            if (count > Capacity) {
                count = Capacity;
            }
        }
        if (count == 0U) {
            return;  // storage too small: leave the pool empty
        }
        const uint32_t n = static_cast<uint32_t>(count);

        // Chain free blocks by index: free[i].next = i+1, last -> invalid.
        for (uint32_t i = 0U; i < n; ++i) {
            const uint32_t next = (i + 1U < n) ? (i + 1U) : detail::pool_index_invalid;
            std::memcpy(
                reinterpret_cast<void*>(detail::pool_block_base(begin, i,  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) uintptr_t block address -> void* (recorded allocator deviation)
                                                                static_cast<uint32_t>(stride))),
                &next, sizeof(next));
        }

        cs_ = cs;
        record_.free_head.store(detail::pool_pack_head(0U, 0U),
                                std::memory_order_relaxed);
        record_.base = begin;
        record_.block_size = static_cast<uint16_t>(stride);
        record_.capacity = static_cast<uint16_t>(n);
        record_.used.store(0U, std::memory_order_relaxed);
        record_.high_watermark.store(0U, std::memory_order_relaxed);
        record_.reclaim = &EventPool::reclaim;
        record_.owner = this;
        record_.cs = cs;

        pool_id_ = register_pool(&record_);
        COACT_ASSERT(pool_id_ != 0U);  // registry full: configuration error
    }

    // Take a free block and begin the coact::Event lifetime via placement-new,
    // then initialize signal/ref_ctr and bind pool_id. Returns nullptr when the
    // pool is exhausted. Multiple producers may call concurrently. The block is
    // raw storage while idle (free-list `next` lives in the head); every alloc
    // re-starts the Event object, so no stale field is observable.
    Event* alloc(uint16_t signal) noexcept
    {
        return alloc_with_margin(signal, 0U);
    }

    // Take a free block only when at least `margin` blocks remain free after
    // the claim. The margin check and the free-list pop share one critical
    // section, so concurrent allocators cannot consume a reserved block.
    // A margin greater than or equal to capacity rejects every allocation.
    Event* alloc_with_margin(uint16_t signal, uint16_t margin) noexcept
    {
        uint32_t claimed = detail::pool_index_invalid;
        uint16_t used_after_claim = 0U;

        if constexpr (detail::is_single_core_pool_profile_v<Profile>) {
            // RttSingleCoreProfile: plain LIFO pop. The irq-mask CS already
            // serializes everything, so no CAS / ABA tag / backoff; used and
            // high-watermark are updated in the same critical section.
            const CriticalSection::Token tok = cs_.save(cs_.ctx);
            const uint32_t head = record_.free_head.load(std::memory_order_relaxed);
            const uint32_t idx = detail::pool_head_index(head);
            const uint16_t used = record_.used.load(std::memory_order_relaxed);
            const uint16_t capacity = record_.capacity;
            if (COACT_UNLIKELY(detail::pool_index_invalid != idx) &&
                ((static_cast<uint32_t>(used) + static_cast<uint32_t>(margin)) <
                 static_cast<uint32_t>(capacity))) {
                record_.free_head.store(
                    detail::pool_pack_head(load_next(idx), 0U),
                    std::memory_order_relaxed);
                const uint16_t u = static_cast<uint16_t>(used + 1U);
                record_.used.store(u, std::memory_order_relaxed);
                if (u > record_.high_watermark.load(std::memory_order_relaxed)) {
                    record_.high_watermark.store(u, std::memory_order_relaxed);
                }
                claimed = idx;
            }
            cs_.restore(cs_.ctx, tok);
        } else {
            // HostSmpProfile: tagged-CAS free-list pop. Acquire on the head so
            // the block's `next` field written by a releasing reclaimer is
            // visible (design §7.4: no relaxed ordering).
            const CriticalSection::Token tok = cs_.save(cs_.ctx);
            uint16_t used = record_.used.load(std::memory_order_relaxed);
            bool reserved = false;
            while ((static_cast<uint32_t>(used) + static_cast<uint32_t>(margin)) <
                   static_cast<uint32_t>(record_.capacity)) {
                const uint16_t next_used = static_cast<uint16_t>(used + 1U);
                if (record_.used.compare_exchange_weak(
                        used, next_used,
                        std::memory_order_acquire, std::memory_order_relaxed)) {
                    used_after_claim = next_used;
                    reserved = true;
                    break;
                }
            }

            uint32_t head = record_.free_head.load(std::memory_order_acquire);
            uint32_t backoff_rounds = 0U;
            while (reserved) {
                const uint32_t idx = detail::pool_head_index(head);
                if (COACT_UNLIKELY(detail::pool_index_invalid == idx)) {
                    record_.used.fetch_sub(1U, std::memory_order_release);
                    used_after_claim = 0U;
                    break;
                }
                const uint32_t nxt = load_next(idx);
                const uint32_t new_head = detail::pool_pack_head(
                    nxt, detail::pool_head_tag(head) + 1U);
                if (record_.free_head.compare_exchange_weak(
                        head, new_head,
                        std::memory_order_acquire, std::memory_order_relaxed)) {
                    claimed = idx;
                    break;
                }
                detail::pool_backoff(head, record_.free_head, backoff_rounds);
            }
            cs_.restore(cs_.ctx, tok);

            if (detail::pool_index_invalid != claimed) {
                uint16_t h = record_.high_watermark.load(std::memory_order_relaxed);
                while (used_after_claim > h &&
                       !record_.high_watermark.compare_exchange_weak(
                           h, used_after_claim, std::memory_order_relaxed)) {
                }
            }
        }

        if (detail::pool_index_invalid != claimed) {
            return ::new (reinterpret_cast<void*>(detail::pool_block_base(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) uintptr_t block address -> void* (recorded allocator deviation)
                             record_.base, claimed, record_.block_size)))
                Event{signal, pool_id_, 0U};
        }
        return nullptr;
    }

    // Typed allocation over a composed event-block layout (design §7.2 /
    // interfaces §5.3): claims a block (placement-news coact::Event + fields),
    // then placement-news `Payload` into the layout's payload region with
    // value-initialization (zero bytes - the nanopb `*_init_zero` semantics for
    // a trivial POD). Returns a view over the block, or nullptr when the pool
    // is exhausted (the caller distinguishes alloc failure from decode).
    //
    // The caller owns the meta region: it must placement-new the application
    // meta before reading it, then run pb_decode over the constructed payload.
    //
    // Compile-time contract enforced here (via detail::event_layout_complies_v):
    //   - Layout is standard-layout, first member exactly coact::Event (offset 0)
    //     - derived-event / down-cast views are rejected
    //   - sizeof(Layout) <= BlockSize and the pool stride covers alignof(Layout)
    //   - alignof(Payload) <= PayloadAlign and the payload region is aligned
    //   - Payload is trivially copyable + trivially destructible
    template <typename Layout, typename Payload, size_t PayloadAlign>
    Layout* alloc_typed(uint16_t signal) noexcept
    {
        return alloc_typed_with_margin<Layout, Payload, PayloadAlign>(signal, 0U);
    }

    // Typed allocation with the same admission margin as alloc_with_margin().
    // The layout contract remains identical to alloc_typed(), so a reserved
    // allocation cannot bypass payload alignment or lifecycle validation.
    template <typename Layout, typename Payload, size_t PayloadAlign>
    Layout* alloc_typed_with_margin(uint16_t signal, uint16_t margin) noexcept
    {
        static_assert(detail::event_layout_complies_v<
                          Layout, Payload, PayloadAlign, BlockSize, BlockAlign>,
                      "coact: event-block layout violates the pool lifecycle "
                      "contract (standard-layout, exact coact::Event at offset "
                      "0, fits BlockSize, alignment covered, trivial payload)");
        Event* e = alloc_with_margin(signal, margin);
        if (nullptr == e) {
            return nullptr;
        }
        void* payload_slot = static_cast<std::byte*>(
            static_cast<void*>(e)) + offsetof(Layout, payload);
        ::new (payload_slot) Payload{};
        return static_cast<Layout*>(static_cast<void*>(e));
    }

    uint16_t used() const noexcept
    {
        if constexpr (detail::is_single_core_pool_profile_v<Profile>) {
            // Stats snapshot must also travel through the irq-mask CS so a
            // reader gets a consistent view (design §7.4).
            if (nullptr != cs_.save) {
                const CriticalSection::Token tok = cs_.save(cs_.ctx);
                const uint16_t u = record_.used.load(std::memory_order_relaxed);
                cs_.restore(cs_.ctx, tok);
                return u;
            }
            return record_.used.load(std::memory_order_relaxed);
        }
        return record_.used.load(std::memory_order_relaxed);
    }
    uint16_t capacity() const noexcept
    {
        return record_.capacity;
    }
    uint16_t high_watermark() const noexcept
    {
        if constexpr (detail::is_single_core_pool_profile_v<Profile>) {
            if (nullptr != cs_.save) {
                const CriticalSection::Token tok = cs_.save(cs_.ctx);
                const uint16_t h = record_.high_watermark.load(
                    std::memory_order_relaxed);
                cs_.restore(cs_.ctx, tok);
                return h;
            }
            return record_.high_watermark.load(std::memory_order_relaxed);
        }
        return record_.high_watermark.load(std::memory_order_relaxed);
    }

private:
    uint32_t load_next(uint32_t idx) const noexcept
    {
        uint32_t next = detail::pool_index_invalid;
        std::memcpy(&next,
                    reinterpret_cast<void*>(detail::pool_block_base(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) uintptr_t block address -> void* (recorded allocator deviation)
                        record_.base, idx, record_.block_size)),
                    sizeof(next));
        return next;
    }

    static void store_next(uintptr_t addr, uint32_t next) noexcept
    {
        std::memcpy(reinterpret_cast<void*>(addr), &next, sizeof(next));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) uintptr_t block address -> void* (recorded allocator deviation)
    }

    // Reclaim callback bound into PoolRecord::reclaim. Routed through the
    // event's pool_id so one static function serves every instance of this
    // instantiation. The head push is guarded by the injected CriticalSection
    // (irq mask on single-core) so it serializes against concurrent producers
    // and ISR.
    static void reclaim(Event* e) noexcept
    {
        COACT_ASSERT(e != nullptr);
        PoolRecord* rec = pool_record(e->pool_id);
        COACT_ASSERT(rec != nullptr);

        EventPool* self =
            static_cast<EventPool*>(rec->owner);
        const CriticalSection::Token tok = self->cs_.save(self->cs_.ctx);

        const uintptr_t addr = reinterpret_cast<uintptr_t>(e);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) object -> integer for block index (recorded allocator deviation)
        COACT_ASSERT(addr >= rec->base);
        const uint32_t idx = static_cast<uint32_t>((addr - rec->base)
                                                   / rec->block_size);

        if constexpr (detail::is_single_core_pool_profile_v<Profile>) {
            // Plain LIFO push inside the irq-mask CS: no CAS / ABA tag /
            // backoff; used is decremented in the same critical section.
            const uint32_t head = rec->free_head.load(std::memory_order_relaxed);
            store_next(addr, detail::pool_head_index(head));
            rec->free_head.store(detail::pool_pack_head(idx, 0U),
                                 std::memory_order_relaxed);
            rec->used.store(static_cast<uint16_t>(
                                rec->used.load(std::memory_order_relaxed) - 1U),
                            std::memory_order_relaxed);
        } else {
            // Treiber push: the `next` write is published by the release store
            // so an acquire-popping alloc observes it (design §7.4).
            uint32_t head = rec->free_head.load(std::memory_order_relaxed);
            uint32_t backoff_rounds = 0U;
            uint32_t new_head = 0U;
            for (;;) {
                store_next(addr, detail::pool_head_index(head));
                new_head = detail::pool_pack_head(
                    idx, detail::pool_head_tag(head) + 1U);
                if (rec->free_head.compare_exchange_weak(
                        head, new_head,
                        std::memory_order_release, std::memory_order_relaxed)) {
                    break;
                }
                detail::pool_backoff(head, rec->free_head, backoff_rounds);
            }
            rec->used.fetch_sub(static_cast<uint16_t>(1U),
                                std::memory_order_relaxed);
        }

        self->cs_.restore(self->cs_.ctx, tok);
    }

    PoolRecord record_;
    uint8_t pool_id_ = 0U;
    CriticalSection cs_;
};

// Immediate reclaim strategy (design §7.4, single-core start baseline): every
// final reference returns its block to the pool right away - one free-list push
// per event, no batch chaining. begin()/flush() are no-ops so the Dispatcher's
// per-batch loop is identical for either strategy. Mirrors event_gc()'s
// ref-count semantics (pool_id 0 == static, never recycled; reclaim at 0).
class ImmediateReclaimer
{
public:
    void begin() noexcept {}
    void release(Event* e) noexcept { coact::event_gc(e); }
    void flush() noexcept {}
};

// Batched reclaim for the flow in Dispatcher::run()'s dequeue loop: a single
// reclaiming thread consumes many events per batch, each carrying a final
// reference (ref_ctr reaches 0). Instead of one free_head CAS per event
// (EventPool::reclaim), defer them per pool and splice each pool's pending
// chain back with a single CAS via detail::pool_reclaim_chain. Producers that
// reclaim on the (rare) drop path keep using the immediate reclaim().
//
// Only the single reclaiming thread may touch one of these; it is not
// thread-safe by design.
//
// MaxPending is the per-batch distinct-pool capacity (design §7.4): the
// Dispatcher instantiates it with min(kBatchSizeMax, kMaxEventPools). The
// default keeps the historical 4 for callers that do not introduce a Config.
// When the table is full, release() degrades to an immediate single-block
// reclaim instead of asserting (design §7.4's second option).
template <uint16_t MaxPending = detail::kDefaultReclaimBatcherPools>
class ReclaimBatcher
{
public:
    static constexpr uint16_t kMaxPending = MaxPending;

    // Must call begin() before the dequeue batch and flush() after it.
    void begin() noexcept { active_count_ = 0; }

    // Splice every pool's pending chain back and reset the batch.
    void flush() noexcept
    {
        for (uint8_t i = 0U; i < active_count_; ++i) {
            Pending& p = pending_[i];
            if (p.count > 0U) {
                detail::pool_reclaim_chain(p.rec, p.first, p.last, p.count);
                p.count = 0U;
            }
        }
        active_count_ = 0U;
    }

    // Release an event's final reference, deferring the free_list splice.
    // Mirrors event_gc()'s ref-count semantics (pool_id 0 == static, never
    // recycled; decrement and reclaim at 0). Only for pool-owned events.
    //
    // SMP-correctness: chaining a block writes its `next` field, which a
    // concurrent alloc reads via load_next on the shared free list. On a
    // multi-core host the injected CriticalSection must therefore be a real
    // serialization (make_spin_critical_section): it is held here across the
    // chain-link AND the cap-triggered splice so alloc / reclaim / batch never
    // interleave on one block's next field. The single-core irq-mask CS is
    // also correct (alloc and reclaim are never concurrent on one core).
    void release(Event* e) noexcept
    {
        COACT_ASSERT(e != nullptr);
        if (COACT_UNLIKELY(e->pool_id == 0U)) {
            return;  // static event: never recycled
        }
        if (COACT_LIKELY(e->ref_ctr > 0U)) {
            --e->ref_ctr;
        }
        if (COACT_UNLIKELY(e->ref_ctr != 0U)) {
            return;  // a reference is still held: not yet recyclable
        }
        PoolRecord* rec = pool_record(e->pool_id);
        COACT_ASSERT(rec != nullptr);
        if (nullptr == rec) {
            return;
        }
        const CriticalSection::Token tok = rec->cs.save(rec->cs.ctx);

        const uintptr_t addr = reinterpret_cast<uintptr_t>(e);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) object -> integer for block index (recorded allocator deviation)
        COACT_ASSERT(addr >= rec->base);
        const uint32_t idx = static_cast<uint32_t>((addr - rec->base)
                                                   / rec->block_size);
        Pending* p = pending_of(rec);
        if (nullptr == p) {
            // Design §7.4 fallback: the per-batch table is full (only reachable
            // when the config-derived capacity is exceeded). Degrade to an
            // immediate single-block reclaim instead of asserting. We already
            // hold rec->cs, so the block's `next` write and the head CAS
            // serialize against a concurrent alloc exactly as a normal splice.
            splice_locked(rec, idx, idx, static_cast<uint16_t>(1U), tok);
            rec->cs.restore(rec->cs.ctx, tok);
            return;
        }
        if (p->count == 0U) {
            p->first = idx;
            p->last = idx;
        } else {
            // prepend: e->next = old first
            detail::pool_store_next(addr, p->first);
            p->first = idx;
        }
        p->count = static_cast<uint16_t>(p->count + 1U);
        if (p->count >= detail::kReclaimBatchCap) {
            splice_locked(p, tok);   // already inside rec->cs: no re-acquire
        }

        rec->cs.restore(rec->cs.ctx, tok);
    }

private:
    struct Pending {
        PoolRecord* rec = nullptr;
        uint32_t first = 0U;
        uint32_t last = 0U;
        uint16_t count = 0U;
    };

    Pending* pending_of(PoolRecord* rec) noexcept
    {
        for (uint8_t i = 0U; i < active_count_; ++i) {
            if (pending_[i].rec == rec) {
                return &pending_[i];
            }
        }
        if (active_count_ >= kMaxPending) {
            return nullptr;   // table full: caller degrades to immediate reclaim
        }
        Pending* p = &pending_[active_count_++];
        p->rec = rec;
        p->count = 0U;
        return p;
    }

    // Splice one pool's pending chain [first..last] (count blocks) onto its
    // free list. rec->cs is already held by the caller (release holds it for
    // the whole chain + splice so the block `next` writes and the head CAS
    // serialize against a concurrent alloc). The token is passed through so we
    // do NOT re-enter the CS.
    static void splice_locked(PoolRecord* rec, uint32_t first, uint32_t last,
                              uint16_t count, CriticalSection::Token tok) noexcept
    {
        uint32_t head = rec->free_head.load(std::memory_order_relaxed);
        uint32_t backoff_rounds = 0U;
        uint32_t new_head = 0U;
        for (;;) {
            const uintptr_t last_addr = detail::pool_block_base(
                rec->base, last, rec->block_size);
            detail::pool_store_next(last_addr, detail::pool_head_index(head));
            new_head = detail::pool_pack_head(
                first, detail::pool_head_tag(head) + 1U);
            // Release publishes the chain's `next`-field writes to an
            // acquire-popping alloc (design §7.4).
            if (rec->free_head.compare_exchange_weak(
                    head, new_head,
                    std::memory_order_release, std::memory_order_relaxed)) {
                break;
            }
            detail::pool_backoff(head, rec->free_head, backoff_rounds);
        }
        rec->used.fetch_sub(count, std::memory_order_relaxed);
        (void)tok;
    }

    static void splice_locked(Pending* p, CriticalSection::Token tok) noexcept
    {
        splice_locked(p->rec, p->first, p->last, p->count, tok);
        p->count = 0U;
    }

    Pending pending_[kMaxPending];
    uint8_t active_count_ = 0U;
};

// Named batched-reclaimer alias (design §7.4): ReclaimBatcher under its
// strategy name. Defaults to the historical capacity 4 when no Config is given.
template <uint16_t MaxPending = detail::kDefaultReclaimBatcherPools>
using BatchedReclaimer = ReclaimBatcher<MaxPending>;

namespace detail {

// Compile-time reclaimer strategy for a board profile (design §7.4): the
// single-core profile starts with immediate reclaim (batched may be enabled
// after a stable real-device measurement shows a win); the SMP profile uses
// batched, matching the concurrent verification tests.
template <typename Profile, uint16_t MaxPending>
using select_reclaimer_t = std::conditional_t<
    Profile::kUseBatchedReclaim,
    coact::BatchedReclaimer<MaxPending>,
    coact::ImmediateReclaimer>;

}  // namespace detail

}  // namespace coact
