// coact SoftIrqOps (software interrupt simulation) test.
// SPDX-License-Identifier: MIT
//
// Two PAL paths in one binary:
//   - pal::Posix: REAL signal mechanics. SIGRTMIN is queued via sigqueue() and
//     consumed through a signalfd; NO signal handler is ever registered (the
//     key design point of the SoftIrqOps contract). Covers:
//       1. producer-thread raise -> consumer take() carries the payload,
//       2. back-to-back raises queue FIFO with distinct payloads,
//       3. take() timeout returns -1 and deinit() restores the consumer's
//          signal mask.
//   - pal::RtThread (COACT_RTT_STUB): the shared-mailbox ring FIFO + busy
//     reject + timeout paths. Board-level verification of the real
//     rt_signal_install / rt_thread_kill wakeup is pending (see the
//     comment in test/rtthread_stub.h).
//
// Note on coalescing (test 2): SIGRTMIN is a real-time signal on Linux and
// is QUEUED per instance (multiple identical raises do NOT coalesce);
// standard-signal coalescing does NOT apply. We drain everything the producer
// raised before the consumer takes any, so the assertion is strict FIFO.
#include "test/test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <signal.h>

/* Pull in stub BEFORE pal_rtthread.hpp so RT-Thread types resolve. */
#ifndef COACT_RTT_STUB
#define COACT_RTT_STUB
#endif
#include "test/rtthread_stub.h"

#include "coact/pal.hpp"
#include "coact/pal_posix.hpp"
#include "coact/pal_rtthread.hpp"

namespace {

using coact::pal::Posix;
using coact::pal::RtThread;
using PosixSoftIrq = Posix::SoftIrqHandle;
using PosixThread  = Posix::ThreadHandle;
using RtSoftIrq    = RtThread::SoftIrqHandle;
using RtThreadHandle = RtThread::ThreadHandle;

/* ---- POSIX: signalfd path ------------------------------------------------- */

struct PosixRaiseCtx {
    Posix*         pal;
    PosixSoftIrq* h;
    int32_t       payload;
    std::atomic<bool>* ready;
};

COACT_TEST(posix_softirq_raise_take)
{
    Posix pal;
    PosixSoftIrq h{};
    REQUIRE(pal.softirq_init(h));

    std::atomic<bool> raised{false};
    PosixRaiseCtx ctx{&pal, &h, 4242, &raised};
    PosixThread t{};
    REQUIRE(pal.thread_create(t, [](void* a) {
        PosixRaiseCtx* c = static_cast<PosixRaiseCtx*>(a);
        c->h->pal->softirq_raise(*c->h, c->payload);
        c->ready->store(true, std::memory_order_release);
    }, &ctx));

    /* 1 s budget keeps the test stable under load without making a hang
       indistinguishable from a logic failure. */
    const int32_t got = pal.softirq_take(h, 1000U);
    pal.thread_join(t);

    CHECK_EQ(4242, got);
    CHECK(raised.load(std::memory_order_acquire));
    pal.softirq_deinit(h);
}

COACT_TEST(posix_softirq_queued_fifo)
{
    Posix pal;
    PosixSoftIrq h{};
    REQUIRE(pal.softirq_init(h));

    std::atomic<bool> raised{false};
    PosixRaiseCtx ctx{&pal, &h, 0, &raised};
    PosixThread t{};
    REQUIRE(pal.thread_create(t, [](void* a) {
        PosixRaiseCtx* c = static_cast<PosixRaiseCtx*>(a);
        for (int32_t i = 0; i < 8; ++i) {
            /* Sequential, back-to-back raises BEFORE the consumer takes any:
               SIGRTMIN is a real-time signal, queued per instance (distinct
               sival_int values), so all 8 must survive and arrive in FIFO
               order on the consumer's signalfd. Standard-signal coalescing
               does NOT apply to realtime signals. */
            c->h->pal->softirq_raise(*c->h, 100 + i);
        }
        c->ready->store(true, std::memory_order_release);
    }, &ctx));
    pal.thread_join(t);

    bool fifo = true;
    for (int32_t i = 0; i < 8; ++i) {
        const int32_t expected = 100 + i;
        const int32_t got = pal.softirq_take(h, 1000U);
        if (got != expected) { fifo = false; }
    }
    CHECK(fifo);
    pal.softirq_deinit(h);
}

COACT_TEST(posix_softirq_take_timeout_and_mask_restore)
{
    Posix pal;
    PosixSoftIrq h{};
    REQUIRE(pal.softirq_init(h));

    /* Nothing raised: take(60 ms) must time out and return -1, spending at
       least ~50 ms on the wall clock. The upper bound is not checked because
       poll() can legitimately overshoot. */
    const uint32_t t0 = static_cast<uint32_t>(pal.monotonic_ns() / 1000000ULL);
    const int32_t got = pal.softirq_take(h, 60U);
    const uint32_t t1 = static_cast<uint32_t>(pal.monotonic_ns() / 1000000ULL);

    CHECK_EQ(-1, got);
    CHECK((t1 - t0) >= 50U);

    pal.softirq_deinit(h);

    /* deinit() must restore the consumer's SIGRTMIN-equivalent mask: the
       fixed SoftIrqSignal number is no longer in the thread's blocked set
       after deinit. The test must use the same symbol the PAL uses (Posix's
       SIGRTMIN is glibc's runtime macro and is not necessarily equal). */
    sigset_t now;
    sigemptyset(&now);
    (void)pthread_sigmask(SIG_BLOCK, nullptr, &now);
    CHECK_EQ(0, sigismember(&now, Posix::SoftIrqSignal));
}

/* ---- RT-Thread stub: SPSC mailbox ring path ------------------------------ */

COACT_TEST(rtthread_softirq_ring_fifo_and_busy_reject)
{
    RtThread pal;
    RtSoftIrq h{};
    REQUIRE(pal.softirq_init(h));

    /* Fill the 8-slot ring. The slot count is a compile-time constant
       (kSoftIrqRingSlots = 8) — 9 raises must reject the last one. */
    bool published_ok = true;
    for (uint32_t i = 0U; i < 8U; ++i) {
        const int32_t payload = static_cast<int32_t>(i) * 11;
        if (!pal.softirq_raise(h, payload)) { published_ok = false; }
    }
    CHECK(published_ok);
    CHECK(!pal.softirq_raise(h, 999));   /* full: busy reject */

    /* Drain in FIFO order; matches the producer sequence. */
    bool fifo = true;
    for (uint32_t i = 0U; i < 8U; ++i) {
        const int32_t expected = static_cast<int32_t>(i) * 11;
        const int32_t got = pal.softirq_take(h, 200U);
        if (got != expected) { fifo = false; }
    }
    CHECK(fifo);

    /* Ring is now empty: the next take must time out (proving the tail index
       was advanced exactly 8 times — no off-by-one). */
    const uint32_t t0 = static_cast<uint32_t>(pal.monotonic_ns() / 1000000ULL);
    const int32_t drained = pal.softirq_take(h, 30U);
    const uint32_t t1 = static_cast<uint32_t>(pal.monotonic_ns() / 1000000ULL);
    CHECK_EQ(-1, drained);
    CHECK((t1 - t0) >= 20U);

    pal.softirq_deinit(h);
}

struct RtRaiseCtx {
    RtThread*   pal;
    RtSoftIrq*  h;
    int32_t     payload;
    std::atomic<bool>* ready;
};

COACT_TEST(rtthread_softirq_cross_thread)
{
    RtThread pal;
    RtSoftIrq h{};
    REQUIRE(pal.softirq_init(h));

    std::atomic<bool> raised{false};
    RtRaiseCtx ctx{&pal, &h, 77, &raised};
    RtThreadHandle t{};
    REQUIRE(pal.thread_create(t, [](void* a) {
        RtRaiseCtx* c = static_cast<RtRaiseCtx*>(a);
        c->h->pal->softirq_raise(*c->h, c->payload);
        c->ready->store(true, std::memory_order_release);
    }, &ctx));

    const int32_t got = pal.softirq_take(h, 1000U);
    pal.thread_join(t);

    CHECK_EQ(77, got);
    CHECK(raised.load(std::memory_order_acquire));
    pal.softirq_deinit(h);
}

/* timeout_ms 0 means "wait forever" (pal.hpp SoftIrqOps contract), NOT an
   immediate timeout. This is the one place the two PALs disagreed: poll()
   spells forever as -1 and returns at once for 0, so a POSIX implementation
   that forwarded 0 straight through would report "nothing raised" while the
   producer was about to raise. The consumer must still be waiting when the
   payload arrives. */
COACT_TEST(posix_softirq_take_zero_waits_forever)
{
    Posix pal;
    PosixSoftIrq h{};
    REQUIRE(pal.softirq_init(h));

    std::atomic<bool> raised{false};
    PosixRaiseCtx ctx{&pal, &h, 31337, &raised};
    PosixThread t{};
    REQUIRE(pal.thread_create(t, [](void* a) {
        PosixRaiseCtx* c = static_cast<PosixRaiseCtx*>(a);
        /* Give the consumer time to reach take(0) and actually block; if 0
           were still "return immediately" the consumer would already have
           given up by the time this lands. */
        c->h->pal->sleep_us(50000U);
        c->h->pal->softirq_raise(*c->h, c->payload);
        c->ready->store(true, std::memory_order_release);
    }, &ctx));

    const int32_t got = pal.softirq_take(h, 0U);
    pal.thread_join(t);

    CHECK_EQ(31337, got);
    CHECK(raised.load(std::memory_order_acquire));
    pal.softirq_deinit(h);
}

}  // namespace

COACT_TEST_MAIN()