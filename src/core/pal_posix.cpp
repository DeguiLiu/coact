// coact POSIX PAL implementation.
// SPDX-License-Identifier: MIT
#include "coact/pal_posix.hpp"

#include <cstring>
#include <ctime>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

namespace coact {
namespace pal {

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
      ns_per_tick_(0U)
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
    /* no-op on POSIX host */
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

}  // namespace pal
}  // namespace coact
