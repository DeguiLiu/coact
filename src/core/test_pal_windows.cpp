// coact Windows PAL sync-primitive test (SemOps / MutexOps / CondOps /
// ThreadOps / SoftIrqOps). SPDX-License-Identifier: MIT
//
// Windows-only: built by src/core/CMakeLists.txt under if(WIN32). The
// assertions mirror src/core/test_pal_sync.cpp and test_softirq.cpp so the
// Windows PAL honours the exact same contract the POSIX and RT-Thread PALs do:
//   - SemOps: 0 = non-blocking try, kWaitForever = block, counting semantics,
//     bounded timeout.
//   - CondOps: timeout_ms 0 = wait FOREVER (the opposite of Win32's raw
//     SleepConditionVariableCS 0 meaning) — the hand-off test exercises this.
//   - SoftIrqOps: 8-slot SPSC FIFO, 9th raise rejected, N distinct raises ->
//     N distinct payloads (no coalescing), timeout returns -1.
//
// COACT_ENABLE_WINDOWS_TEST_HOOKS must be defined before pal_windows.hpp to
// expose the test-only start-handshake timeout accessor; production TUs do not
// define it and therefore cannot zero the production bound.
#define COACT_ENABLE_WINDOWS_TEST_HOOKS
#include "test/test_harness.hpp"

#include <atomic>
#include <cstdint>

#include "coact/pal.hpp"
#include "coact/pal_windows.hpp"

namespace {

using coact::pal::Windows;

/* ---- SemOps --------------------------------------------------------------- */

COACT_TEST(windows_sem_init_take_release)
{
    Windows pal;
    Windows::SemHandle sem;
    CHECK(pal.sem_init(sem, 0U));
    CHECK(!pal.sem_take(sem, 0U));            // 0 tokens: non-blocking try fails
    pal.sem_release(sem);
    CHECK(pal.sem_take(sem, 0U));             // the released token is consumed
    CHECK(!pal.sem_take(sem, 0U));
    pal.sem_release(sem);
    pal.sem_release(sem);
    CHECK(pal.sem_take(sem, 0U));
    CHECK(pal.sem_take(sem, 0U));             // counting: 2 releases -> 2 takes
    pal.sem_deinit(sem);
}

COACT_TEST(windows_sem_timeout_ms)
{
    Windows pal;
    Windows::SemHandle sem;
    CHECK(pal.sem_init(sem, 0U));
    const uint32_t t0 = static_cast<uint32_t>(pal.monotonic_ns() / 1000000ULL);
    CHECK(!pal.sem_take(sem, 50U));           // no token: must time out
    const uint32_t t1 = static_cast<uint32_t>(pal.monotonic_ns() / 1000000ULL);
    CHECK((t1 - t0) >= 40U);                  // ~50 ms elapsed, coarse guard
    pal.sem_release(sem);
    CHECK(pal.sem_take(sem, 50U));            // ready token: returns immediately
    pal.sem_deinit(sem);
}

COACT_TEST(windows_sem_isr_release)
{
    Windows pal;
    Windows::SemHandle sem;
    CHECK(pal.sem_init(sem, 0U));
    // Host ISR simulation: a normal thread stands in for the ISR.
    pal.sem_release_from_isr(sem);
    CHECK(pal.sem_take(sem, 0U));
    pal.sem_deinit(sem);
}

COACT_TEST(windows_sem_wait_forever_wakes)
{
    Windows pal;
    Windows::SemHandle sem;
    CHECK(pal.sem_init(sem, 0U));
    std::atomic<bool> got{false};
    Windows::ThreadHandle t{};
    struct Ctx { Windows* pal; Windows::SemHandle* sem; std::atomic<bool>* got; }
        ctx{&pal, &sem, &got};
    CHECK(pal.thread_create(t, [](void* a) {
        Ctx* c = static_cast<Ctx*>(a);
        c->got->store(c->pal->sem_take(*c->sem, coact::pal::kWaitForever),
                      std::memory_order_release);
    }, &ctx));
    pal.sem_release(sem);                     // wakes the kWaitForever waiter
    pal.thread_join(t);
    CHECK(got.load(std::memory_order_acquire));
    pal.sem_deinit(sem);
}

/* ---- MutexOps / ThreadOps ------------------------------------------------- */

COACT_TEST(windows_mutex_lock_unlock)
{
    Windows pal;
    Windows::MutexHandle m;
    CHECK(pal.mutex_init(m));
    pal.mutex_lock(m);
    pal.mutex_unlock(m);
    pal.mutex_lock(m);
    pal.mutex_unlock(m);
    pal.mutex_deinit(m);
    CHECK(true);                              // no deadlock = pass
}

COACT_TEST(windows_mutex_serializes_two_threads)
{
    Windows pal;
    Windows::MutexHandle m;
    CHECK(pal.mutex_init(m));
    std::atomic<int32_t> counter{0};
    std::atomic<int32_t> max_overlap{0};
    std::atomic<bool> go{false};

    struct Ctx {
        std::atomic<int32_t>* counter;
        std::atomic<int32_t>* max_overlap;
        std::atomic<bool>* go;
        Windows::MutexHandle* m;
    } ctx{&counter, &max_overlap, &go, &m};

    Windows::ThreadHandle t1{}, t2{};
    CHECK(pal.thread_create(t1, [](void* a) {
        Ctx* c = static_cast<Ctx*>(a);
        while (!c->go->load(std::memory_order_relaxed)) { }
        c->m->pal->mutex_lock(*c->m);
        const int32_t inside = ++*c->counter;
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
    CHECK(max_overlap.load() <= 1);           // mutex held by at most one
}

COACT_TEST(windows_thread_create_join)
{
    Windows pal;
    std::atomic<int32_t> ran{0};
    Windows::ThreadHandle t{};
    CHECK(pal.thread_create(t, [](void* a) {
        static_cast<std::atomic<int32_t>*>(a)->store(1, std::memory_order_release);
    }, &ran));
    pal.thread_join(t);
    CHECK_EQ(1, ran.load(std::memory_order_acquire));
}

/* ---- Dispatcher start handshake lifetime --------------------------------- */
//
// Defect pinned here: on a handshake timeout the PAL used to CloseHandle-and-
// detach the entry thread, which then ran the Dispatcher loop against a
// destroyed PAL/Dispatcher/Runtime (use-after-free). The gate protocol makes
// the entry return without touching user_entry_ on any failed start, and
// start_dispatcher joins it before returning false.

// Positive contract: a successful start releases the entry into user_entry_,
// and join_dispatcher() reclaims it. The real entry is the Dispatcher loop; an
// entry that returns on its own is a legal ThreadEntry and keeps this joinable.
COACT_TEST(windows_start_dispatcher_success_runs_entry)
{
    Windows pal;
    std::atomic<int32_t> ran{0};
    struct Ctx { std::atomic<int32_t>* ran; } ctx{&ran};
    const bool ok = pal.start_dispatcher([](void* a) {
        static_cast<Ctx*>(a)->ran->fetch_add(1, std::memory_order_release);
    }, &ctx);
    CHECK(ok);
    // start_dispatcher returns only after it opened the gate, so the entry is
    // on its way in; give it a bounded moment to report.
    for (int i = 0; i < 2000 && ran.load(std::memory_order_acquire) == 0; ++i) {
        pal.sleep_us(500U);
    }
    CHECK_EQ(1, ran.load(std::memory_order_acquire));
    pal.join_dispatcher();                 // entry already returned
    CHECK(true);                           // no hang = pass
}

// The invariant: a handshake timeout must never let the entry thread run
// user_entry_, and must never leave an unjoinable (detached) thread. The
// host-test hook forces the timeout deterministically (0 = fail immediately).
COACT_TEST(windows_start_dispatcher_timeout_never_runs_entry)
{
    Windows pal;
    pal.set_start_timeout_for_test(0U);        // force the handshake timeout
    std::atomic<int32_t> ran{0};
    struct Ctx { std::atomic<int32_t>* ran; } ctx{&ran};
    const bool ok = pal.start_dispatcher([](void* a) {
        static_cast<Ctx*>(a)->ran->fetch_add(1, std::memory_order_release);
    }, &ctx);
    CHECK(!ok);                            // failure return unchanged
    // Under the old detach path the starved thread would signal started_event_
    // and call user_entry_ here, flipping `ran` to 1.
    pal.sleep_us(200000U);                 // give a detached thread time to run
    CHECK_EQ(0, ran.load(std::memory_order_acquire));
    pal.join_dispatcher();                 // must be a no-op, not a hang
    CHECK(true);
}

// A failed start must be restartable: start_abandoned_ is reset per attempt and
// a later start still completes the gate handshake.
COACT_TEST(windows_start_dispatcher_abort_then_restart)
{
    Windows pal;
    std::atomic<int32_t> ran{0};
    struct Ctx { std::atomic<int32_t>* ran; } ctx{&ran};
    pal.set_start_timeout_for_test(0U);
    CHECK(!pal.start_dispatcher([](void* a) {
        static_cast<Ctx*>(a)->ran->fetch_add(1, std::memory_order_release);
    }, &ctx));
    pal.set_start_timeout_for_test(1000U);
    const bool ok = pal.start_dispatcher([](void* a) {
        static_cast<Ctx*>(a)->ran->fetch_add(1, std::memory_order_release);
    }, &ctx);
    CHECK(ok);
    for (int i = 0; i < 2000 && ran.load(std::memory_order_acquire) == 0; ++i) {
        pal.sleep_us(500U);
    }
    CHECK_EQ(1, ran.load(std::memory_order_acquire));
    pal.join_dispatcher();
}

// Stale-signal regression. A timed-out attempt abandons its entry, but that
// entry still executes SetEvent(started_event_) before it observes
// start_abandoned_ and returns, so the auto-reset started_event_ is left
// signaled. A later attempt must NOT consume that stale signal and report
// success before its own entry has reached the gate.
//
// Reproduces without the per-attempt ResetEvent: attempt #2 with a zero-length
// bound returns true in ~0 ms while ran is still 0, i.e. success reported
// before the dispatcher ran at all.
COACT_TEST(windows_start_dispatcher_stale_signal_not_reused)
{
    Windows pal;
    std::atomic<int32_t> ran{0};
    struct Ctx { std::atomic<int32_t>* ran; } ctx{&ran};
    const auto entry = [](void* a) {
        static_cast<Ctx*>(a)->ran->fetch_add(1, std::memory_order_release);
    };

    // Attempt #1: a zero-length handshake bound forces the timeout path and
    // guarantees a stale start signal is left pending (the entry signals before
    // it can be joined).
    pal.set_start_timeout_for_test(0U);
    CHECK(!pal.start_dispatcher(entry, &ctx));
    pal.join_dispatcher();

    // Attempt #2 with the same zero-length bound. A stale signal would make
    // this return true instantly; with the fix the event was reset at the top
    // of this attempt, so its own not-yet-scheduled entry cannot signal in time
    // and the attempt must fail exactly like attempt #1.
    const bool ok = pal.start_dispatcher(entry, &ctx);
    CHECK(!ok);
    pal.join_dispatcher();

    // Positive control: a real attempt still succeeds and returns promptly
    // (well inside its own 1000 ms bound), and the entry it started runs.
    pal.set_start_timeout_for_test(1000U);
    const uint64_t t0 = pal.monotonic_ns();
    const bool ok2 = pal.start_dispatcher(entry, &ctx);
    const uint64_t elapsed_ms = (pal.monotonic_ns() - t0) / 1000000ULL;
    CHECK(ok2);
    CHECK(elapsed_ms < 500ULL);            // not a timeout-returned-as-success
    for (int i = 0; i < 2000 && ran.load(std::memory_order_acquire) == 0; ++i) {
        pal.sleep_us(500U);
    }
    CHECK_EQ(1, ran.load(std::memory_order_acquire));  // only attempt #3 ran
    pal.join_dispatcher();
}

/* ---- CondOps: timeout 0 == wait forever ----------------------------------- */

COACT_TEST(windows_cond_handoff_zero_is_forever)
{
    Windows pal;
    Windows::MutexHandle m;
    Windows::CondHandle cond;
    CHECK(pal.mutex_init(m));
    CHECK(pal.cond_init(cond));
    m.pal = &pal;
    cond.pal = &pal;

    // Discriminating shape: the waiter signals that it has entered
    // cond_wait(0) and committed to blocking, and only THEN does the main
    // thread signal. A cond_wait that treated 0 as "return immediately" would
    // make the waiter loop back, re-check `done` (still false) and re-enter —
    // so `waited` would be observed as >1 spin. Waiting for real keeps the
    // spin count at 1 because the waiter is genuinely parked in the signal.
    //
    // The loop deliberately re-checks with no sleep: correctness here must come
    // from cond_wait blocking, not from the test being slow. (Note the harness
    // has no watchdog, so a test that hangs hangs the suite — which is exactly
    // why the loop is bounded by `done` and the main thread always sets it.)
    std::atomic<bool> entered{false};
    std::atomic<bool> done{false};
    std::atomic<int>  reentries{0};
    struct Ctx { Windows::MutexHandle* m; Windows::CondHandle* c;
                 std::atomic<bool>* entered; std::atomic<bool>* done;
                 std::atomic<int>* reentries; }
        ctx{&m, &cond, &entered, &done, &reentries};

    Windows::ThreadHandle t{};
    CHECK(pal.thread_create(t, [](void* a) {
        Ctx* c = static_cast<Ctx*>(a);
        c->m->pal->mutex_lock(*c->m);
        while (!c->done->load(std::memory_order_relaxed)) {
            c->reentries->fetch_add(1, std::memory_order_relaxed);
            c->entered->store(true, std::memory_order_release);
            // timeout_ms 0 means WAIT FOREVER. A PAL that forwarded 0 to
            // SleepConditionVariableCS would return at once and spin here.
            c->c->pal->cond_wait(*c->c, *c->m, 0U);
        }
        c->m->pal->mutex_unlock(*c->m);
    }, &ctx));

    // Wait until the worker has committed to cond_wait(0). It holds the mutex
    // up to that point, so the acquire below also proves the wait released it.
    while (!entered.load(std::memory_order_acquire)) { }
    pal.mutex_lock(m);                       // blocks until cond_wait releases
    CHECK_EQ(1, reentries.load(std::memory_order_relaxed));
    done.store(true, std::memory_order_relaxed);
    pal.cond_signal(cond);
    pal.mutex_unlock(m);
    pal.thread_join(t);

    // The worker saw `done` on the pass it resumed into, so it never looped.
    CHECK_EQ(1, reentries.load(std::memory_order_relaxed));

    pal.cond_deinit(cond);
    pal.mutex_deinit(m);
}

/* ---- Worker-shape hand-off (mirrors test_pal_sync PalWorkerProbe) --------- */

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
            ++executed;
        }
    }
};

COACT_TEST(windows_worker_handoff_probe)
{
    Windows pal;
    PalWorkerProbe<Windows> w;
    CHECK(w.start(pal));
    CHECK(w.submit(1));
    CHECK(!w.submit(2));                // single slot: busy reject
    while (w.executed.load() < 1) { }
    CHECK(w.submit(3));
    w.stop();
    CHECK_EQ(2, w.executed.load());
}

/* ---- SoftIrqOps: event wake + 8-slot SPSC FIFO payload ring --------------- */

COACT_TEST(windows_softirq_raise_take_cross_thread)
{
    Windows pal;
    Windows::SoftIrqHandle h{};
    REQUIRE(pal.softirq_init(h));

    std::atomic<bool> raised{false};
    struct Ctx { Windows* pal; Windows::SoftIrqHandle* h; int32_t payload;
                 std::atomic<bool>* ready; } ctx{&pal, &h, 4242, &raised};
    Windows::ThreadHandle t{};
    REQUIRE(pal.thread_create(t, [](void* a) {
        Ctx* c = static_cast<Ctx*>(a);
        c->h->pal->softirq_raise(*c->h, c->payload);
        c->ready->store(true, std::memory_order_release);
    }, &ctx));

    const int32_t got = pal.softirq_take(h, 1000U);
    pal.thread_join(t);

    CHECK_EQ(4242, got);
    CHECK(raised.load(std::memory_order_acquire));
    pal.softirq_deinit(h);
}

COACT_TEST(windows_softirq_fifo_and_busy_reject)
{
    Windows pal;
    Windows::SoftIrqHandle h{};
    REQUIRE(pal.softirq_init(h));

    // Fill the fixed 8-slot ring; the 9th raise must be rejected, not coalesced.
    bool published_ok = true;
    for (uint32_t i = 0U; i < Windows::kSoftIrqRingSlots; ++i) {
        const int32_t payload = static_cast<int32_t>(i) * 11;
        if (!pal.softirq_raise(h, payload)) { published_ok = false; }
    }
    CHECK(published_ok);
    CHECK(!pal.softirq_raise(h, 999));        // full: busy reject

    // Drain in FIFO order: N distinct raises -> N distinct payloads.
    bool fifo = true;
    for (uint32_t i = 0U; i < Windows::kSoftIrqRingSlots; ++i) {
        const int32_t expected = static_cast<int32_t>(i) * 11;
        const int32_t got = pal.softirq_take(h, 200U);
        if (got != expected) { fifo = false; }
    }
    CHECK(fifo);

    // Ring empty: the next take must time out (no off-by-one in the tail).
    const uint32_t t0 = static_cast<uint32_t>(pal.monotonic_ns() / 1000000ULL);
    const int32_t drained = pal.softirq_take(h, 60U);
    const uint32_t t1 = static_cast<uint32_t>(pal.monotonic_ns() / 1000000ULL);
    CHECK_EQ(-1, drained);
    CHECK((t1 - t0) >= 50U);

    pal.softirq_deinit(h);
}

/* ---- Sleep + tick quantization -------------------------------------------- */

COACT_TEST(windows_sleep_us_bounds)
{
    Windows pal;
    const uint32_t t0 = static_cast<uint32_t>(pal.monotonic_ns() / 1000000ULL);
    pal.sleep_us(30000U);                     // 30 ms
    const uint32_t t1 = static_cast<uint32_t>(pal.monotonic_ns() / 1000000ULL);
    CHECK((t1 - t0) >= 20U);
}

COACT_TEST(windows_set_tick_hz_quantizes)
{
    Windows pal;
    pal.set_tick_hz(100U);                    // 10 ms quantum
    const uint64_t a = pal.monotonic_ns();
    CHECK_EQ(0ULL, a % 10000000ULL);          // multiple of 10 ms
    pal.set_tick_hz(0U);                      // back to native resolution
    CHECK(pal.clock_resolution_ns() <= 1000000ULL);
}

}  // namespace

COACT_TEST_MAIN()
