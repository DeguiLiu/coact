// coact POSIX PAL implementation.
// SPDX-License-Identifier: MIT
#include "coact/pal_posix.hpp"

#include <cstring>
#include <ctime>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

namespace coact {
namespace pal {

/* SoftIrqSignal is declared in pal_posix.hpp as Posix::SoftIrqSignal — keep
   the value in one place so header users and the .cpp cannot drift. */

/* pthread void*(void*) -> ThreadEntry void(void*) adapter. Reads entry and
   context from the ThreadHandle the create call passed as arg. */
void* Posix::thread_trampoline(void* arg) noexcept
{
    ThreadHandle* t = static_cast<ThreadHandle*>(arg);
    t->entry(t->context);
    return nullptr;
}

thread_local ExecutionContext Posix::tls_ctx_ = {
    ContextKind::Task, 0U, 0U, false
};

Posix::Posix() noexcept
    : wake_(false),
      thread_valid_(false),
      thread_{},
      user_entry_(nullptr),
      user_ctx_(nullptr),
      tick_hz_(0U),
      ns_per_tick_(0U),
      last_progress_ns_(0U)
{
    pthread_mutex_init(&mutex_, nullptr);
    pthread_cond_init(&cond_, nullptr);
}

void Posix::set_tick_hz(uint32_t hz) noexcept
{
    if (0U == hz || hz > 1000000000U) {
        tick_hz_ = 0U;
        ns_per_tick_ = 0U;
        return;
    }
    tick_hz_ = hz;
    ns_per_tick_ = 1000000000ULL / hz;
}

void Posix::set_dispatcher_stack_bytes(uint32_t /*bytes*/) noexcept
{
    /* pthread default stack (8 MiB) is used; no-op. */
}

/* Interrupt masking is a no-op on POSIX host. The token carries no state. */
CriticalToken Posix::irq_save() noexcept
{
    CriticalToken tok;
    tok.value = 0U;
    return tok;
}

void Posix::irq_restore(CriticalToken /*token*/) noexcept
{
    /* no-op on POSIX */
}

bool Posix::register_current_task(LogicalPrio prio) noexcept
{
    if (kInvalidPrio == prio || prio > kMaxPrio) {
        return false;
    }
    tls_ctx_.kind = ContextKind::Task;
    tls_ctx_.logical_prio = prio;
    tls_ctx_.prio_valid = true;
    tls_ctx_.direct_depth = 0U;
    return true;
}

ExecutionContext Posix::current_context() const noexcept
{
    return tls_ctx_;
}

uint64_t Posix::monotonic_ns() const noexcept
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t raw = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
                 + static_cast<uint64_t>(ts.tv_nsec);
    if (0U != tick_hz_) {
        raw = (raw / ns_per_tick_) * ns_per_tick_;
    }
    return raw;
}

uint64_t Posix::clock_resolution_ns() const noexcept
{
    if (0U != tick_hz_) {
        return ns_per_tick_;
    }
    struct timespec ts;
    clock_getres(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

void Posix::wait_dispatcher(uint32_t timeout_ms) noexcept
{
    pthread_mutex_lock(&mutex_);
    if (!wake_) {
        if (0U == timeout_ms) {
            pthread_cond_wait(&cond_, &mutex_);
        }
        else {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec  += static_cast<time_t>(timeout_ms / 1000U);
            deadline.tv_nsec += static_cast<long>((timeout_ms % 1000U) * 1000000UL);
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec  += 1;
                deadline.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&cond_, &mutex_, &deadline);
        }
    }
    wake_ = false;
    pthread_mutex_unlock(&mutex_);
}

void Posix::signal_dispatcher_from_task() noexcept
{
    pthread_mutex_lock(&mutex_);
    wake_ = true;
    pthread_cond_signal(&cond_);
    pthread_mutex_unlock(&mutex_);
}

// Thread-simulated ISR only: pthread_mutex_lock / pthread_cond_signal are not
// async-signal-safe, so this must never run from a real POSIX signal handler.
// Host tests drive "ISR" producers from ordinary threads under the mutex, which
// is the sole supported use (P2-11).
void Posix::signal_dispatcher_from_isr() noexcept
{
    pthread_mutex_lock(&mutex_);
    wake_ = true;
    pthread_cond_signal(&cond_);
    pthread_mutex_unlock(&mutex_);
}

void* Posix::dispatcher_entry(void* arg) noexcept
{
    Posix* self = static_cast<Posix*>(arg);
    tls_ctx_.kind = ContextKind::Dispatcher;
    tls_ctx_.logical_prio = 0U;
    tls_ctx_.prio_valid = false;
    tls_ctx_.direct_depth = 0U;
    self->user_entry_(self->user_ctx_);
    return nullptr;
}

void Posix::start_dispatcher(ThreadEntry entry, void* context) noexcept
{
    user_entry_ = entry;
    user_ctx_ = context;
    thread_valid_ = (0 == pthread_create(&thread_, nullptr, &Posix::dispatcher_entry, this));
}

void Posix::join_dispatcher() noexcept
{
    if (thread_valid_) {
        pthread_join(thread_, nullptr);
        thread_valid_ = false;
    }
}

void Posix::watchdog_progress(uint32_t /*marker*/) noexcept
{
    /* Single writer (the Dispatcher thread): a relaxed store is sufficient;
       readers use a relaxed load and judge staleness by elapsed time. */
    last_progress_ns_.store(monotonic_ns(), std::memory_order_relaxed);
}

uint64_t Posix::dispatcher_progress_ns() const noexcept
{
    return last_progress_ns_.load(std::memory_order_relaxed);
}

bool Posix::dispatcher_alive_within(uint32_t window_ms) const noexcept
{
    const uint64_t last = last_progress_ns_.load(std::memory_order_relaxed);
    if (0U == last) {
        return false;  /* never beat: not proven alive */
    }
    const uint64_t elapsed = monotonic_ns() - last;
    return elapsed < (static_cast<uint64_t>(window_ms) * 1000000ULL);
}

void Posix::enter_direct() noexcept
{
    ++tls_ctx_.direct_depth;
}

void Posix::leave_direct() noexcept
{
    if (tls_ctx_.direct_depth > 0U) {
        --tls_ctx_.direct_depth;
    }
}

/* ---------------------------------------------------------------------------
 * SemOps family: counting semaphore over mutex + cond (no sem_t — the
 * mutex+cond form is exception-free, clocked with CLOCK_REALTIME deadlines
 * exactly like wait_dispatcher, and shares the CondOps machinery).
 * ------------------------------------------------------------------------- */

bool Posix::sem_init(SemHandle& sem, uint32_t initial) noexcept
{
    if (0U != pthread_mutex_init(&sem.mtx, nullptr)) {
        return false;
    }
    if (0U != pthread_cond_init(&sem.cond, nullptr)) {
        pthread_mutex_destroy(&sem.mtx);
        return false;
    }
    sem.count = initial;
    sem.pal   = this;
    return true;
}

bool Posix::sem_take(SemHandle& sem, uint32_t timeout_ms) noexcept
{
    pthread_mutex_lock(&sem.mtx);
    if (0U == timeout_ms) {
        /* non-blocking try */
        const bool ok = (sem.count > 0U);
        if (ok) { --sem.count; }
        pthread_mutex_unlock(&sem.mtx);
        return ok;
    }
    if (kWaitForever == timeout_ms) {
        while (0U == sem.count) {
            pthread_cond_wait(&sem.cond, &sem.mtx);
        }
        --sem.count;
        pthread_mutex_unlock(&sem.mtx);
        return true;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec  += static_cast<time_t>(timeout_ms / 1000U);
    deadline.tv_nsec += static_cast<long>((timeout_ms % 1000U) * 1000000UL);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    while (0U == sem.count) {
        if (ETIMEDOUT == pthread_cond_timedwait(&sem.cond, &sem.mtx, &deadline)) {
            break;
        }
    }
    const bool ok = (sem.count > 0U);
    if (ok) { --sem.count; }
    pthread_mutex_unlock(&sem.mtx);
    return ok;
}

void Posix::sem_release(SemHandle& sem) noexcept
{
    pthread_mutex_lock(&sem.mtx);
    ++sem.count;
    pthread_cond_signal(&sem.cond);
    pthread_mutex_unlock(&sem.mtx);
}

void Posix::sem_release_from_isr(SemHandle& sem) noexcept
{
    /* Host ISR simulation: pthread_cond_signal is not async-signal-safe, so
       this must only be called from a normal thread standing in for the ISR
       (same limitation as signal_dispatcher_from_isr, P2-11). */
    sem_release(sem);
}

void Posix::sem_deinit(SemHandle& sem) noexcept
{
    pthread_mutex_destroy(&sem.mtx);
    pthread_cond_destroy(&sem.cond);
}

/* ---------------------------------------------------------------------------
 * MutexOps family: plain pthread_mutex_t.
 * ------------------------------------------------------------------------- */

bool Posix::mutex_init(MutexHandle& m) noexcept
{
    if (0U != pthread_mutex_init(&m.mtx, nullptr)) {
        return false;
    }
    m.pal = this;
    return true;
}

void Posix::mutex_lock(MutexHandle& m) noexcept
{
    pthread_mutex_lock(&m.mtx);
}

void Posix::mutex_unlock(MutexHandle& m) noexcept
{
    pthread_mutex_unlock(&m.mtx);
}

void Posix::mutex_deinit(MutexHandle& m) noexcept
{
    pthread_mutex_destroy(&m.mtx);
}

/* ---------------------------------------------------------------------------
 * CondOps family: hand-off condition variable. timeout_ms 0 = wait forever
 * (the Dispatcher wait convention), kWaitForever likewise; any other value
 * bounds the wait and returns on timeout (best effort for the demo drains).
 * ------------------------------------------------------------------------- */

bool Posix::cond_init(CondHandle& c) noexcept
{
    if (0U != pthread_cond_init(&c.cond, nullptr)) {
        return false;
    }
    c.pal = this;
    return true;
}

void Posix::cond_wait(CondHandle& c, MutexHandle& m, uint32_t timeout_ms) noexcept
{
    if (0U == timeout_ms || kWaitForever == timeout_ms) {
        pthread_cond_wait(&c.cond, &m.mtx);
        return;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec  += static_cast<time_t>(timeout_ms / 1000U);
    deadline.tv_nsec += static_cast<long>((timeout_ms % 1000U) * 1000000UL);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    (void)pthread_cond_timedwait(&c.cond, &m.mtx, &deadline);
}

void Posix::cond_signal(CondHandle& c) noexcept
{
    pthread_cond_signal(&c.cond);
}

void Posix::cond_broadcast(CondHandle& c) noexcept
{
    pthread_cond_broadcast(&c.cond);
}

void Posix::cond_deinit(CondHandle& c) noexcept
{
    pthread_cond_destroy(&c.cond);
}

/* ---------------------------------------------------------------------------
 * ThreadOps family: pthread create/join for demo worker threads.
 * ------------------------------------------------------------------------- */

bool Posix::thread_create(ThreadHandle& t, ThreadEntry entry, void* context) noexcept
{
    /* Store entry+context in the handle: the handle outlives the thread
       (join is mandatory), so this is heap-free. */
    t.entry   = entry;
    t.context = context;
    t.pal     = this;
    t.valid   = (0U == pthread_create(&t.tid, nullptr, &thread_trampoline, &t));
    return t.valid;
}

void Posix::thread_join(ThreadHandle& t) noexcept
{
    if (t.valid) {
        pthread_join(t.tid, nullptr);
        t.valid = false;
    }
}

void Posix::sleep_us(uint32_t us) noexcept
{
    struct timespec ts;
    ts.tv_sec  = static_cast<time_t>(us / 1000000U);
    ts.tv_nsec = static_cast<long>((us % 1000000U) * 1000UL);
    while (0 != nanosleep(&ts, &ts) && EINTR == errno) {
        /* resume the remainder after a signal */
    }
}

/* ---------------------------------------------------------------------------
 * SoftIrqOps family: signalfd-backed software interrupt simulation.
 *
 * NO signal handler is ever registered: SIGRTMIN stays blocked on every
 * thread that touches this path, and signalfd converts the queued signal
 * into an fd event. Consumption therefore happens entirely in thread context
 * where malloc / locks are allowed (the async-signal-safe limitation that
 * would poison a pthread_kill+handler design never applies). sigqueue
 * carries the payload in sival_int; standard-realtime signal queuing
 * semantics apply — consecutive identical raises may be COALESCED by the
 * kernel if the consumer has not drained the earlier one yet (see
 * test_softirq.cpp for the documented in-order delivery contract).
 * ------------------------------------------------------------------------- */

bool Posix::softirq_init(SoftIrqHandle& h) noexcept
{
    /* Block SIGRTMIN BEFORE creating the signalfd: without the mask an
       unhandled SIGRTMIN would take its default action and kill the process
       on the first raise. The mask is recorded on the installing (consumer)
       thread so deinit() can restore it. */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SoftIrqSignal);
    if (0 != pthread_sigmask(SIG_BLOCK, &mask, nullptr)) {
        return false;
    }
    h.consumer = pthread_self();
    h.pal      = this;
    h.fd       = signalfd(-1, &mask, SFD_CLOEXEC);
    if (-1 == h.fd) {
        (void)pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);
        h.consumer = pthread_t{};
        h.pal = nullptr;
    }
    return (-1 != h.fd);
}

bool Posix::softirq_raise(SoftIrqHandle& /*h*/, int32_t payload) noexcept
{
    /* Block SIGRTMIN in the producer too: an unblocked producer thread would
       synchronously consume the queued signal itself before the consumer's
       signalfd could observe it. The sigmask write is per-thread and cheap.
       NOTE: the handle does not carry the consumer's PID; sigqueue targets
       this process (getpid()), which is the only supported topology — the
       softirq producer and consumer always live in one process. */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SoftIrqSignal);
    if (0 != pthread_sigmask(SIG_BLOCK, &mask, nullptr)) {
        return false;
    }
    siginfo_t si;
    std::memset(&si, 0, sizeof(si));
    si.si_code  = SI_QUEUE;
    si.si_pid   = static_cast<pid_t>(getpid());
    si.si_uid   = static_cast<uid_t>(geteuid());
    si.si_value.sival_int = payload;
    return (0 == sigqueue(getpid(), SoftIrqSignal, si.si_value));
}

int32_t Posix::softirq_take(SoftIrqHandle& h, uint32_t timeout_ms) noexcept
{
    struct pollfd pfd;
    pfd.fd      = h.fd;
    pfd.events  = POLLIN;
    pfd.revents = 0;
    int timeout = (kWaitForever == timeout_ms) ? -1
                                               : static_cast<int>(timeout_ms);
    const int pret = poll(&pfd, 1, timeout);
    if (pret <= 0) {
        return -1;   /* timeout (0) or poll error (-1): same caller contract */
    }
    struct signalfd_siginfo ssi;
    const ssize_t n = read(h.fd, &ssi, sizeof(ssi));
    if (n != static_cast<ssize_t>(sizeof(ssi))) {
        return -1;
    }
    return static_cast<int32_t>(ssi.ssi_int);
}

void Posix::softirq_deinit(SoftIrqHandle& h) noexcept
{
    if (-1 != h.fd) {
        close(h.fd);
        h.fd = -1;
    }
    /* Restore the consumer thread's mask so the caller's process-wide signal
       state is unchanged after the test (contract: "deinit ... restores the
       consumer thread's signal mask on Linux"). SIG_UNBLOCK unblocks every
       signal present in `mask`; we build `mask` with the one signal we
       blocked during init(). */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SoftIrqSignal);
    (void)pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);
}

}  // namespace pal
}  // namespace coact
