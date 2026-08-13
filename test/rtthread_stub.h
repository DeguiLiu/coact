/**
 * coact RT-Thread host stub - backs RT-Thread API with pthreads for host tests.
 * Include before pal_rtthread.hpp via -DCOACT_RTT_STUB.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <pthread.h>
#include <sched.h>
#include <time.h>

typedef uint8_t   rt_uint8_t;
typedef uint32_t  rt_uint32_t;
typedef int32_t   rt_int32_t;
typedef uint32_t  rt_tick_t;
typedef int       rt_err_t;
typedef long      rt_size_t;
typedef uintptr_t rt_ubase_t;
typedef intptr_t  rt_base_t;

#define RT_EOK             0
#define RT_ETIMEOUT        7
#define RT_ENOMEM          8
#define RT_EINVAL          10
#define RT_WAITING_FOREVER ((rt_int32_t)0x7FFFFFFF)
#define RT_WAITING_NO      0
#define RT_IPC_FLAG_PRIO   0x01
#define RT_TICK_PER_SECOND 1000

#ifndef RT_ALIGN_SIZE
#define RT_ALIGN_SIZE 8
#endif

/* --- Interrupt masking: emulate single-core exclusivity on host -------- */
/* A single-core irq mask prevents both ISR and thread preemption. On the SMP
   host we emulate that with one global mutex acquired in irq_save and released
   in irq_restore, so the SingleCoreCriticalRing / pool critical sections are
   genuinely mutually exclusive in the host tests. Coact usage is strictly
   non-nested (one save/restore pair per pool/ring op); the depth counter is a
   safety net. inline thread_local so every TU shares one depth variable. */
inline pthread_mutex_t& stub_irq_mutex() noexcept
{
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    return m;
}
inline thread_local int g_irq_depth = 0;
inline rt_base_t rt_hw_interrupt_disable() noexcept
{
    if (0 == g_irq_depth++) {
        pthread_mutex_lock(&stub_irq_mutex());
    }
    return static_cast<rt_base_t>(g_irq_depth);
}
inline void rt_hw_interrupt_enable(rt_base_t) noexcept
{
    if (0 == --g_irq_depth) {
        pthread_mutex_unlock(&stub_irq_mutex());
    }
}

/* --- ISR nesting: always 0 on host -------------------------------------- */
/* inline (external linkage) so the test TU and the PAL TU share one counter;
   a `static thread_local` copy would diverge per TU. */
inline thread_local uint8_t g_isr_nest = 0U;
inline uint8_t rt_interrupt_get_nest() noexcept { return g_isr_nest; }
inline void stub_set_isr_nest(uint8_t n) noexcept { g_isr_nest = n; }

/* --- Semaphore ---------------------------------------------------------- */
struct rt_semaphore {
    pthread_mutex_t mtx;
    pthread_cond_t  cond;
    uint32_t        count;
    bool            init_done;   /* host-only: guard against double pthread init */
};
typedef struct rt_semaphore *rt_sem_t;

/* --- Fault injection (host-only) ----------------------------------------- */
/* Static PAL tests inject definite kernel failures (design §7.5: init/start
   errors must be propagated, never silently swallowed). Each hook returns the
   injected rt_err_t instead of RT_EOK. rt_sem_init/rt_thread_init return the
   fault WITHOUT side effects; rt_thread_startup returns the fault WITHOUT
   spawning the pthread. Reset with stub_reset_faults() between tests. */
inline rt_err_t& stub_sem_init_fault() noexcept
{
    static rt_err_t v = RT_EOK;
    return v;
}
inline rt_err_t& stub_thread_init_fault() noexcept
{
    static rt_err_t v = RT_EOK;
    return v;
}
inline rt_err_t& stub_thread_startup_fault() noexcept
{
    static rt_err_t v = RT_EOK;
    return v;
}
inline void stub_reset_faults() noexcept
{
    stub_sem_init_fault()      = RT_EOK;
    stub_thread_init_fault()   = RT_EOK;
    stub_thread_startup_fault() = RT_EOK;
}

inline rt_sem_t rt_sem_create(const char*, rt_uint32_t val, rt_uint8_t) noexcept
{
    rt_sem_t s = static_cast<rt_sem_t>(std::malloc(sizeof(rt_semaphore)));
    if (nullptr == s) { return nullptr; }
    pthread_mutex_init(&s->mtx, nullptr);
    pthread_cond_init(&s->cond, nullptr);
    s->count = val;
    s->init_done = true;
    return s;
}
inline rt_err_t rt_sem_init(rt_sem_t s, const char*, rt_uint32_t val, rt_uint8_t) noexcept
{
    if (nullptr == s) { return -RT_EINVAL; }
    if (RT_EOK != stub_sem_init_fault()) { return stub_sem_init_fault(); }
    if (!s->init_done) {
        pthread_mutex_init(&s->mtx, nullptr);
        pthread_cond_init(&s->cond, nullptr);
        s->init_done = true;
    }
    s->count = val;
    return RT_EOK;
}
inline rt_err_t rt_sem_detach(rt_sem_t s) noexcept
{
    if (nullptr == s) { return -RT_EINVAL; }
    pthread_mutex_destroy(&s->mtx);
    pthread_cond_destroy(&s->cond);
    s->init_done = false;
    return RT_EOK;
}
inline rt_err_t rt_sem_delete(rt_sem_t s) noexcept
{
    if (nullptr == s) { return -RT_EINVAL; }
    pthread_mutex_destroy(&s->mtx);
    pthread_cond_destroy(&s->cond);
    std::free(s);
    return RT_EOK;
}
inline rt_err_t rt_sem_take(rt_sem_t s, rt_int32_t ticks) noexcept
{
    if (nullptr == s) { return -RT_EINVAL; }
    pthread_mutex_lock(&s->mtx);
    if (RT_WAITING_NO == ticks) {
        rt_err_t r = (s->count > 0U) ? RT_EOK : -RT_ETIMEOUT;
        if (RT_EOK == r) { --s->count; }
        pthread_mutex_unlock(&s->mtx);
        return r;
    }
    if (RT_WAITING_FOREVER != ticks) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        const long ms = static_cast<long>(ticks); /* 1 tick == 1 ms at 1kHz */
        dl.tv_sec  += ms / 1000L;
        dl.tv_nsec += (ms % 1000L) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        while (s->count == 0U) {
            if (ETIMEDOUT == pthread_cond_timedwait(&s->cond, &s->mtx, &dl)) { break; }
        }
        rt_err_t r = (s->count > 0U) ? RT_EOK : -RT_ETIMEOUT;
        if (RT_EOK == r) { --s->count; }
        pthread_mutex_unlock(&s->mtx);
        return r;
    }
    while (s->count == 0U) { pthread_cond_wait(&s->cond, &s->mtx); }
    --s->count;
    pthread_mutex_unlock(&s->mtx);
    return RT_EOK;
}
inline rt_err_t rt_sem_release(rt_sem_t s) noexcept
{
    if (nullptr == s) { return -RT_EINVAL; }
    pthread_mutex_lock(&s->mtx);
    ++s->count;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mtx);
    return RT_EOK;
}

/* --- Thread ------------------------------------------------------------- */
struct rt_thread {
    pthread_t   tid;
    void      (*entry)(void*);
    void*       param;
    rt_ubase_t  user_data;
};
typedef struct rt_thread *rt_thread_t;

/* thread_local rt_thread_self storage. MUST be inline (external linkage) so the
   test TU and the PAL TU share ONE per-thread copy: a `static thread_local`
   would give each TU its own variable, so a thread created from the test TU
   (run_in_thread) would set a self pointer that RtThread::register_current_task
   (compiled in pal_rtthread.cpp) never sees. See g_isr_nest above. */
inline thread_local rt_thread_t tls_rtt_self = nullptr;

inline void* rtt_stub_run(void* arg) noexcept
{
    rt_thread_t t = static_cast<rt_thread_t>(arg);
    tls_rtt_self = t;
    t->entry(t->param);
    return nullptr;
}
inline rt_thread_t rt_thread_create(const char*, void(*entry)(void*), void* p,
                                    rt_uint32_t, rt_uint8_t, rt_uint32_t) noexcept
{
    rt_thread_t t = static_cast<rt_thread_t>(std::malloc(sizeof(rt_thread)));
    if (nullptr == t) { return nullptr; }
    t->entry = entry; t->param = p; t->user_data = 0U; t->tid = {};
    return t;
}
/* Static thread init: the PAL (pal_rtthread.cpp) uses rt_thread_init for the
   Dispatcher. The stub stores entry/param; the fixed stack array is ignored
   (the pthread gets the default stack). */
inline rt_err_t rt_thread_init(struct rt_thread *thread, const char* /*name*/,
                               void(*entry)(void*), void* parameter,
                               void* /*stack_start*/, rt_uint32_t /*stack_size*/,
                               rt_uint8_t /*priority*/, rt_uint32_t /*tick*/) noexcept
{
    if (nullptr == thread) { return -RT_EINVAL; }
    if (RT_EOK != stub_thread_init_fault()) { return stub_thread_init_fault(); }
    thread->entry = entry;
    thread->param = parameter;
    thread->user_data = 0U;
    thread->tid = {};
    return RT_EOK;
}
inline rt_err_t rt_thread_startup(rt_thread_t t) noexcept
{
    if (nullptr == t) { return -RT_EINVAL; }
    if (RT_EOK != stub_thread_startup_fault()) { return stub_thread_startup_fault(); }
    return (0 == pthread_create(&t->tid, nullptr, rtt_stub_run, t)) ? RT_EOK : -RT_ENOMEM;
}
inline rt_err_t rt_thread_delete(rt_thread_t t) noexcept
{
    if (nullptr == t) { return -RT_EINVAL; }
    if (t->tid != pthread_t{}) { pthread_join(t->tid, nullptr); }
    std::free(t); return RT_EOK;
}
inline rt_thread_t rt_thread_self() noexcept { return tls_rtt_self; }

/* --- Time --------------------------------------------------------------- */
inline rt_tick_t rt_tick_get() noexcept
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<rt_tick_t>(
        static_cast<uint64_t>(ts.tv_sec) * 1000U
        + static_cast<uint64_t>(ts.tv_nsec) / 1000000U);
}

/* --- Memory / console --------------------------------------------------- */
inline void* rt_malloc(rt_size_t n) noexcept { return std::malloc(static_cast<size_t>(n)); }
inline void  rt_free(void* p)       noexcept { std::free(p); }
inline int   rt_kprintf(const char* fmt, ...) noexcept
{
    va_list ap; va_start(ap, fmt);
    const int n = std::vfprintf(stderr, fmt, ap);
    va_end(ap); return n;
}
/* rt_snprintf mirrors the real RT-Thread kstdio API (included via rtthread.h
   -> rtklibc.h); the log adapter's writer renders into a fixed line buffer. */
inline int rt_snprintf(char* buf, rt_size_t size, const char* fmt, ...) noexcept
{
    va_list ap; va_start(ap, fmt);
    const int n = std::vsnprintf(buf, static_cast<size_t>(size), fmt, ap);
    va_end(ap); return n;
}
