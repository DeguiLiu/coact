// coact EventPool - fixed-capacity mutable-event pool with embedded free
// list, plus the QP-style reference-count helpers event_ref_inc/event_gc.
// See design 6 and implementation contract 4.1.
//
// Embedded free-list algorithm adapted from newosp
// include/osp/mem_pool.hpp (MIT, Copyright (c) 2024 liudegui). Reference-
// count semantics follow QP/C++ QF gc()/newRef_ (src/qf/qf_dyn.cpp) and the
// pool free-list follows QP QMPool (src/qf/qf_mem.cpp); both adapted and
// re-expressed against the coact PoolRecord/Event API. Project license: MIT.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <type_traits>

#include "coact/assert.hpp"
#include "coact/event.hpp"

namespace coact {

namespace detail {

inline constexpr size_t pool_block_align(size_t size) noexcept
{
    const size_t align = alignof(std::max_align_t);
    return (size + align - 1U) & ~(align - 1U);
}

// NoLock: default pool sync policy. Assumes a single allocating context (or
// external serialization). Used by single-producer unit tests.
struct NoPoolLock {
    struct Scoped {
        Scoped(NoPoolLock&) noexcept {}
    };
};

}  // namespace detail

// Thread-safe pool lock using a std::mutex (POSIX/SMP pools, per design 6.4
// "POSIX/SMP 实现使用 PAL 原子或实例 mutex"). lock/unlock never allocate; no
// exceptions under -fno-exceptions. The Scoped guard is not RAII-deferring:
// it locks on construction and unlocks on destruction.
struct PoolMutexLock {
    std::mutex mtx{};
    struct Scoped {
        explicit Scoped(PoolMutexLock& l) noexcept : lock_(l.mtx) {}
        std::unique_lock<std::mutex> lock_;
    };
};

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
    if (e->pool_id == 0U) {
        return;  // static event: never recycled
    }
    if (e->ref_ctr > 0U) {
        --e->ref_ctr;
    }
    if (e->ref_ctr == 0U) {
        PoolRecord* rec = pool_record(e->pool_id);
        COACT_ASSERT(rec != nullptr);
        rec->reclaim(e);
    }
}

// Fixed-capacity mutable-event pool. Blocks are linked by a pointer embedded
// in the first word of each free block; the live pool state lives in a
// PoolRecord registered in the global registry, so the reclaim callback is
// routed by the event's pool_id and works for any number of pool instances.
template <uint16_t BlockSize, uint16_t Capacity,
          typename LockT = detail::NoPoolLock>
class EventPool {
public:
    static_assert(Capacity > 0U, "EventPool requires a non-zero capacity");
    static_assert(BlockSize >= sizeof(Event),
                  "EventPool block must fit an Event");
    static_assert(detail::pool_block_align(BlockSize) <= 0xFFFFU,
                  "aligned block stride must fit uint16_t");

    EventPool() noexcept
        : record_{},
          pool_id_(0U)
    {
    }

    // Initialize the embedded free list from external storage and register
    // this pool in the global registry. Pools are registered once during
    // initialization; calling init twice on the same pool is not supported.
    void init(void* storage, size_t bytes) noexcept
    {
        const size_t stride = detail::pool_block_align(BlockSize);
        const size_t align = alignof(std::max_align_t);

        const uintptr_t base = reinterpret_cast<uintptr_t>(storage);
        const uintptr_t begin = (base + align - 1U) & ~(align - 1U);
        const uintptr_t end = base + bytes;

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

        // chain free blocks: block[i] -> block[i+1], last -> nullptr
        for (size_t i = 0U; i < count; ++i) {
            void* next = (i + 1U < count)
                ? reinterpret_cast<void*>(begin + (i + 1U) * stride)
                : nullptr;
            std::memcpy(reinterpret_cast<void*>(begin + i * stride),
                        &next, sizeof(void*));
        }

        record_.free_list = reinterpret_cast<void*>(begin);
        record_.block_size = static_cast<uint16_t>(stride);
        record_.capacity = static_cast<uint16_t>(count);
        record_.used = 0U;
        record_.high_watermark = 0U;
        record_.reclaim = &EventPool::reclaim;
        record_.lock_ptr = &lock_;

        pool_id_ = register_pool(&record_);
        COACT_ASSERT(pool_id_ != 0U);  // registry full: configuration error
    }

    // Take a free block and initialize the Event base: signal set, ref_ctr 0,
    // pool_id bound to this pool. Returns nullptr when the pool is exhausted.
    // Thread-safe when LockT provides an RAII Scoped guard (guards alloc and
    // the pool's reclaim path); with the default NoPoolLock this is single-
    // producer semantics and the caller must serialize access.
    Event* alloc(uint16_t signal) noexcept
    {
        typename LockT::Scoped guard(lock_);
        void* block = record_.free_list;
        if (block == nullptr) {
            return nullptr;
        }
        void* next = nullptr;
        std::memcpy(&next, block, sizeof(void*));
        record_.free_list = next;

        ++record_.used;
        if (record_.used > record_.high_watermark) {
            record_.high_watermark = record_.used;
        }

        Event* e = static_cast<Event*>(block);
        e->signal = signal;
        e->pool_id = pool_id_;
        e->ref_ctr = 0U;
        return e;
    }

    uint16_t used() const noexcept { return record_.used; }
    uint16_t high_watermark() const noexcept { return record_.high_watermark; }

private:
    // Reclaim callback bound into PoolRecord::reclaim. Routed through the
    // event's pool_id so one static function serves every instance of this
    // instantiation. The pool's sync guard (record_.lock_ptr, a LockT*) guards
    // the free-list critical section, so a Dispatcher reclaim is serialized
    // against a concurrent producer alloc (with LockT != NoPoolLock).
    static void reclaim(Event* e) noexcept
    {
        COACT_ASSERT(e != nullptr);
        PoolRecord* rec = pool_record(e->pool_id);
        COACT_ASSERT(rec != nullptr);
        COACT_ASSERT(rec->used > 0U);

        typename LockT::Scoped guard(
            *static_cast<LockT*>(rec->lock_ptr));

        void* block = static_cast<void*>(e);
        std::memcpy(block, &rec->free_list, sizeof(void*));
        rec->free_list = block;
        --rec->used;
    }

    PoolRecord record_;
    uint8_t pool_id_;
    LockT lock_;
};

}  // namespace coact
