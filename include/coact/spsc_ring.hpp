// coact lock-free single-producer/single-consumer ring prototype.
// SPDX-License-Identifier: MIT
//
// Design 5.2 / 5.4 prototype (stage-1 hard checkpoint A). Independent,
// TDD-validated prototype that decides whether a generic coact::SpscRing is
// worth migrating into the cmdfw delivery path. NOT wired into any production
// call site yet.
//
// Two monotonically increasing uint16_t sequences (producer "head", consumer
// "tail") address a power-of-two slot array; slot index is
// sequence & (Capacity - 1). The full condition is the modular uint16
// difference head - tail == Capacity, so no sentinel slot is wasted. The
// producer publishes a payload with a release store to head; the consumer
// observes it with an acquire load. The consumer recycles a slot with a
// release store to tail; the producer observes it with an acquire load.
// Linux/SMP and RT-Thread single-core share the same C++ memory model; no
// volatile, plain index or platform incidental ordering is used.
//
// Sequence differences are explicitly cast to uint16_t BEFORE comparison to
// force modular 2^16 arithmetic and defeat integer promotion changing the wrap
// semantics. try_push() checks capacity first and only moves the payload on the
// success path, so a failed push leaves the caller's value valid (not
// consumed); try_pop() likewise only moves when an element is present and the
// popped slot returns to the default invalid state. try_push_observed()
// (design 5.4) fuses the push with a size-after sample in a single critical
// section-equivalent pass.
//
// Instantiation is gated on std::atomic<uint16_t>::is_always_lock_free: if the
// target toolchain ever proves 16-bit atomics non-lock-free, instantiation is
// rejected and the caller must fall back to the short irq-mask ring. libatomic
// lock fallback is never silently accepted.
#pragma once

#include <atomic>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "coact/config.hpp"

namespace coact {

namespace detail {

// Raw byte storage for a not-yet-constructed T, aligned for placement new.
template <typename T>
struct alignas(alignof(T)) SpscSlotStorage {
    unsigned char bytes[sizeof(T)];
};

}  // namespace detail

// Compile-time configuration validity for SpscRing. Exposed as a trait so
// tests can assert the boundary conditions (capacity 0/1/non-power-of-two/too
// large and non-noexcept payload types) at compile time without instantiating
// the (rejected) class.
template <typename T, uint16_t Capacity>
struct is_spsc_ring_config {
    static constexpr bool value =
        std::atomic<uint16_t>::is_always_lock_free &&
        std::is_nothrow_move_assignable<T>::value &&
        std::is_nothrow_default_constructible<T>::value &&
        (Capacity >= 2U) &&
        ((Capacity & (Capacity - 1U)) == 0U) &&
        (Capacity <= 0x7FFFU);
};

template <typename T, uint16_t Capacity>
class SpscRing final {
public:
    static_assert(is_spsc_ring_config<T, Capacity>::value,
                  "coact: SpscRing requires lock-free uint16 atomics, a "
                  "power-of-two capacity in [2, 0x7FFF], and a payload that is "
                  "nothrow-move-assignable and nothrow-default-constructible");

    SpscRing() noexcept = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // Push a payload. On failure (full) the caller's value is NOT consumed.
    [[nodiscard]] bool try_push(T&& value) noexcept
    {
        const uint16_t h = head_.load(std::memory_order_relaxed);
        const uint16_t t = tail_.load(std::memory_order_acquire);
        const uint16_t n = static_cast<uint16_t>(h - t);
        if (n >= Capacity) {
            return false;
        }
        T* slot = slot_at(h);
        new (static_cast<void*>(slot)) T(std::move(value));
        head_.store(static_cast<uint16_t>(h + 1U), std::memory_order_release);
        return true;
    }

    // Fused push + size-after (design 5.4). On success size_after is the fill
    // level at the moment of the publish store; on failure it equals the
    // current (full) level. No second tail load is needed.
    [[nodiscard]] QueueResult try_push_observed(T&& value) noexcept
    {
        const uint16_t h = head_.load(std::memory_order_relaxed);
        const uint16_t t = tail_.load(std::memory_order_acquire);
        const uint16_t n = static_cast<uint16_t>(h - t);
        if (n >= Capacity) {
            return QueueResult{false, n};
        }
        T* slot = slot_at(h);
        new (static_cast<void*>(slot)) T(std::move(value));
        head_.store(static_cast<uint16_t>(h + 1U), std::memory_order_release);
        return QueueResult{true, static_cast<uint16_t>(n + 1U)};
    }

    // Pop a payload into `out`. On failure (empty) `out` is untouched.
    [[nodiscard]] bool try_pop(T& out) noexcept
    {
        const uint16_t t = tail_.load(std::memory_order_relaxed);
        const uint16_t h = head_.load(std::memory_order_acquire);
        if (t == h) {
            return false;
        }
        T* slot = slot_at(t);
        out = std::move(*slot);
        slot->~T();
        tail_.store(static_cast<uint16_t>(t + 1U), std::memory_order_release);
        return true;
    }

    // Point-in-time fill level (relaxed read of both sequences).
    [[nodiscard]] uint16_t size() const noexcept
    {
        const uint16_t h = head_.load(std::memory_order_relaxed);
        const uint16_t t = tail_.load(std::memory_order_relaxed);
        const uint16_t n = static_cast<uint16_t>(h - t);
        return (n > Capacity) ? Capacity : n;
    }

    [[nodiscard]] static constexpr uint16_t capacity() noexcept
    {
        return Capacity;
    }

private:
    T* slot_at(uint16_t seq) noexcept
    {
        const uint16_t idx = static_cast<uint16_t>(seq & (Capacity - 1U));
        return static_cast<T*>(static_cast<void*>(&cells_[idx]));
    }

    // Producer-owned head and consumer-owned tail live on separate cache
    // lines: each sequence is written by one role and read by the other, so
    // they must not ping-pong inside the same line on SMP hosts.
    alignas(64) std::atomic<uint16_t> head_{0U};
    alignas(64) std::atomic<uint16_t> tail_{0U};
    alignas(32) detail::SpscSlotStorage<T> cells_[Capacity];
};

}  // namespace coact
