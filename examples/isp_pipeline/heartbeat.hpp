// coact isp_pipeline heartbeat skeleton.
//
// Two cross-cutting invariants live here, each owned by its own skeleton so a
// loop body cannot skip the advice:
//
//   BeatLoop  -- "every iteration of a worker loop must report liveness"
//   ScanLoop  -- "every iteration of a main-thread wait loop must scan"
//
// Both place the advice at the same join point (after the predicate, before the
// body) so the report always precedes the blocking call: a single blocking call
// can delay at most one cycle, and none can suppress reporting entirely.
//
// Concurrency contract:
//   - Writers (worker threads) only do relaxed stores to Beat::last_us. No lock.
//   - The reader (Slots::Check) is called from the main thread only, which is
//     also the sole owner of Slot::timed_out. No lock is needed.
//   - Slots have static lifetime and are never recycled, so no generation /
//     ABA guard is required (contrast: a reusable slot table would need one).
//
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace hb {

using NowFn = uint64_t (*)();

namespace detail {
inline NowFn g_now_fn{nullptr};

/* Zero when no clock is registered: a slot then reads as maximally stale,
   which is the fail-safe direction for a watchdog. */
inline uint64_t now_us() noexcept
{
    return (nullptr != g_now_fn) ? g_now_fn() : 0U;
}
}  // namespace detail

/* Register the microsecond clock once, before the first Register(). */
inline void SetClock(NowFn fn) noexcept
{
    detail::g_now_fn = fn;
}

// ---------------------------------------------------------------------------
// Beat: the liveness timestamp. One relaxed store on the hot path.
// ---------------------------------------------------------------------------
struct Beat {
    std::atomic<uint64_t> last_us{0};

    /* Written by a worker thread, read by the main thread (Slots::Check), so
       this is a cross-thread atomic. On a target where it is not lock-free the
       operation silently pulls in libatomic -- a hidden lock plus heap, exactly
       what the conventions forbid -- so the build must fail instead. */
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "coact: the heartbeat timestamp must be lock-free on the target");

    void Hit() noexcept
    {
        last_us.store(detail::now_us(), std::memory_order_relaxed);
    }

    uint64_t LastUs() const noexcept
    {
        return last_us.load(std::memory_order_relaxed);
    }
};

// ---------------------------------------------------------------------------
// Skeleton 1: beat. Join point = a monitored thread's loop.
//
// Pred/Body are template parameters, not std::function: instantiation inlines
// the body, so there is no indirect call and no heap. A void body is terminated
// by the predicate; a bool body may also terminate itself (the original
// `break` becomes `return false`). if constexpr picks one at compile time.
// ---------------------------------------------------------------------------
template <typename Pred, typename Body>
void BeatLoop(Beat* b, Pred&& pred, Body&& body) noexcept
{
    while (pred()) {
        if (nullptr != b) {
            b->Hit();
        }
        if constexpr (std::is_same_v<decltype(body()), bool>) {
            if (!body()) {
                break;
            }
        }
        else {
            body();
        }
    }
}

// ---------------------------------------------------------------------------
// Slot table: fixed capacity, no allocation. Exists because coact has no
// per-entity heartbeat slot -- Monitor::ao() is per-AO and the workers here are
// not AOs.
// ---------------------------------------------------------------------------
struct SlotCfg {
    const char* name;        /* static lifetime; stored by pointer, not copied */
    uint32_t    timeout_us;
};

class Slots {
public:
    /* Component-level default, deliberately NOT sized to one consumer: a table
       that grew with its caller would have to be a template, and a templated
       table cannot be reached through the single g_slots pointer without a
       type-erasing interface layer. Consumers assert their own lower bound
       instead (see the static_assert next to kWorkerThreadCount in main.cpp).
       16 covers the example's 7 workers with room for board-specific extras. */
    static constexpr uint8_t kCapacity = 16U;

    /* Returns nullptr when the table is full. Callers must tolerate a null
       handle and let the beat path skip, never block startup. */
    Beat* Register(const SlotCfg& cfg) noexcept
    {
        if (nullptr == cfg.name) {
            return nullptr;
        }
        for (uint8_t i = 0U; i < kCapacity; ++i) {
            Slot& s = slots_[i];
            if (s.used) {
                continue;
            }
            s.used = true;
            s.name = cfg.name;
            s.timeout_us = cfg.timeout_us;
            s.timed_out = false;
            s.beat.Hit();               /* stamp now: no instant false positive */
            return &s.beat;
        }
        return nullptr;
    }

    void Unregister(const Beat* b) noexcept
    {
        Slot* s = find(b);
        if (nullptr != s) {
            s->used = false;
            s->timed_out = false;
        }
    }

    /* Scan every registered slot. Returns the number currently timed out and
       invokes the callback on the rising edge only, so a still-stalled slot
       does not re-report every scan. */
    uint32_t Check() noexcept
    {
        const uint64_t now = detail::now_us();
        uint32_t timed_out = 0U;
        for (uint8_t i = 0U; i < kCapacity; ++i) {
            Slot& s = slots_[i];
            if (!s.used) {
                continue;
            }
            const uint64_t last = s.beat.LastUs();
            const bool stale = (now > last) && ((now - last) > s.timeout_us);
            if (stale) {
                ++timed_out;
                if (!s.timed_out) {
                    s.timed_out = true;
                    if (nullptr != on_timeout_) {
                        on_timeout_(s.name, on_timeout_ctx_);
                    }
                }
            }
            else {
                s.timed_out = false;
            }
        }
        return timed_out;
    }

    void SetOnTimeout(void (*fn)(const char*, void*), void* ctx) noexcept
    {
        on_timeout_ = fn;
        on_timeout_ctx_ = ctx;
    }

    /* Diagnostic iteration: fn(name, last_beat_us, timed_out). */
    template <typename Fn>
    void ForEachSlot(Fn&& fn) const noexcept
    {
        for (uint8_t i = 0U; i < kCapacity; ++i) {
            if (slots_[i].used) {
                fn(slots_[i].name, slots_[i].beat.LastUs(), slots_[i].timed_out);
            }
        }
    }

private:
    struct Slot {
        Beat        beat{};
        const char* name{nullptr};
        uint32_t    timeout_us{0U};
        bool        used{false};
        bool        timed_out{false};
    };

    Slot* find(const Beat* b) noexcept
    {
        for (uint8_t i = 0U; i < kCapacity; ++i) {
            if (slots_[i].used && (&slots_[i].beat == b)) {
                return &slots_[i];
            }
        }
        return nullptr;
    }

    Slot slots_[kCapacity]{};
    void (*on_timeout_)(const char*, void*) {nullptr};
    void* on_timeout_ctx_{nullptr};
};

/* The example's single slot table, following the existing g_pal / g_session
   pointer convention. ScanLoop consults it; the pointcut is therefore fixed at
   compile time rather than passed per call site. */
inline Slots* g_slots{nullptr};

// ---------------------------------------------------------------------------
// Skeleton 2: scan. Join point = a main-thread wait loop.
//
// Same shape as BeatLoop; the advice differs. Kept as a separate skeleton
// because the two advices attach to two different loop families -- widening one
// skeleton to serve both would be the over-abstraction.
// ---------------------------------------------------------------------------
template <typename Pred, typename Body>
void ScanLoop(Pred&& pred, Body&& body) noexcept
{
    while (pred()) {
        if (nullptr != g_slots) {
            /* The count is deliberately dropped: reporting is the callback's
               job (it owns the counter), so the return value carries nothing
               the caller can act on. */
            static_cast<void>(g_slots->Check());
        }
        if constexpr (std::is_same_v<decltype(body()), bool>) {
            if (!body()) {
                break;
            }
        }
        else {
            body();
        }
    }
}

}  // namespace hb
