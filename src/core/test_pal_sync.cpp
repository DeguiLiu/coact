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
    int32_t job{0};
    std::atomic<int32_t> executed{0};
    bool running{false};

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

}  // namespace

COACT_TEST_MAIN()
