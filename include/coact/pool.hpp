// coact EventPool - fixed-capacity mutable-event pool with a lock-free indexed
// free list, plus the QP-style reference-count helpers event_ref_inc/event_gc.
// See design 6 and implementation contract 4.1.
//
// The free-list head is a 32-bit tagged word: low 16 bits = free block index,
// high 16 bits = ABA tag that increments on every successful alloc/reclaim.
// A 32-bit atomic is a native CAS on x86 and on 32-bit ARM Cortex-M (LDREX/
// STREX) with NO libatomic fallback, so it stays lock-free on RT-Thread 5.2.x
// single-core targets. The head RMW is additionally wrapped in an injected
// CriticalSection (irq mask) so a single-core 100 MHz MCU guards against ISR
// preemption in O(1): RT-Thread maps save/restore to rt_hw_interrupt_disable/
// enable (the record.used/high_watermark atomics then use relaxed ordering);
// POSIX SMP injects no-op hooks and the 32-bit CAS provides the concurrency.
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

#include "coact/assert.hpp"
#include "coact/event.hpp"
#include "coact/pal.hpp"

namespace coact {

namespace detail {

inline constexpr uint32_t pool_index_invalid = 0xFFFFU;  // [15:0] empty sentinel
inline constexpr uint32_t pool_tag_shift = 16U;

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
                         unsigned& spin) noexcept
{
    ++spin;
    if (spin > 3U) {   // cap at 2^3=8 relaxed loads: longer spins just spin on
        spin = 3U;     // a hot contended line, multiplying cache misses
    }
    const unsigned width = 1U << spin;
    for (unsigned i = 0U; i < width; ++i) {
        head = free_head.load(std::memory_order_relaxed);
    }
}

inline constexpr size_t pool_block_align(size_t size) noexcept
{
    const size_t align = alignof(std::max_align_t);
    return (size + align - 1U) & ~(align - 1U);
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

}  // namespace detail

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

// Fixed-capacity lock-free mutable-event pool using a 32-bit tagged-indexed
// free list. alloc() and reclaim() are safe concurrently from multiple
// producers, and the head RMW is guarded by an injected CriticalSection so a
// single-core RT-Thread/ISR context never interleaves mid-operation.
//
// Cost: 32-bit atomic CAS (native on x86 and ARM Cortex-M) plus a save/restore
// pair per head operation. No libatomic lock on any target.
template <uint16_t BlockSize, uint16_t Capacity>
class EventPool {
public:
    static_assert(Capacity > 0U, "EventPool requires a non-zero capacity");
    static_assert(Capacity < 0xFFFFU,
                  "EventPool capacity must fit the 16-bit free-list index");
    static_assert(BlockSize >= sizeof(Event),
                  "EventPool block must fit an Event");
    static_assert(detail::pool_block_align(BlockSize) <= 0xFFFFU,
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
        const size_t stride = detail::pool_block_align(BlockSize);
        const size_t align = alignof(std::max_align_t);

        const uintptr_t raw_base = reinterpret_cast<uintptr_t>(storage);
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
                reinterpret_cast<void*>(detail::pool_block_base(begin, i,
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

        pool_id_ = register_pool(&record_);
        COACT_ASSERT(pool_id_ != 0U);  // registry full: configuration error
    }

    // Take a free block and initialize the Event base: signal set, ref_ctr 0,
    // pool_id bound to this pool. Returns nullptr when the pool is exhausted.
    // Lock-free: multiple producers may call concurrently.
    Event* alloc(uint16_t signal) noexcept
    {
        const CriticalSection::Token tok = cs_.save(cs_.ctx);
        uint32_t head = record_.free_head.load(std::memory_order_relaxed);
        unsigned spin = 0U;
        Event* out = nullptr;
        uint32_t claimed = detail::pool_index_invalid;
        for (;;) {
            const uint32_t idx = detail::pool_head_index(head);
            if (COACT_UNLIKELY(detail::pool_index_invalid == idx)) {
                break;  // pool exhausted
            }
            const uint32_t nxt = load_next(idx);
            const uint32_t new_head = detail::pool_pack_head(
                nxt, detail::pool_head_tag(head) + 1U);
            if (record_.free_head.compare_exchange_weak(
                    head, new_head,
                    std::memory_order_relaxed, std::memory_order_relaxed)) {
                claimed = idx;
                break;
            }
            detail::pool_backoff(head, record_.free_head, spin);
        }
        cs_.restore(cs_.ctx, tok);

        if (detail::pool_index_invalid != claimed) {
            Event* e = static_cast<Event*>(
                reinterpret_cast<void*>(detail::pool_block_base(
                    record_.base, claimed, record_.block_size)));
            e->signal = signal;
            e->pool_id = pool_id_;
            e->ref_ctr = 0U;
            const uint16_t u = record_.used.fetch_add(1U,
                                                      std::memory_order_relaxed) + 1U;
            uint16_t h = record_.high_watermark.load(std::memory_order_relaxed);
            while (u > h &&
                   !record_.high_watermark.compare_exchange_weak(
                       h, u, std::memory_order_relaxed)) {
            }
            out = e;
        }
        return out;
    }

    uint16_t used() const noexcept { return record_.used.load(std::memory_order_relaxed); }
    uint16_t high_watermark() const noexcept
    {
        return record_.high_watermark.load(std::memory_order_relaxed);
    }

private:
    uint32_t load_next(uint32_t idx) const noexcept
    {
        uint32_t next = detail::pool_index_invalid;
        std::memcpy(&next,
                    reinterpret_cast<void*>(detail::pool_block_base(
                        record_.base, idx, record_.block_size)),
                    sizeof(next));
        return next;
    }

    static void store_next(uintptr_t addr, uint32_t next) noexcept
    {
        std::memcpy(reinterpret_cast<void*>(addr), &next, sizeof(next));
    }

    // Reclaim callback bound into PoolRecord::reclaim. Routed through the
    // event's pool_id so one static function serves every instance of this
    // instantiation. The head push is guarded by the injected CriticalSection
    // (irq mask on single-core) so it serializes against concurrent producers
    // and ISR. Lock-free.
    static void reclaim(Event* e) noexcept
    {
        COACT_ASSERT(e != nullptr);
        PoolRecord* rec = pool_record(e->pool_id);
        COACT_ASSERT(rec != nullptr);

        EventPool* self =
            reinterpret_cast<EventPool*>(rec->owner);
        const CriticalSection::Token tok = self->cs_.save(self->cs_.ctx);

        const uintptr_t addr = reinterpret_cast<uintptr_t>(e);
        COACT_ASSERT(addr >= rec->base);
        const uint32_t idx = static_cast<uint32_t>((addr - rec->base)
                                                   / rec->block_size);

        uint32_t head = rec->free_head.load(std::memory_order_relaxed);
        unsigned spin = 0U;
        uint32_t new_head = 0U;
        for (;;) {
            store_next(addr, detail::pool_head_index(head));
            new_head = detail::pool_pack_head(
                idx, detail::pool_head_tag(head) + 1U);
            if (rec->free_head.compare_exchange_weak(
                    head, new_head,
                    std::memory_order_relaxed, std::memory_order_relaxed)) {
                break;
            }
            detail::pool_backoff(head, rec->free_head, spin);
        }
        rec->used.fetch_sub(1U, std::memory_order_relaxed);

        self->cs_.restore(self->cs_.ctx, tok);
    }

    PoolRecord record_;
    uint8_t pool_id_;
    CriticalSection cs_;
};

}  // namespace coact
