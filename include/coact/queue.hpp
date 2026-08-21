// coact bounded queue backends (multi-producer single-consumer).
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

#include "coact/config.hpp"
#include "coact/pal.hpp"

namespace coact {

namespace detail {

// Raw byte storage for a not-yet-constructed T, aligned for placement new.
template <typename T>
struct alignas(alignof(T)) SlotStorage {
    std::byte bytes[sizeof(T)];
};

template <typename T>
T* slot_ptr(SlotStorage<T>& storage) noexcept
{
    return std::launder(reinterpret_cast<T*>(storage.bytes));
}

template <typename T>
const T* slot_ptr(const SlotStorage<T>& storage) noexcept
{
    return std::launder(reinterpret_cast<const T*>(storage.bytes));
}

// MPSC cell: a producer exclusively owns a Writing cell until it publishes
// the payload. The single consumer only reads Ready cells. Payload first +
// alignas(32) keeps the hot-path push/pop copies on 32-byte cache boundaries.
template <typename T>
struct alignas(32) MpscCell {
    SlotStorage<T> storage;
    std::atomic<uint32_t> state{0U};
    std::atomic<uint64_t> ticket{0U};
};

// Cache-line-isolated atomic counter for head/tail hot spots.
struct alignas(64) PaddedAtomic {
    std::atomic<uint32_t> value{0U};
};

struct alignas(64) PaddedAtomic64 {
    std::atomic<uint64_t> value{0U};
};

}  // namespace detail

// ---------------------------------------------------------------------------
// BoundedMpscQueue: bounded multi-producer single-consumer queue for SMP
// targets. Producers claim one Free cell, construct the payload, then publish
// it as Ready. The consumer scans the fixed cell set for the earliest Ready
// publication. A producer stalled while constructing consumes only its own
// cell; it cannot hide later completed submissions behind a reservation gap.
//
// Single-thread pushes preserve FIFO order. Concurrent pushes are selected by
// the smallest publication ticket currently visible to the consumer; a later
// completed push may therefore bypass a producer that is still constructing.
// No per-producer FIFO order is promised under concurrent publication. All
// operations have bounded scans and use no dynamic storage. size() counts
// Writing and Ready cells so shutdown can wait for admitted producers.
// ---------------------------------------------------------------------------
template <typename T, uint16_t Capacity>
class BoundedMpscQueue {
    static_assert(Capacity > 0U, "BoundedMpscQueue capacity must be non-zero");
    static_assert(std::atomic<uint64_t>::is_always_lock_free || (Capacity == 0U),
                  "BoundedMpscQueue requires lock-free 64-bit atomics");

public:
    BoundedMpscQueue() noexcept {
    }

    // Destruction requires all producers and the consumer to be quiescent;
    // otherwise a kWriting cell may still be between placement-new and publish.
    ~BoundedMpscQueue() noexcept {
        destroy_live_cells();
    }

    explicit BoundedMpscQueue(CriticalSection) noexcept
        : BoundedMpscQueue() {
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
        return pop_ready(out);
    }

    // Read the earliest Ready payload without consuming it. Returns false when
    // no payload is published; otherwise copies the next-to-serve slot into
    // `out`. This is a consumer-side operation and must not run concurrently
    // with try_pop(). Staging uses it to observe Low arrival metadata.
    bool front(T& out) const noexcept {
        const uint16_t index = oldest_ready_index();
        if (index == Capacity) {
            return false;
        }
        out = *detail::slot_ptr(cells_[index].storage);
        return true;
    }

    uint16_t size() const noexcept {
        uint16_t count = 0U;
        for (uint16_t index = 0U; index < Capacity; ++index) {
            if (cells_[index].state.load(std::memory_order_acquire) != kFree) {
                ++count;
            }
        }
        return count;
    }

    bool has_ready() const noexcept {
        return oldest_ready_index() != Capacity;
    }

    static constexpr uint16_t capacity() noexcept {
        return Capacity;
    }

private:
    enum : uint32_t {
        kFree = 0U,
        kWriting = 1U,
        kReady = 2U,
        kConsuming = 3U
    };

    template <typename U>
    bool push_impl(U&& v) noexcept {
        const uint32_t start = producer_probe_.value.fetch_add(1U, std::memory_order_relaxed);
        for (uint16_t offset = 0U; offset < Capacity; ++offset) {
            const uint16_t index = static_cast<uint16_t>(
                (start + static_cast<uint32_t>(offset)) % Capacity);
            detail::MpscCell<T>& cell = cells_[index];
            uint32_t expected = kFree;
            if (!cell.state.compare_exchange_strong(
                    expected, kWriting,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                continue;
            }

            T* slot = detail::slot_ptr(cell.storage);
            ::new (slot) T(std::forward<U>(v));
            cell.ticket.store(publication_ticket_.value.fetch_add(1U, std::memory_order_relaxed),
                              std::memory_order_relaxed);
            cell.state.store(kReady, std::memory_order_release);
            return true;
        }
        return false;
    }

    uint16_t oldest_ready_index() const noexcept {
        uint16_t oldest = Capacity;
        uint64_t oldest_ticket = 0U;
        for (uint16_t index = 0U; index < Capacity; ++index) {
            const detail::MpscCell<T>& cell = cells_[index];
            if (cell.state.load(std::memory_order_acquire) != kReady) {
                continue;
            }
            const uint64_t ticket = cell.ticket.load(std::memory_order_relaxed);
            if (oldest == Capacity || ticket < oldest_ticket) {
                oldest = index;
                oldest_ticket = ticket;
            }
        }
        return oldest;
    }

    bool pop_ready(T& out) noexcept {
        const uint16_t index = oldest_ready_index();
        if (index == Capacity) {
            return false;
        }
        detail::MpscCell<T>& cell = cells_[index];
        cell.state.store(kConsuming, std::memory_order_relaxed);
        T* slot = detail::slot_ptr(cell.storage);
        out = std::move(*slot);
        slot->~T();
        cell.state.store(kFree, std::memory_order_release);
        return true;
    }

    void destroy_live_cells() noexcept {
        for (uint16_t index = 0U; index < Capacity; ++index) {
            detail::MpscCell<T>& cell = cells_[index];
            const uint32_t state = cell.state.load(std::memory_order_relaxed);
            if (state == kFree) {
                continue;
            }
            T* slot = detail::slot_ptr(cell.storage);
            slot->~T();
        }
    }

    detail::PaddedAtomic producer_probe_;
    detail::PaddedAtomic64 publication_ticket_;
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

    // Destruction requires the injected critical section to be quiescent.
    ~SingleCoreCriticalRing() noexcept {
        const uint16_t count = std::exchange(count_, 0U);
        const uint16_t write_index = std::exchange(write_index_, 0U);
        for (uint16_t offset = 0U; offset < count; ++offset) {
            const uint16_t index = static_cast<uint16_t>(
                (static_cast<uint32_t>(write_index) + offset) % Capacity);
            T* slot = detail::slot_ptr(cells_[index]);
            slot->~T();
        }
    }

    SingleCoreCriticalRing(const SingleCoreCriticalRing&) = delete;
    SingleCoreCriticalRing& operator=(const SingleCoreCriticalRing&) = delete;

    bool try_push(T&& v) noexcept {
        return try_push_observed(std::move(v)).success;
    }

    // Fused push + size-after (design 5.4): capacity check, payload move and
    // fill-level read all happen inside ONE irq-mask critical section, so the
    // caller (e.g. the cmdfw DeliveryTx) no longer needs a separate
    // size_locked() + try_push() pair that enters the critical section twice.
    // On success size_after is the fill level right after the push; on a full
    // failure it equals the current (full) level. A failed push does NOT
    // consume the caller's value: the payload is only moved once capacity is
    // known to be available.
    [[nodiscard]] QueueResult try_push_observed(T&& v) noexcept {
        const CriticalSection::Token token = cs_.save(cs_.ctx);
        bool ok = false;
        if (count_ < Capacity) {
            const uint32_t index = static_cast<uint32_t>(write_index_)
                                 + static_cast<uint32_t>(count_);
            T* slot = detail::slot_ptr(cells_[index % Capacity]);
            ::new (slot) T(std::move(v));
            ++count_;
            ok = true;
        }
        const uint16_t size_after = count_;
        cs_.restore(cs_.ctx, token);
        return QueueResult{ok, size_after};
    }

    bool try_pop(T& out) noexcept {
        const CriticalSection::Token token = cs_.save(cs_.ctx);
        bool ok = false;
        if (count_ > 0U) {
            T* slot = detail::slot_ptr(cells_[write_index_]);
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

    bool has_ready() const noexcept {
        return size() != 0U;
    }

    // Read the oldest (head) payload without consuming it. Returns false when
    // empty. Same critical-section scope as try_pop() so the read is atomic
    // with respect to producers on the single core.
    bool front(T& out) const noexcept {
        const CriticalSection::Token token = cs_.save(cs_.ctx);
        bool ok = false;
        if (count_ > 0U) {
            out = *detail::slot_ptr(cells_[write_index_]);
            ok = true;
        }
        cs_.restore(cs_.ctx, token);
        return ok;
    }

    // Caller must already hold the exact CriticalSection injected at
    // construction. This avoids a non-reentrant nested irq-mask/spin lock in
    // compound queue-state transitions; it never exposes storage or permits a
    // mutation outside try_push()/try_pop().
    uint16_t size_locked() const noexcept { return count_; }

private:
    CriticalSection cs_;
    uint16_t write_index_ = 0U;
    uint16_t count_ = 0U;
    alignas(32) detail::SlotStorage<T> cells_[Capacity];
};

}  // namespace coact
