// Host microbenchmark for the stage-1 SPSC hard checkpoint A: coact::SpscRing
// (lock-free acquire/release) vs the existing irq-mask SingleCoreCriticalRing.
//
// Numbers here are x86-64 host measurements (algorithm-level; the irq-mask ring
// runs with a no-op critical section). The authoritative target comparison is
// the ARM static assembly analysis (arm_ring_probe.cpp) which measures
// instruction counts, .text/.rodata and libatomic symbols for cortex-m4.
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "coact/queue.hpp"
#include "coact/spsc_ring.hpp"

namespace {

// No-op critical section: the host Posix PAL irq_save is itself a no-op, so
// this is the honest host measurement of the ring algorithm (the ARM cost of
// cpsid/cpsie is measured separately by arm_ring_probe.cpp).
uintptr_t noop_save(void*) { return 0U; }
void noop_restore(void*, uintptr_t) {}

coact::CriticalSection noop_cs()
{
    coact::CriticalSection cs;
    cs.ctx = nullptr;
    cs.save = &noop_save;
    cs.restore = &noop_restore;
    return cs;
}

inline uint64_t rdtsc()
{
    unsigned lo = 0U;
    unsigned hi = 0U;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32U) | static_cast<uint64_t>(lo);
}

void report(const char* name, const std::vector<uint64_t>& samples)
{
    std::vector<uint64_t> s = samples;
    std::sort(s.begin(), s.end());
    const size_t n = s.size();
    uint64_t sum = 0U;
    for (uint64_t v : s) {
        sum += v;
    }
    const uint64_t mean = sum / static_cast<uint64_t>(n);
    const uint64_t median = s[n / 2U];
    const uint64_t p99 = s[(n * 99U) / 100U];
    const uint64_t maxv = s[n - 1U];
    std::printf("%-32s n=%-7zu median=%-8llu p99=%-8llu max=%-8llu mean=%-8llu\n",
                name, n,
                static_cast<unsigned long long>(median),
                static_cast<unsigned long long>(p99),
                static_cast<unsigned long long>(maxv),
                static_cast<unsigned long long>(mean));
}

// Push path in isolation: ring kept half full; each iteration measures one
// push then pops to restore the level (the pop is not measured).
template <typename Q>
void bench_push(const char* name, Q& q, const uint16_t capacity, const int iters)
{
    const uint16_t half = static_cast<uint16_t>(capacity / 2U);
    for (uint16_t i = 0U; i < half; ++i) {
        (void)q.try_push(static_cast<uint16_t>(i));
    }
    uint16_t v = 0U;
    std::vector<uint64_t> samples;
    samples.reserve(static_cast<size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        const uint64_t t0 = rdtsc();
        (void)q.try_push(static_cast<uint16_t>(i));
        const uint64_t t1 = rdtsc();
        (void)q.try_pop(v);
        samples.push_back(t1 - t0);
    }
    report(name, samples);
}

// Pop path in isolation: ring kept half full; each iteration measures one pop
// then pushes to restore the level (the push is not measured).
template <typename Q>
void bench_pop(const char* name, Q& q, const uint16_t capacity, const int iters)
{
    const uint16_t half = static_cast<uint16_t>(capacity / 2U);
    for (uint16_t i = 0U; i < half; ++i) {
        (void)q.try_push(static_cast<uint16_t>(i));
    }
    uint16_t v = 0U;
    std::vector<uint64_t> samples;
    samples.reserve(static_cast<size_t>(iters));
    for (int i = 0; i < iters; ++i) {
        const uint64_t t0 = rdtsc();
        (void)q.try_pop(v);
        const uint64_t t1 = rdtsc();
        (void)q.try_push(static_cast<uint16_t>(i));
        samples.push_back(t1 - t0);
    }
    report(name, samples);
}

}  // namespace

int main()
{
    constexpr int kIters = 200000;
    constexpr uint16_t kCap = 16;

    std::printf("host: x86-64, rdtsc cycles/op, %d iterations, capacity %u\n",
                kIters, static_cast<unsigned>(kCap));

    coact::SpscRing<uint16_t, kCap> spsc;
    bench_push("spsc_push", spsc, kCap, kIters);
    bench_pop("spsc_pop", spsc, kCap, kIters);

    coact::SingleCoreCriticalRing<uint16_t, kCap> ring(noop_cs());
    bench_push("irqmask_ring_push (noop cs)", ring, kCap, kIters);
    bench_pop("irqmask_ring_pop (noop cs)", ring, kCap, kIters);

    // Two-thread SPSC throughput: strict FIFO with a contended cache line.
    {
        constexpr uint32_t kN = 4000000U;
        coact::SpscRing<uint32_t, 256> q;
        std::atomic<uint32_t> consumed{0U};
        auto t0 = std::chrono::steady_clock::now();
        std::thread consumer([&q, &consumed]() {
            uint32_t v = 0U;
            uint32_t count = 0U;
            while (count < kN) {
                if (q.try_pop(v)) {
                    ++count;
                }
            }
            consumed.store(count, std::memory_order_relaxed);
        });
        for (uint32_t i = 0U; i < kN; ++i) {
            while (!q.try_push(std::move(i))) {
                std::this_thread::yield();
            }
        }
        consumer.join();
        auto t1 = std::chrono::steady_clock::now();
        const double ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        std::printf("%-32s items=%u  %.1f ns/op  (push+pop pair)\n",
                    "spsc_2thread_throughput",
                    consumed.load(), ns / static_cast<double>(kN));
    }

    return 0;
}
