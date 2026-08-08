// coact bounded queue backends (multi-producer single-consumer).
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <new>
#include <utility>

#include "coact/pal.hpp"

namespace coact {

namespace detail {

// Raw byte storage for a not-yet-constructed T, aligned for placement new.
template <typename T>
struct alignas(alignof(T)) SlotStorage {
    unsigned char bytes[sizeof(T)];
};

// MPSC cell: raw payload first, then a monotonic sequence number. Payload
// first + alignas(32) keeps the hot-path push/pop copies on 32-byte cache
// boundaries (a 24-byte StagingSlot straddles a cache line when 8-aligned).
template <typename T>
struct alignas(32) MpscCell {
    SlotStorage<T> storage;
    std::atomic<uint32_t> seq{0U};
};

// Cache-line-isolated atomic counter for head/tail hot spots.
struct alignas(64) PaddedAtomic {
    std::atomic<uint32_t> value{0U};
};

}  // namespace detail

// ---------------------------------------------------------------------------
// BoundedMpscQueue: lock-free bounded multi-producer single-consumer queue for
// SMP targets. Each slot carries a sequence number implementing a ticket
// reservation: producers reserve a position with a CAS on head, write the
// payload, then publish with a release store; the single consumer reads with
// an acquire load and recycles the slot. Head and tail live on separate cache
// lines. The algorithm follows the public Vyukov bounded queue idea (rewritten
// here); the per-slot sequence idea is borrowed from newosp spsc_ringbuffer.
//
// Capacity == 1 uses a dedicated 3-state slot machine: under the general
// sequence scheme a single slot cannot distinguish "published for position p"
// from "recycled (free for position p+1)".
// ---------------------------------------------------------------------------
template <typename T, uint16_t Capacity>
class BoundedMpscQueue {
    static_assert(Capacity > 0U, "BoundedMpscQueue capacity must be non-zero");

public:
    BoundedMpscQueue() noexcept {
        for (uint32_t i = 0U; i < static_cast<uint32_t>(Capacity); ++i) {
            cells_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    BoundedMpscQueue(const BoundedMpscQueue&) = delete;
    BoundedMpscQueue& operator=(const BoundedMpscQueue&) = delete;

    bool try_push(const T& v) noexcept {
        return push_impl(v);
    }

    bool try_push(T&& v) noexcept {
        return push_impl(std::move(v));
    }

    bool try_pop(T& out) noexcept {
        if constexpr (Capacity == 1U) {
            return pop_single(out);
        }
        else {
            return pop_general(out);
        }
    }

    uint16_t size() const noexcept {
        if constexpr (Capacity == 1U) {
            return static_cast<uint16_t>(
                (cells_[0].seq.load(std::memory_order_relaxed) == kSinglePublished)
                    ? 1U
                    : 0U);
        }
        else {
            const uint32_t h = head_.value.load(std::memory_order_relaxed);
            const uint32_t t = tail_.value.load(std::memory_order_relaxed);
            const uint32_t n = h - t;
            return (n > static_cast<uint32_t>(Capacity))
                       ? Capacity
                       : static_cast<uint16_t>(n);
        }
    }

    static constexpr uint16_t capacity() noexcept {
        return Capacity;
    }

private:
    enum : uint32_t {
        kSingleEmpty = 0U,      // capacity-1 slot: no item
        kSingleReserved = 1U,   // capacity-1 slot: producer writing payload
        kSinglePublished = 2U   // capacity-1 slot: item ready
    };

    template <typename U>
    bool push_impl(U&& v) noexcept {
        if constexpr (Capacity == 1U) {
            return push_single(std::forward<U>(v));
        }
        else {
            return push_general(std::forward<U>(v));
        }
    }

    template <typename U>
    bool push_general(U&& v) noexcept {
        uint32_t pos = head_.value.load(std::memory_order_relaxed);
        for (;;) {
            detail::MpscCell<T>& cell = cells_[pos % Capacity];
            const uint32_t seq = cell.seq.load(std::memory_order_acquire);
            const int32_t dif = static_cast<int32_t>(seq - pos);
            if (dif == 0) {
                if (head_.value.compare_exchange_weak(
                        pos, pos + 1U,
                        std::memory_order_relaxed, std::memory_order_relaxed)) {
                    break;   // position pos reserved by this producer
                }
            }
            else if (dif < 0) {
                return false;   // full: consumer has not recycled this slot
            }
            else {
                pos = head_.value.load(std::memory_order_relaxed);
            }
        }
        T* slot = reinterpret_cast<T*>(&cells_[pos % Capacity].storage);
        new (static_cast<void*>(slot)) T(std::forward<U>(v));
        cells_[pos % Capacity].seq.store(pos + 1U, std::memory_order_release);
        return true;
    }

    bool pop_general(T& out) noexcept {
        const uint32_t pos = tail_.value.load(std::memory_order_relaxed);
        detail::MpscCell<T>& cell = cells_[pos % Capacity];
        const uint32_t seq = cell.seq.load(std::memory_order_acquire);
        if (seq != pos + 1U) {
            return false;   // empty: slot not yet published
        }
        T* slot = reinterpret_cast<T*>(&cell.storage);
        out = std::move(*slot);
        slot->~T();
        cell.seq.store(pos + static_cast<uint32_t>(Capacity), std::memory_order_release);
        tail_.value.store(pos + 1U, std::memory_order_relaxed);
        return true;
    }

    template <typename U>
    bool push_single(U&& v) noexcept {
        uint32_t expected = kSingleEmpty;
        if (!cells_[0].seq.compare_exchange_strong(
                expected, kSingleReserved,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return false;   // single slot busy (reserved or published) -> full
        }
        T* slot = reinterpret_cast<T*>(&cells_[0].storage);
        new (static_cast<void*>(slot)) T(std::forward<U>(v));
        cells_[0].seq.store(kSinglePublished, std::memory_order_release);
        return true;
    }

    bool pop_single(T& out) noexcept {
        if (cells_[0].seq.load(std::memory_order_acquire) != kSinglePublished) {
            return false;   // empty
        }
        T* slot = reinterpret_cast<T*>(&cells_[0].storage);
        out = std::move(*slot);
        slot->~T();
        cells_[0].seq.store(kSingleEmpty, std::memory_order_release);
        return true;
    }

    detail::PaddedAtomic head_;   // producer ticket counter (contended)
    detail::PaddedAtomic tail_;   // consumer position (single writer)
    alignas(64) detail::MpscCell<T> cells_[Capacity];
};

// ---------------------------------------------------------------------------
// SingleCoreCriticalRing: bounded single-core queue guarded by an injected
// interrupt critical section (e.g. rt_hw_interrupt_disable/enable). Inside the
// critical section only fixed indices and slot state are touched; no user
// callbacks are invoked. Suitable for RT-Thread single-core ISR producers.
// ---------------------------------------------------------------------------
template <typename T, uint16_t Capacity>
class SingleCoreCriticalRing {
    static_assert(Capacity > 0U, "SingleCoreCriticalRing capacity must be non-zero");

public:
    explicit SingleCoreCriticalRing(CriticalSection cs) noexcept
        : cs_(cs) {
    }

    SingleCoreCriticalRing(const SingleCoreCriticalRing&) = delete;
    SingleCoreCriticalRing& operator=(const SingleCoreCriticalRing&) = delete;

    bool try_push(T&& v) noexcept {
        const CriticalSection::Token token = cs_.save(cs_.ctx);
        bool ok = false;
        if (count_ < Capacity) {
            const uint32_t index = static_cast<uint32_t>(write_index_)
                                 + static_cast<uint32_t>(count_);
            T* slot = reinterpret_cast<T*>(&cells_[index % Capacity]);
            new (static_cast<void*>(slot)) T(std::move(v));
            ++count_;
            ok = true;
        }
        cs_.restore(cs_.ctx, token);
        return ok;
    }

    bool try_pop(T& out) noexcept {
        const CriticalSection::Token token = cs_.save(cs_.ctx);
        bool ok = false;
        if (count_ > 0U) {
            T* slot = reinterpret_cast<T*>(&cells_[write_index_]);
            out = std::move(*slot);
            slot->~T();
            write_index_ = static_cast<uint16_t>(
                (static_cast<uint32_t>(write_index_) + 1U) % Capacity);
            --count_;
            ok = true;
        }
        cs_.restore(cs_.ctx, token);
        return ok;
    }

    uint16_t size() const noexcept {
        const CriticalSection::Token token = cs_.save(cs_.ctx);
        const uint16_t n = count_;
        cs_.restore(cs_.ctx, token);
        return n;
    }

private:
    CriticalSection cs_;
    uint16_t write_index_ = 0U;
    uint16_t count_ = 0U;
    alignas(32) detail::SlotStorage<T> cells_[Capacity];
};

}  // namespace coact
