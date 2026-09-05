// coact PAL sync-primitives test (SemOps / MutexOps / CondOps / ThreadOps).
// SPDX-License-Identifier: MIT
//
// Host tests cover BOTH concrete paths:
//   - pal::Posix (native pthreads), and
//   - pal::RtThread under COACT_RTT_STUB (rt_sem/rt_mutex/rt_thread backed
//     by pthreads through test/rtthread_stub.h).
// Assertions exercise: init/take/release, counting semantics, timeouts,
// ISR-safe release (stub ISR-nest simulation), mutex ownership, condvar
// hand-off, and static-thread-table create/join (the WorkerBase contract).
#include "test/test_harness.hpp"

#include <atomic>
#include <cstdint>

/* Pull in stub BEFORE pal_rtthread.hpp so RT-Thread types resolve. */
#ifndef COACT_RTT_STUB
#define COACT_RTT_STUB
#endif
#include "test/rtthread_stub.h"

#include "coact/pal.hpp"
#include "coact/pal_posix.hpp"
#include "coact/pal_rtthread.hpp"
#include "coact/spsc_ring.hpp"

namespace {

using coact::pal::Posix;
using coact::pal::RtThread;
using PosixCondHandle = Posix::CondHandle;
using PosixMutexHandle = Posix::MutexHandle;
using PosixSemHandle = Posix::SemHandle;
using PosixThreadHandle = Posix::ThreadHandle;
using RtThreadCondHandle = RtThread::CondHandle;
using RtThreadMutexHandle = RtThread::MutexHandle;
using RtThreadSemHandle = RtThread::SemHandle;
using RtThreadThreadHandle = RtThread::ThreadHandle;

/* ---- SemOps / MutexOps / CondOps / ThreadOps on the POSIX PAL ------------ */

COACT_TEST(posix_sem_init_take_release)
{
    coact::pal::Posix pal;
    PosixSemHandle sem;
    CHECK(pal.sem_init(sem, 0U));
    CHECK(!pal.sem_take(sem, 0U));            /* 0 tokens: non-block take fails */
    pal.sem_release(sem);
    CHECK(pal.sem_take(sem, 0U));             /* the released token is consumed */
    CHECK(!pal.sem_take(sem, 0U));
    pal.sem_release(sem);
    pal.sem_release(sem);
    CHECK(pal.sem_take(sem, 0U));
    CHECK(pal.sem_take(sem, 0U));             /* counting: 2 releases -> 2 takes */
    pal.sem_deinit(sem);
}

COACT_TEST(posix_sem_timeout_ms)
{
    coact::pal::Posix pal;
    PosixSemHandle sem;
    CHECK(pal.sem_init(sem, 0U));
    const uint32_t t0 = static_cast<uint32_t>(pal.monotonic_ns() / 1000000ULL);
    CHECK(!pal.sem_take(sem, 50U));           /* no token: must time out */
    const uint32_t t1 = static_cast<uint32_t>(pal.monotonic_ns() / 1000000ULL);
    CHECK((t1 - t0) >= 40U);                  /* ~50 ms elapsed, coarse guard */
    pal.sem_release(sem);
    CHECK(pal.sem_take(sem, 50U));            /* ready token: returns immediately */
    pal.sem_deinit(sem);
}

COACT_TEST(posix_sem_isr_release)
{
    coact::pal::Posix pal;
    PosixSemHandle sem;
    CHECK(pal.sem_init(sem, 0U));
    /* Host ISR simulation (the documented contract: a normal thread standing
       in for the ISR; POSIX has no async-signal-safe release). */
    pal.sem_release_from_isr(sem);
    CHECK(pal.sem_take(sem, 0U));
    pal.sem_deinit(sem);
}

COACT_TEST(posix_mutex_lock_unlock)
{
    coact::pal::Posix pal;
    PosixMutexHandle m;
    CHECK(pal.mutex_init(m));
    pal.mutex_lock(m);
    pal.mutex_unlock(m);
    pal.mutex_lock(m);
    pal.mutex_unlock(m);
    pal.mutex_deinit(m);
    CHECK(true);                              /* no deadlock = pass */
}

COACT_TEST(posix_mutex_serializes_two_threads)
{
    coact::pal::Posix pal;
    PosixMutexHandle m;
    CHECK(pal.mutex_init(m));
    std::atomic<int32_t> counter{0};
    std::atomic<int32_t> max_overlap{0};
    std::atomic<bool> go{false};

    struct Ctx {
        std::atomic<int32_t>* counter;
        std::atomic<int32_t>* max_overlap;
        std::atomic<bool>* go;
        PosixMutexHandle* m;
    } ctx{&counter, &max_overlap, &go, &m};

    /* Thread entry goes through the PAL ThreadOps (same trampoline shape the
       demo workers use). */
    PosixThreadHandle t1{}, t2{};
    CHECK(pal.thread_create(t1, [](void* a) {
        Ctx* c = static_cast<Ctx*>(a);
        while (!c->go->load(std::memory_order_relaxed)) { }
        c->m->pal->mutex_lock(*c->m);
        const int32_t inside = ++*c->counter;
        /* If another thread ever overlapped, counter would exceed the max
           single-section overlap we allow (2). */
        if (inside > *c->max_overlap) { *c->max_overlap = inside; }
        --*c->counter;
        c->m->pal->mutex_unlock(*c->m);
    }, &ctx));
    CHECK(pal.thread_create(t2, [](void* a) {
        Ctx* c = static_cast<Ctx*>(a);
        while (!c->go->load(std::memory_order_relaxed)) { }
        c->m->pal->mutex_lock(*c->m);
        const int32_t inside = ++*c->counter;
        if (inside > *c->max_overlap) { *c->max_overlap = inside; }
        --*c->counter;
        c->m->pal->mutex_unlock(*c->m);
    }, &ctx));
    go.store(true, std::memory_order_relaxed);
    pal.thread_join(t1);
    pal.thread_join(t2);
    pal.mutex_deinit(m);
    CHECK(max_overlap.load() <= 1);           /* mutex held by at most one */
}

COACT_TEST(posix_cond_handoff)
{
    coact::pal::Posix pal;
    PosixMutexHandle m;
    PosixCondHandle cond;
    CHECK(pal.mutex_init(m));
    CHECK(pal.cond_init(cond));
    m.pal = &pal;
    cond.pal = &pal;

    std::atomic<bool> ready{false};
    std::atomic<bool> done{false};
    struct Ctx { PosixMutexHandle* m; PosixCondHandle* c; std::atomic<bool>* ready;
                 std::atomic<bool>* done; } ctx{&m, &cond, &ready, &done};

    PosixThreadHandle t{};
    CHECK(pal.thread_create(t, [](void* a) {
        Ctx* c = static_cast<Ctx*>(a);
        c->m->pal->mutex_lock(*c->m);
        c->ready->store(true, std::memory_order_release);
        while (!c->done->load(std::memory_order_relaxed)) {
            c->c->pal->cond_wait(*c->c, *c->m, 0U);   /* 0 = wait forever */
        }
        c->m->pal->mutex_unlock(*c->m);
    }, &ctx));

    while (!ready.load(std::memory_order_acquire)) { }
    pal.mutex_lock(m);
    done.store(true, std::memory_order_relaxed);
    pal.cond_signal(cond);
    pal.mutex_unlock(m);
    pal.thread_join(t);

    pal.cond_deinit(cond);
    pal.mutex_deinit(m);
    CHECK(ready.load() && done.load());
}

COACT_TEST(posix_thread_create_join)
{
    coact::pal::Posix pal;
    std::atomic<int32_t> ran{0};
    PosixThreadHandle t{};
    CHECK(pal.thread_create(t, [](void* a) {
        static_cast<std::atomic<int32_t>*>(a)->store(1, std::memory_order_release);
    }, &ran));
    pal.thread_join(t);
    CHECK_EQ(1, ran.load(std::memory_order_acquire));
}

/* ---- SemOps / MutexOps / CondOps / ThreadOps on the RT-Thread PAL (stub) - */

COACT_TEST(rtthread_sem_init_take_release)
{
    coact::pal::RtThread pal;
    RtThreadSemHandle sem;
    CHECK(pal.sem_init(sem, 0U));
    CHECK(!pal.sem_take(sem, 0U));
    pal.sem_release(sem);
    CHECK(pal.sem_take(sem, 0U));
    pal.sem_release(sem);
    pal.sem_release(sem);
    CHECK(pal.sem_take(sem, 0U));
    CHECK(pal.sem_take(sem, 0U));
    pal.sem_deinit(sem);
}

COACT_TEST(rtthread_sem_timeout_ms)
{
    coact::pal::RtThread pal;
    RtThreadSemHandle sem;
    CHECK(pal.sem_init(sem, 0U));
    const uint32_t t0 = static_cast<uint32_t>(pal.monotonic_ns() / 1000000ULL);
    CHECK(!pal.sem_take(sem, 50U));
    const uint32_t t1 = static_cast<uint32_t>(pal.monotonic_ns() / 1000000ULL);
    CHECK((t1 - t0) >= 40U);
    pal.sem_release(sem);
    CHECK(pal.sem_take(sem, 50U));
    pal.sem_deinit(sem);
}

COACT_TEST(rtthread_sem_isr_release)
{
    coact::pal::RtThread pal;
    RtThreadSemHandle sem;
    CHECK(pal.sem_init(sem, 0U));
    /* rt_sem_release is ISR-safe (real target and stub both). Nest the ISR
       flag to prove the release path does not depend on task context. */
    stub_set_isr_nest(1U);
    pal.sem_release_from_isr(sem);
    stub_set_isr_nest(0U);
    CHECK(pal.sem_take(sem, 0U));
    pal.sem_deinit(sem);
}

COACT_TEST(rtthread_mutex_lock_unlock)
{
    coact::pal::RtThread pal;
    RtThreadMutexHandle m;
    CHECK(pal.mutex_init(m));
    pal.mutex_lock(m);
    pal.mutex_unlock(m);
    pal.mutex_deinit(m);
    CHECK(true);
}

COACT_TEST(rtthread_cond_handoff)
{
    coact::pal::RtThread pal;
    RtThreadMutexHandle m;
    RtThreadCondHandle cond;
    CHECK(pal.mutex_init(m));
    CHECK(pal.cond_init(cond));
    m.pal = &pal;
    cond.pal = &pal;

    std::atomic<bool> ready{false};
    std::atomic<bool> done{false};
    struct Ctx { RtThreadMutexHandle* m; RtThreadCondHandle* c; std::atomic<bool>* ready;
                 std::atomic<bool>* done; } ctx{&m, &cond, &ready, &done};

    RtThreadThreadHandle t{};
    CHECK(pal.thread_create(t, [](void* a) {
        Ctx* c = static_cast<Ctx*>(a);
        c->m->pal->mutex_lock(*c->m);
        c->ready->store(true, std::memory_order_release);
        while (!c->done->load(std::memory_order_relaxed)) {
            c->c->pal->cond_wait(*c->c, *c->m, 0U);
        }
        c->m->pal->mutex_unlock(*c->m);
    }, &ctx));

    while (!ready.load(std::memory_order_acquire)) { }
    pal.mutex_lock(m);
    done.store(true, std::memory_order_relaxed);
    pal.cond_signal(cond);               /* pthread pattern: signal under lock
                                            (cond_wait releases/re-acquires m) */
    pal.mutex_unlock(m);
    pal.thread_join(t);

    pal.cond_deinit(cond);
    pal.mutex_deinit(m);
    CHECK(ready.load() && done.load());
}

COACT_TEST(rtthread_thread_create_join)
{
    coact::pal::RtThread pal;
    std::atomic<int32_t> ran{0};
    RtThreadThreadHandle t{};
    CHECK(pal.thread_create(t, [](void* a) {
        static_cast<std::atomic<int32_t>*>(a)->store(1, std::memory_order_release);
    }, &ran));
    pal.thread_join(t);
    CHECK_EQ(1, ran.load(std::memory_order_acquire));
}

/* ---- Worker-shape contract: cross-thread hand-off via PAL primitives ------ */

/* A minimal WorkerBase stand-in: single-slot ring, submit-from-producer,
   drain-on-stop — the shape the demo's WorkerBase will be rebuilt on (Phase
   2). Exercises the exact primitive mix the demo uses. */
template <typename PalT>
struct PalWorkerProbe {
    PalT* pal;
    typename PalT::MutexHandle mtx;
    typename PalT::CondHandle cond;
    typename PalT::ThreadHandle thread;
    int32_t job{0};                        /* guarded by mtx */
    std::atomic<int32_t> executed{0};      /* cross-thread read by the test loop */
    bool running{false};               /* guarded by mtx */

    bool start(PalT& p)
    {
        pal = &p;
        if (!pal->mutex_init(mtx)) { return false; }
        if (!pal->cond_init(cond)) { return false; }
        mtx.pal = pal;
        cond.pal = pal;
        running = true;
        return pal->thread_create(thread, [](void* a) {
            static_cast<PalWorkerProbe*>(a)->run();
        }, this);
    }

    /* Producer side (Dispatcher thread): reject when busy. */
    bool submit(int32_t j)
    {
        pal->mutex_lock(mtx);
        bool ok = false;
        if (0 == job) {
            job = j;
            ok = true;
        }
        pal->cond_signal(cond);
        pal->mutex_unlock(mtx);
        return ok;
    }

    void stop()
    {
        pal->mutex_lock(mtx);
        running = false;
        pal->cond_signal(cond);
        pal->mutex_unlock(mtx);
        pal->thread_join(thread);
        pal->cond_deinit(cond);
        pal->mutex_deinit(mtx);
    }

private:
    void run()
    {
        for (;;) {
            pal->mutex_lock(mtx);
            while (running && 0 == job) {
                cond.pal->cond_wait(cond, mtx, 0U);
            }
            const int32_t j = job;
            job = 0;
            const bool alive = running;
            pal->mutex_unlock(mtx);
            if (!alive && 0 == j) { return; }
            ++executed;                 /* the "hardware latency" stand-in */
        }
    }
};

COACT_TEST(posix_worker_handoff_probe)
{
    coact::pal::Posix pal;
    PalWorkerProbe<coact::pal::Posix> w;
    CHECK(w.start(pal));
    CHECK(w.submit(1));
    CHECK(!w.submit(2));                /* single slot: busy reject */
    while (w.executed.load() < 1) { }
    CHECK(w.submit(3));
    w.stop();
    CHECK_EQ(2, w.executed.load());
}


COACT_TEST(rtthread_signal_before_wait_no_lost_wake)
{
    /* Regression (M-1 follow-up): a signal that fires AFTER the waiter
       increments the counter but BEFORE it blocks must not be lost, and a
       token released when the waiter is between loops must not break the
       next blocking wait. Loops fast enough to hit the window under -O2. */
    coact::pal::RtThread pal;
    PalWorkerProbe<coact::pal::RtThread> w;
    CHECK(w.start(pal));
    /* Start at 1: job uses 0 as the empty-slot sentinel, so a submitted
       value of 0 would look like an empty slot and the worker predicate
       would never become true (a real deadlock, but of the test design,
       not the PAL). */
    for (int32_t i = 1; i <= 200; ++i) {
        CHECK(w.submit(i));
        while (w.executed.load() < i) { }
    }
    w.stop();
    CHECK_EQ(200, w.executed.load());
}

COACT_TEST(rtthread_worker_handoff_probe)
{
    coact::pal::RtThread pal;
    PalWorkerProbe<coact::pal::RtThread> w;
    CHECK(w.start(pal));
    CHECK(w.submit(1));
    CHECK(!w.submit(2));
    while (w.executed.load() < 1) { }
    CHECK(w.submit(3));
    w.stop();
    CHECK_EQ(2, w.executed.load());
}

COACT_TEST(posix_watchdog_progress_records_timestamp)
{
    coact::pal::Posix pal;
    /* Fresh PAL: no progress yet, so it is not "proven alive". */
    CHECK(!pal.dispatcher_alive_within(1000U));
    const uint64_t before = pal.monotonic_ns();
    pal.watchdog_progress(1U);
    const uint64_t recorded = pal.dispatcher_progress_ns();
    CHECK(recorded >= before);
    /* A tiny window from the last progress must now report alive. */
    CHECK(pal.dispatcher_alive_within(1000U));
}

COACT_TEST(posix_alive_without_progress_is_false)
{
    coact::pal::Posix pal;
    /* Never called watchdog_progress: dispatcher_progress_ns() == 0, so
       alive_within() must be false regardless of window. */
    CHECK_EQ(0ULL, pal.dispatcher_progress_ns());
    CHECK(!pal.dispatcher_alive_within(1000U));
    CHECK(!pal.dispatcher_alive_within(1U));
}

COACT_TEST(posix_alive_windows_expire)
{
    coact::pal::Posix pal;
    pal.set_tick_hz(0U);  /* native ns resolution for the timing assertion */
    pal.watchdog_progress(1U);
    CHECK(pal.dispatcher_alive_within(1000U));
    /* Sleep past a 1 ms window: the same progress timestamp is now stale. */
    pal.sleep_us(3000U);
    CHECK(!pal.dispatcher_alive_within(1U));
}

/* ---- SPSC worker hand-off: SpscRing + PAL semaphore primitive mix ------- */

/* The lock-free WorkerBase shape (design_isp_pipeline_optimization P2):
   SpscRing<Job, Pow2> storage + one PAL semaphore wake. The producer is a
   single thread (the Dispatcher analog); the consumer is the worker thread.
   Exercises the exact primitive mix the rebuilt demo WorkerBase uses. */
template <typename PalT>
struct SpscWorkerProbe {
    PalT* pal;
    typename PalT::SemHandle wake;
    typename PalT::ThreadHandle thread;
    coact::SpscRing<int32_t, 4U> ring;
    std::atomic<uint32_t> executed{0U};
    std::atomic<bool> running{false};
    int32_t last_job{-1};

    bool start(PalT& p)
    {
        pal = &p;
        if (!pal->sem_init(wake, 0U)) { return false; }
        running.store(true, std::memory_order_release);
        return pal->thread_create(thread, [](void* a) {
            static_cast<SpscWorkerProbe*>(a)->run();
        }, this);
    }

    /* Producer side: try_push rejects on a full ring, no lock anywhere. */
    bool submit(int32_t j)
    {
        return ring.try_push(std::move(j));
    }

    void stop()
    {
        running.store(false, std::memory_order_release);
        pal->sem_release(wake);
        pal->thread_join(thread);
    }

private:
    void run()
    {
        for (;;) {
            int32_t j = 0;
            while (ring.try_pop(j)) {
                last_job = j;
                executed.fetch_add(1U, std::memory_order_relaxed);
            }
            if (!running.load(std::memory_order_acquire)) { return; }
            (void)pal->sem_take(wake, 100U);
        }
    }
};

COACT_TEST(posix_spsc_worker_handoff_probe)
{
    coact::pal::Posix pal;
    SpscWorkerProbe<coact::pal::Posix> w;
    CHECK(w.start(pal));

    /* Depth-4 ring: the 5th submit rejects (full), no lock involved. */
    CHECK(w.submit(1));
    CHECK(w.submit(2));
    CHECK(w.submit(3));
    CHECK(w.submit(4));
    CHECK(!w.submit(5));               /* full: honest busy reject */
    w.pal->sem_release(w.wake);

    /* Wait for the batch to drain, then one more round-trip. */
    while (w.executed.load(std::memory_order_acquire) < 4U) { }
    CHECK(w.submit(6));
    w.pal->sem_release(w.wake);
    while (w.executed.load(std::memory_order_acquire) < 5U) { }
    CHECK_EQ(6, w.last_job);           /* FIFO order preserved */

    w.stop();
    CHECK_EQ(5U, w.executed.load());
}

}  // namespace

COACT_TEST_MAIN()
