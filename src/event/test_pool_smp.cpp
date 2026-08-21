// coact HostSmpProfile pool-backend tests (design §7.4 / §13.1):
// SMP keeps the tagged atomic head + CAS and publishes the free-list head with
// correct memory ordering - acquire when allocating (loading the head), release
// when reclaiming (storing the head). It must NOT rely on relaxed ordering or
// one CPU's accidental behavior, and the 32-bit CAS must be lock-free on the
// toolchain (design §13.1). A multi-producer + single-reclaimer stress proves
// no duplicate alloc, no double reclaim, no payload cross-talk and no
// reference-count underflow.
//
// Per design §7.3 the SMP host pool must be bound to a real serializing
// CriticalSection (make_spin_critical_section), so the batched-reclaim chain
// writes and the plain `next`-field accesses serialize.
// SPDX-License-Identifier: MIT

#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <vector>

#include "coact/event.hpp"
#include "coact/pal.hpp"
#include "coact/pool.hpp"

#include "test/test_harness.hpp"

// Design §13.1: the SMP pool profile may not rely on a libatomic lock in the
// real-time path. This is the compile-time gate.
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "coact: HostSmpProfile requires a lock-free 32-bit atomic");

namespace {

constexpr std::uint16_t kCap = 32U;
constexpr std::uint16_t kBlock = 32U;
constexpr std::uint32_t kMagic = 0xA110CAEDU;

// Payload region after the 4-byte Event header (28 bytes on a 32-byte block).
struct Probe {
    std::uint32_t magic;
    std::uint32_t serial;
    std::uint32_t fill_a;
    std::uint32_t fill_b;
    std::uint32_t fill_c;
};

static_assert(sizeof(Probe) == 20U, "probe must fit the 28-byte payload region");

using SmpPool = coact::EventPool<kBlock, kCap, coact::HostSmpProfile>;

template <std::uint16_t Stride, std::uint16_t Capacity>
struct PoolStorage {
    alignas(64) std::uint8_t data[Stride * Capacity];
};

inline Probe* probe_of(coact::Event* e) noexcept
{
    return reinterpret_cast<Probe*>(reinterpret_cast<std::uint8_t*>(e)
                                    + sizeof(coact::Event));
}

inline void write_probe(coact::Event* e, std::uint32_t serial) noexcept
{
    Probe* p = probe_of(e);
    p->magic = kMagic;
    p->serial = serial;
    p->fill_a = serial * 2654435761U;
    p->fill_b = serial ^ 0x9E3779B9U;
    p->fill_c = ~serial;
}

// A mutex-guarded inbox: producers push allocated events, the single reclaimer
// pops and releases them. The pool is the component under test, not this box.
struct Inbox {
    std::mutex mu;
    std::vector<coact::Event*> events;
    std::vector<std::uint32_t> outstanding;  // serials currently in flight

    bool push(coact::Event* e, std::uint32_t serial)
    {
        std::lock_guard<std::mutex> g(mu);
        for (std::uint32_t s : outstanding) {
            if (s == serial) {
                return false;   // duplicate alloc: same serial twice in flight
            }
        }
        outstanding.push_back(serial);
        events.push_back(e);
        return true;
    }

    bool pop(coact::Event*& e, std::uint32_t& serial)
    {
        std::lock_guard<std::mutex> g(mu);
        if (events.empty()) {
            return false;
        }
        e = events.back();
        events.pop_back();
        serial = probe_of(e)->serial;
        for (std::size_t i = 0U; i < outstanding.size(); ++i) {
            if (outstanding[i] == serial) {
                outstanding[i] = outstanding.back();
                outstanding.pop_back();
                return true;
            }
        }
        return false;   // double reclaim: serial not outstanding
    }

    bool empty()
    {
        std::lock_guard<std::mutex> g(mu);
        return events.empty();
    }
};

}  // namespace

COACT_TEST(smp_is_always_lock_free_compile_gate)
{
    // The static_assert at TU scope already proved it; keep a runtime check so
    // the test target exists and is observable.
    CHECK(std::atomic<std::uint32_t>::is_always_lock_free);
}

COACT_TEST(smp_margin_admission_is_atomic_across_producers)
{
    constexpr std::uint16_t kMarginCap = 4U;
    constexpr std::uint16_t kMargin = 2U;
    constexpr int kProducers = 8;

    PoolStorage<kBlock, kMarginCap> storage;
    coact::SpinCriticalSection spin;
    coact::EventPool<kBlock, kMarginCap, coact::HostSmpProfile> pool;
    pool.init(storage.data, sizeof(storage.data),
              coact::make_spin_critical_section(spin));

    std::atomic<bool> go{false};
    std::atomic<std::uint16_t> accepted{0U};
    std::array<coact::Event*, kMarginCap> claimed{};
    std::array<std::thread, kProducers> producers;

    for (std::thread& producer : producers) {
        producer = std::thread([&]() {
            while (!go.load(std::memory_order_acquire)) {
            }
            coact::Event* const event = pool.alloc_with_margin(0x6000U, kMargin);
            if (event != nullptr) {
                const std::uint16_t slot = accepted.fetch_add(
                    1U, std::memory_order_relaxed);
                if (slot < kMarginCap) {
                    claimed[slot] = event;
                }
            }
        });
    }

    go.store(true, std::memory_order_release);
    for (std::thread& producer : producers) {
        producer.join();
    }

    CHECK_EQ(accepted.load(std::memory_order_relaxed), 2U);
    CHECK_EQ(pool.used(), 2U);
    for (std::uint16_t i = 0U; i < accepted.load(std::memory_order_relaxed); ++i) {
        REQUIRE(claimed[i] != nullptr);
        coact::event_gc(claimed[i]);
    }
    CHECK_EQ(pool.used(), 0U);
}

COACT_TEST(smp_mp_alloc_single_reclaimer_stress)
{
    constexpr int kProducers = 4;
    constexpr int kRounds = 4000;

    PoolStorage<kBlock, kCap> storage;
    coact::SpinCriticalSection spin;
    coact::EventPool<kBlock, kCap, coact::HostSmpProfile> pool;
    pool.init(storage.data, sizeof(storage.data),
              coact::make_spin_critical_section(spin));

    Inbox inbox;
    std::atomic<bool> go{false};
    std::atomic<bool> producers_done{false};
    std::atomic<std::uint32_t> next_serial{1U};
    std::atomic<int> failures{0};

    // Multi-producer: alloc, stamp a unique probe, inc ref, hand to reclaimer.
    std::vector<std::thread> producers;
    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back([&]() {
            while (!go.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < kRounds; ++i) {
                coact::Event* e = pool.alloc(0x5000U);
                if (nullptr == e) {
                    std::this_thread::yield();   // transiently exhausted
                    --i;
                    continue;
                }
                const std::uint32_t serial = next_serial.fetch_add(1U,
                                                                   std::memory_order_relaxed);
                write_probe(e, serial);
                // Transfer the allocation reference before the producer stops
                // touching the event.
                if (!inbox.push(e, serial)) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    // a duplicate was detected; still release so the pool drains
                }
                if ((i & 0xFF) == 0U) {
                    std::this_thread::yield();    // vary the interleaving
                }
            }
        });
    }

    // Single reclaimer (the Dispatcher role): verify then release exactly once.
    std::thread reclaimer([&]() {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (;;) {
            coact::Event* e = nullptr;
            std::uint32_t serial = 0U;
            if (inbox.pop(e, serial)) {
                Probe* p = probe_of(e);
                if (p->magic != kMagic ||
                    p->serial != serial ||
                    p->fill_a != serial * 2654435761U ||
                    p->fill_b != (serial ^ 0x9E3779B9U) ||
                    p->fill_c != ~serial) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
                coact::event_gc(e);               // ref 1 -> 0 -> reclaim
                continue;
            }
            if (producers_done.load(std::memory_order_acquire) && inbox.empty()) {
                break;
            }
            std::this_thread::yield();
        }
    });

    go.store(true, std::memory_order_release);
    for (auto& th : producers) {
        th.join();
    }
    producers_done.store(true, std::memory_order_release);
    reclaimer.join();

    CHECK_EQ(failures.load(std::memory_order_relaxed), 0);
    CHECK_EQ(static_cast<long>(inbox.outstanding.size()), 0L);   // nothing left in flight
    CHECK_EQ(pool.used(), 0U);                                   // every block returned
}

COACT_TEST(smp_reclaim_all_after_stress)
{
    // After the storm, the free list must still be intact: a full re-alloc
    // succeeds (nothing lost) and drains again (nothing duplicated).
    constexpr int kProducers = 2;
    constexpr int kRounds = 2000;

    PoolStorage<kBlock, kCap> storage;
    coact::SpinCriticalSection spin;
    coact::EventPool<kBlock, kCap, coact::HostSmpProfile> pool;
    pool.init(storage.data, sizeof(storage.data),
              coact::make_spin_critical_section(spin));

    Inbox inbox;
    std::atomic<bool> go{false};
    std::atomic<bool> producers_done{false};
    std::atomic<std::uint32_t> next_serial{1000U};

    std::vector<std::thread> producers;
    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back([&]() {
            while (!go.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < kRounds; ++i) {
                coact::Event* e = pool.alloc(1U);
                if (nullptr == e) {
                    std::this_thread::yield();
                    --i;
                    continue;
                }
                write_probe(e, next_serial.fetch_add(1U, std::memory_order_relaxed));
                inbox.push(e, probe_of(e)->serial);
            }
        });
    }
    std::thread reclaimer([&]() {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (;;) {
            coact::Event* e = nullptr;
            std::uint32_t s = 0U;
            if (inbox.pop(e, s)) {
                coact::event_gc(e);
                continue;
            }
            if (producers_done.load(std::memory_order_acquire) && inbox.empty()) {
                break;
            }
            std::this_thread::yield();
        }
    });

    go.store(true, std::memory_order_release);
    for (auto& th : producers) {
        th.join();
    }
    producers_done.store(true, std::memory_order_release);
    reclaimer.join();

    CHECK_EQ(pool.used(), 0U);

    // full re-alloc of the whole capacity must succeed (no lost block)
    coact::Event* all[kCap];
    for (std::uint16_t i = 0U; i < kCap; ++i) {
        all[i] = pool.alloc(1U);
        REQUIRE(all[i] != nullptr);
    }
    CHECK_EQ(pool.used(), kCap);
    for (std::uint16_t i = 0U; i < kCap; ++i) {
        coact::event_gc(all[i]);
    }
    CHECK_EQ(pool.used(), 0U);
}

COACT_TEST_MAIN()
