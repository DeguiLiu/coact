// coact Event, global pool registry and pool records - QP-style reference-
// counted mutable events. See design 6 and implementation contract 4.1.
//
// Model adapted from QP/C++ (Quantum Leaps) QEvt / QF event-pool registry
// (src/qf/qf_dyn.cpp, src/qf/qf_mem.cpp); the coact API is an original,
// minimal re-expression. Project license: MIT.
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>

namespace coact {

// Base of every dynamic event. pool_id == 0 marks a non-pool (static) event,
// which event_gc() never recycles. ref_ctr is the reference count: 0 right
// after alloc, +1 per post (event_ref_inc), -1 per consumer (event_gc);
// reaching 0 returns the block to its pool.
struct Event {
    uint16_t signal;
    uint8_t pool_id;   // 0 = static event; otherwise a 1-based registry index
    uint8_t ref_ctr;   // 0 after alloc; +1 per post; 0 again => recycle
};

// Packing for the pool free-list head.
//
// The free-list head is a 32-bit tagged word (native 32-bit CAS on both x86
// and 32-bit ARM Cortex-M — no libatomic fallback):[15:0] = free block index,
// [31:16] = an ABA tag that increments on every successful alloc/reclaim so a
// stale-pop cannot win. Capacity must stay below 0xFFFF (index 0xFFFF is the
// empty sentinel). Blocks are fixed-stride from the pool's aligned base, so an
// index maps to (base + index*block_size).
struct PoolRecord {
    // free_head is the only write-contended atomic (every alloc/reclaim CAS
    // targets it). Give it its own cache line so concurrent producers and the
    // reclaiming thread never invalidate each other's copy of the used/hwm
    // stats, which also change on every operation. newosp isolates the same
    // way (data_dispatcher.hpp: alignas(kCacheLineSize) free_head_ etc.).
    alignas(64) std::atomic<uint32_t> free_head{0};   // [31:16]=ABA tag, [15:0]=free index
    uintptr_t base;                       // aligned base of the block area
    uint16_t block_size;                  // aligned stride between blocks
    uint16_t capacity;
    // Start a fresh cache line so free_head's line never carries used/hwm.
    alignas(64) std::atomic<uint16_t> used{0};
    std::atomic<uint16_t> high_watermark{0};
    void (*reclaim)(Event* e);            // returns a block to this pool (lock-free)
    void* owner;                          // owning EventPool* (for its CriticalSection)
};

// Maximum number of event pools that can be registered at once. pool_id is a
// uint8_t so the registry can never exceed 255 entries.
static constexpr uint8_t kMaxEventPools = 16U;

namespace detail {

// Controlled global registry - the only permitted mutable singleton (see
// contract 0). Populated during initialization only, read-only afterwards.
inline PoolRecord* g_pool_registry[kMaxEventPools] = {};

}  // namespace detail

// Look up a pool record by 1-based pool_id. Returns nullptr for 0 (static
// events) and for unknown ids.
inline PoolRecord* pool_record(uint8_t pool_id) noexcept
{
    if (pool_id == 0U || pool_id > kMaxEventPools) {
        return nullptr;
    }
    return detail::g_pool_registry[pool_id - 1U];
}

// Register a pool record and assign it a 1-based pool_id. Returns 0 if the
// record is null or the registry is full. Idempotent for an already
// registered record.
inline uint8_t register_pool(PoolRecord* rec) noexcept
{
    if (rec == nullptr) {
        return 0U;
    }
    for (uint8_t i = 0U; i < kMaxEventPools; ++i) {
        if (detail::g_pool_registry[i] == rec) {
            return static_cast<uint8_t>(i + 1U);
        }
    }
    for (uint8_t i = 0U; i < kMaxEventPools; ++i) {
        if (detail::g_pool_registry[i] == nullptr) {
            detail::g_pool_registry[i] = rec;
            return static_cast<uint8_t>(i + 1U);
        }
    }
    return 0U;
}

}  // namespace coact
