// coact::diag::LogRtThread - single-core RT-Thread static adapter over the
// frozen Logger core (design 2026-08-12-static-embedded-logging-design.md §5.4
// / §6.3). Wraps Logger<NormalCap, CriticalCap, ...> (which self-owns two
// CS-guarded DiagRing lane storages) with caller-provided static resources: a
// static `struct rt_thread` TCB, an RT_ALIGN_SIZE-aligned writer stack, and two
// static `struct rt_semaphore` (wake + join). No
// rt_thread_create / rt_sem_create / rt_malloc / TLS / atexit; the writer is
// launched with rt_thread_init + rt_thread_startup.
//
// Lifecycle (one init, one start, one stop; no restart):
//   initialize()  -> rt_sem_init(wake + join) + Logger::bind(default rt_tick
//                    clock, rt_kprintf sink, semaphore wake). Any failure keeps
//                    the logger disabled and returns a definite LogRtError.
//   start()       -> rt_thread_init + rt_thread_startup of the writer; returns
//                    kOk only after rt_thread_startup()==RT_EOK.
//   stop()        -> sets the stop flag, releases the wake semaphore, waits for
//                    the writer to drain both lanes and exit (join semaphore).
//                    Requires quiescent producers.
//
// Writer drain policy (design §5.4): at most 4 Critical records per 1 Normal
// batch; pure-blocking wait (rt_sem_take RT_WAITING_FOREVER) when both lanes
// are empty (no polling backoff). Each record is rendered into a fixed
// kLineBufferSize-byte line buffer and emitted through LogSinkOps.
//
// Render policy is compile-time (template <typename Policy>):
//   Policy::render(record, buf, cap) returns LogRenderResult; a catalog policy
//   returns handled=false on a miss so the adapter falls back to the built-in
//   RawHexPolicy (never loses diagnostics). No runtime function pointer, no
//   set-before-start contract.
//
// Size discipline: the heavy machinery (TCB, 4096 B writer stack, two
// semaphores, the Logger, lifecycle and producer wrappers) lives in the
// NON-template base LogRtThreadBase and is compiled once in log_rtthread.cpp.
// Only the render loop (writer_run + emit) is template-dependent and lives in
// the small header-only derived class. A single binary therefore carries one
// copy of the machinery plus one small render loop per instantiated policy.
//
// Producer paths: record_from_task() (task context) and record_from_isr()
// (ISR-safe path). In RT-Thread rt_sem_release is ISR-safe, so both funnel
// through the same Logger::record; the distinct names document ISR provenance
// so a board driver can call the ISR variant from a real handler.
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>

#ifdef COACT_RTT_STUB
#include "test/rtthread_stub.h"
#else
#include <rtthread.h>
#include <rthw.h>
#if defined(RT_USING_SMP) || !defined(RT_CPUS_NR) || (RT_CPUS_NR != 1)
#error "coact::diag::LogRtThread requires one non-SMP CPU"
#endif
#endif

#include "coact/diag/log.hpp"
#include "coact/pal.hpp"

namespace coact {
namespace diag {

// Built-in raw-hex renderer (declaration; definition in log_rtthread.cpp).
// Always handles (handled=true): the "e=/s=/a0=/..." line never loses a record.
struct RawHexPolicy {
    static LogRenderResult render(const LogRecord& r, char* buf,
                                  uint16_t cap) noexcept;
};

// Convert a millisecond timeout to RT-Thread ticks (ceiling). 0 ms maps to the
// RT_WAITING_NO non-blocking take; large values cap at RT_WAITING_FOREVER.
rt_int32_t ms_to_ticks(uint32_t timeout_ms) noexcept;

// Init/start status. Any failure returns a definite error; the adapter never
// falls back to dynamic create/malloc.
enum class LogRtError : uint8_t {
    kOk = 0U,
    kSemInitFailed,        // rt_sem_init failed (wake or join)
    kThreadInitFailed,     // rt_thread_init failed
    kThreadStartFailed,    // rt_thread_startup failed
    kAlreadyInitialized,   // initialize() after ready/start/stop
    kAlreadyStarted,       // start() after start, or after stop (no restart)
    kBindFailed,           // Logger::bind rejected hooks / critical section
};

// ---------------------------------------------------------------------------
// Non-template machinery: static resources, lifecycle, producer wrappers and
// the platform hooks. Defined in log_rtthread.cpp (one copy per binary).
// The writer thread entry is injected by the derived class through the
// constructor (run_/run_self_) so the render loop stays compile-time-policy
// specific without a virtual or a set-before-start runtime hook.
// ---------------------------------------------------------------------------
class LogRtThreadBase {
public:
    using Record = LogRecord;
    using Level = LogLevel;
    using Lane = LogLane;

    static_assert(std::atomic<bool>::is_always_lock_free,
                  "coact::diag: LogRtThread stop flag must not require libatomic");

    static constexpr uint16_t kNormalCapacity = 32U;
    static constexpr uint16_t kCriticalCapacity = 8U;
    static constexpr uint8_t kDebugWatermarkPercent = 50U;
    static constexpr uint8_t kInfoWatermarkPercent = 75U;
    static constexpr uint32_t kWriterStackBytes = 4096U;
    static constexpr uint8_t kWriterPriority = 20U;
    static constexpr uint8_t kWriterTick = 10U;
    static constexpr uint8_t kCriticalDrainBatch = 4U;   // design §5.4
    static constexpr uint16_t kLineBufferSize = 160U;

    // The core Logger owns two fixed-capacity DiagRing storages internally
    // (design §5.1) and guards them with the single injected CriticalSection;
    // only the capacities + watermarks are template parameters.
    using LoggerT = Logger<kNormalCapacity, kCriticalCapacity,
                           kDebugWatermarkPercent, kInfoWatermarkPercent>;

    // `run`/`self` wire the derived writer loop into the base writer thread.
    // Set once at construction; never changed at runtime.
    LogRtThreadBase(void (*run)(void*), void* self) noexcept;
    LogRtThreadBase(const LogRtThreadBase&) = delete;
    LogRtThreadBase& operator=(const LogRtThreadBase&) = delete;

    // ---- Lifecycle (one init, one start, one stop; no restart) ----------
    // rt_sem_init(wake + join) + Logger::bind with default ops. On failure the
    // logger stays disabled and a definite error is returned.
    LogRtError initialize() noexcept;
    // rt_thread_init + rt_thread_startup the writer thread. Auto-initializes
    // when initialize() was not called first.
    LogRtError start() noexcept;
    // Set the stop flag, release the wake semaphore, wait for the writer to
    // drain both lanes and exit. Producers must be quiescent: records produced
    // after stop() are not drained.
    void stop() noexcept;

    bool bound() const noexcept;
    LoggerT& logger() noexcept;
    const LoggerT& logger() const noexcept;

    // Writer-side sink success counter. The writer thread updates it; read it
    // only after stop() (the join semaphore provides the happens-before).
    // Truncation / sink-failure accounting is folded into the core Logger stats
    // via note_formatter_truncated() / note_sink_failed().
    uint32_t sink_write_count() const noexcept;

    // ---- Producer wrappers (task context) -------------------------------
    template <Level L, uint16_t EventId>
    void record_from_task(uint16_t source_id) noexcept
    {
        logger_.record<L, EventId>(source_id);
    }
    template <Level L, uint16_t EventId>
    void record_from_task(uint16_t source_id, uint32_t a0) noexcept
    {
        logger_.record<L, EventId>(source_id, a0);
    }
    template <Level L, uint16_t EventId>
    void record_from_task(uint16_t source_id, uint32_t a0, uint32_t a1) noexcept
    {
        logger_.record<L, EventId>(source_id, a0, a1);
    }
    template <Level L, uint16_t EventId>
    void record_from_task(uint16_t source_id, uint32_t a0, uint32_t a1,
                          uint32_t a2) noexcept
    {
        logger_.record<L, EventId>(source_id, a0, a1, a2);
    }
    template <Level L, uint16_t EventId>
    void record_from_task(uint16_t source_id, uint32_t a0, uint32_t a1,
                          uint32_t a2, uint32_t a3) noexcept
    {
        logger_.record<L, EventId>(source_id, a0, a1, a2, a3);
    }

    // ---- Producer wrappers (ISR-safe path) ------------------------------
    // In RT-Thread rt_sem_release is ISR-safe, and the core Logger now has a
    // distinct record_from_isr() entry that wakes the writer via the ISR-safe
    // signal_from_isr hook.
    template <Level L, uint16_t EventId>
    void record_from_isr(uint16_t source_id) noexcept
    {
        logger_.record_from_isr<L, EventId>(source_id);
    }
    template <Level L, uint16_t EventId>
    void record_from_isr(uint16_t source_id, uint32_t a0) noexcept
    {
        logger_.record_from_isr<L, EventId>(source_id, a0);
    }
    template <Level L, uint16_t EventId>
    void record_from_isr(uint16_t source_id, uint32_t a0, uint32_t a1) noexcept
    {
        logger_.record_from_isr<L, EventId>(source_id, a0, a1);
    }
    template <Level L, uint16_t EventId>
    void record_from_isr(uint16_t source_id, uint32_t a0, uint32_t a1,
                         uint32_t a2) noexcept
    {
        logger_.record_from_isr<L, EventId>(source_id, a0, a1, a2);
    }
    template <Level L, uint16_t EventId>
    void record_from_isr(uint16_t source_id, uint32_t a0, uint32_t a1,
                         uint32_t a2, uint32_t a3) noexcept
    {
        logger_.record_from_isr<L, EventId>(source_id, a0, a1, a2, a3);
    }

protected:
    enum class State : uint8_t {
        kUninitialized,
        kReady,
        kStarted,
        kStopped,
        kInitFailed,
    };

    // ---- Static resources (all in static storage; never allocated) ------
    struct rt_thread writer_tcb_{};
    alignas(RT_ALIGN_SIZE) rt_uint8_t writer_stack_[kWriterStackBytes]{};
    struct rt_semaphore wake_sem_{};
    struct rt_semaphore join_sem_{};

    LoggerT logger_{};

    std::atomic<bool> stop_requested_{false};
    uint32_t sink_write_count_ = 0U;
    State state_ = State::kUninitialized;
    LogRtError last_error_ = LogRtError::kOk;
    void (*run_)(void*) = nullptr;
    void* run_self_ = nullptr;

    // ---- Platform hooks (capture-less; context == this) ------------------
    static uint32_t tick_counter_read(void* ctx) noexcept;
    static bool kprintf_sink(void* ctx, const char* bytes, uint16_t length) noexcept;
    static void wake_signal_task(void* ctx) noexcept;
    static void wake_signal_isr(void* ctx) noexcept;
    static void wake_wait_block(void* ctx) noexcept;
    static void wake_wait_bounded(void* ctx, uint32_t timeout_ms) noexcept;
    static CriticalSection::Token irq_save(void* ctx) noexcept;
    static void irq_restore(void* ctx, CriticalSection::Token token) noexcept;

    LogRtError fail(LogRtError err) noexcept;
};

// ---------------------------------------------------------------------------
// Render-policy template: only the writer loop + render step are here (the
// machinery is in LogRtThreadBase). Policy must provide
//   static LogRenderResult render(const LogRecord&, char*, uint16_t) noexcept;
// The default RawHexPolicy reproduces the historical raw-hex output.
// ---------------------------------------------------------------------------
template <typename Policy = RawHexPolicy>
class LogRtThread : public LogRtThreadBase {
public:
    using LogRtThreadBase::LogRtThreadBase;

    LogRtThread() noexcept
        : LogRtThreadBase(&LogRtThread::writer_thread_entry, this)
    {
    }

private:
    static void writer_thread_entry(void* param) noexcept
    {
        LogRtThread* self = static_cast<LogRtThread*>(param);
        if (nullptr != self) {
            self->writer_run();
        }
    }
    void writer_run() noexcept
    {
        for (;;) {
            bool drained_any = false;

            /* Up to 4 Critical per 1 Normal (design §5.4). */
            for (uint8_t i = 0U; i < kCriticalDrainBatch; ++i) {
                Record r;
                if (logger_.pop_from_lane(Lane::kCritical, r)) {
                    emit(r);
                    drained_any = true;
                }
                else {
                    break;
                }
            }
            Record normal;
            if (logger_.pop_from_lane(Lane::kNormal, normal)) {
                emit(normal);
                drained_any = true;
            }

            /* Exit only after the stop flag is set AND both lanes are drained. */
            if (stop_requested_.load(std::memory_order_acquire) &&
                (0U == logger_.size(Lane::kCritical)) &&
                (0U == logger_.size(Lane::kNormal))) {
                break;
            }
            if (false == drained_any) {
                if (stop_requested_.load(std::memory_order_acquire)) {
                    break;
                }
                /* Pure blocking wait; no polling backoff (design §5.4). */
                const LogWakeOps wake = logger_.wake();
                if (nullptr != wake.wait_block) {
                    wake.wait_block(wake.context);
                }
            }
        }
        (void)rt_sem_release(&join_sem_);
    }
    void emit(const Record& r) noexcept
    {
        char buf[kLineBufferSize];
        bool truncated = false;
        uint16_t len = 0U;
        bool handled = false;
        const LogRenderResult rendered = Policy::render(r, buf, kLineBufferSize);
        if (rendered.handled && (rendered.length > 0U) &&
            (rendered.length < kLineBufferSize) &&
            ('\n' == buf[rendered.length - 1U])) {
            buf[rendered.length] = '\0';
            len = rendered.length;
            truncated = rendered.truncated;
            handled = true;
        }
        else if (!rendered.handled) {
            logger_.note_catalog_miss();
        }
        else {
            truncated = true;
        }
        if (!handled) {
            const LogRenderResult raw = RawHexPolicy::render(r, buf, kLineBufferSize);
            len = raw.length;
            truncated = truncated || raw.truncated;
        }
        if (truncated) {
            logger_.note_formatter_truncated();
        }
        const LogSinkOps sink = logger_.sink();
        if (nullptr != sink.write) {
            const bool ok = sink.write(sink.context, buf, len);
            if (ok) {
                ++sink_write_count_;
            }
            else {
                logger_.note_sink_failed();
            }
        }
    }
};

}  // namespace diag
}  // namespace coact
