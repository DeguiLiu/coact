// coact::diag::LogRtThreadBase implementation (single-core RT-Thread static log
// adapter). SPDX-License-Identifier: MIT
//
// The render-policy-dependent loop (writer_run + emit) is inline in the header
// template LogRtThread<Policy>; this translation unit carries the non-template
// machinery (compiled once per binary): lifecycle, producer wrappers behind the
// base class, platform hooks, plus RawHexPolicy::render (raw-hex fallback).
//
// All kernel resources are static members of LogRtThreadBase; only initialize()
// (rt_sem_init + Logger::bind) and start() (rt_thread_init + rt_thread_startup)
// touch the RT-Thread kernel, and only in task context. No rt_sem_create /
// rt_thread_create / rt_malloc / TLS / atexit.
#include "coact/diag/log_rtthread.hpp"

namespace coact {
namespace diag {

namespace {

// Convert a millisecond timeout to RT-Thread ticks (ceiling). 0 ms maps to the
// RT_WAITING_NO non-blocking take; large values cap at RT_WAITING_FOREVER.
rt_int32_t ms_to_ticks_impl(uint32_t timeout_ms) noexcept
{
    if (0U == timeout_ms) {
        return RT_WAITING_NO;
    }
    const uint64_t ticks =
        (static_cast<uint64_t>(timeout_ms) * RT_TICK_PER_SECOND + 999U) / 1000U;
    if (ticks >= static_cast<uint64_t>(RT_WAITING_FOREVER)) {
        return static_cast<rt_int32_t>(RT_WAITING_FOREVER);
    }
    return static_cast<rt_int32_t>(ticks);
}

}  // namespace

rt_int32_t ms_to_ticks(uint32_t timeout_ms) noexcept
{
    return ms_to_ticks_impl(timeout_ms);
}

LogRenderResult RawHexPolicy::render(const LogRecord& r, char* buf,
                                     uint16_t cap) noexcept
{
    if (0U == cap) {
        return {0U, true, false};
    }
    const int n = rt_snprintf(
        buf, static_cast<rt_size_t>(cap),
        "[%lu] e=%lu s=%lu a0=%08lx a1=%08lx a2=%08lx a3=%08lx\n",
        static_cast<unsigned long>(r.counter),
        static_cast<unsigned long>(r.event_id),
        static_cast<unsigned long>(r.source_id),
        static_cast<unsigned long>(r.arg0),
        static_cast<unsigned long>(r.arg1),
        static_cast<unsigned long>(r.arg2),
        static_cast<unsigned long>(r.arg3));
    if (n < 0) {
        return {0U, true, false};
    }
    const uint32_t written = static_cast<uint32_t>(n);
    if (written >= static_cast<uint32_t>(cap)) {
        return {static_cast<uint16_t>(cap - 1U), true, true};
    }
    return {static_cast<uint16_t>(written), true, false};
}

LogRtThreadBase::LogRtThreadBase(void (*run)(void*), void* self) noexcept
    : writer_tcb_{},
      writer_stack_{},
      wake_sem_{},
      join_sem_{},
      logger_{},
      stop_requested_(false),
      sink_write_count_(0U),
      state_(State::kUninitialized),
      last_error_(LogRtError::kOk),
      run_(run),
      run_self_(self)
{
}

LogRtError LogRtThreadBase::fail(LogRtError err) noexcept
{
    last_error_ = err;
    state_ = State::kInitFailed;
    return err;
}

LogRtError LogRtThreadBase::initialize() noexcept
{
    if (State::kReady == state_ || State::kStarted == state_) {
        return LogRtError::kAlreadyInitialized;
    }
    if (State::kStopped == state_ || State::kInitFailed == state_) {
        return last_error_;
    }

    /* Static wake/join semaphores: rt_sem_init in task context (ISR-safe
       release only). Any failure returns a definite error - no dynamic
       fallback. */
    if (RT_EOK != rt_sem_init(&wake_sem_, "coact_log_wake", 0U, RT_IPC_FLAG_PRIO)) {
        return fail(LogRtError::kSemInitFailed);
    }
    if (RT_EOK != rt_sem_init(&join_sem_, "coact_log_join", 0U, RT_IPC_FLAG_PRIO)) {
        return fail(LogRtError::kSemInitFailed);
    }

    /* Default hooks: rt_tick clock, rt_kprintf sink, semaphore wake. */
    LogClockOps clock{};
    clock.read_counter = &LogRtThreadBase::tick_counter_read;
    clock.frequency_hz = static_cast<uint32_t>(RT_TICK_PER_SECOND);
    clock.context = nullptr;

    LogSinkOps sink{};
    sink.write = &LogRtThreadBase::kprintf_sink;
    sink.context = this;

    LogWakeOps wake{};
    wake.signal_from_task = &LogRtThreadBase::wake_signal_task;
    wake.signal_from_isr = &LogRtThreadBase::wake_signal_isr;
    wake.wait_block = &LogRtThreadBase::wake_wait_block;
    wake.wait_bounded = &LogRtThreadBase::wake_wait_bounded;
    wake.context = this;

    /* Single-core queue gate: rt_hw_interrupt_disable/enable, O(1), no
       libatomic. */
    CriticalSection cs;
    cs.ctx = nullptr;
    cs.save = &LogRtThreadBase::irq_save;
    cs.restore = &LogRtThreadBase::irq_restore;

    if (false == logger_.bind(clock, sink, wake, cs)) {
        return fail(LogRtError::kBindFailed);
    }

    stop_requested_.store(false, std::memory_order_relaxed);
    sink_write_count_ = 0U;
    state_ = State::kReady;
    last_error_ = LogRtError::kOk;
    return LogRtError::kOk;
}

LogRtError LogRtThreadBase::start() noexcept
{
    if (State::kInitFailed == state_) {
        return last_error_;
    }
    if (State::kStarted == state_ || State::kStopped == state_) {
        return LogRtError::kAlreadyStarted;
    }
    if (State::kUninitialized == state_) {
        const LogRtError ie = initialize();
        if (LogRtError::kOk != ie) {
            return ie;
        }
    }

    /* The writer loop entry was injected by the derived template at
       construction (run_/run_self_); start() only wires it into the thread. */
    const rt_err_t terr = rt_thread_init(
        &writer_tcb_, "coact_log", run_, run_self_,
        writer_stack_, static_cast<rt_uint32_t>(kWriterStackBytes),
        kWriterPriority, static_cast<rt_uint32_t>(kWriterTick));
    if (RT_EOK != terr) {
        return fail(LogRtError::kThreadInitFailed);
    }

    const rt_err_t serr = rt_thread_startup(&writer_tcb_);
    if (RT_EOK != serr) {
        return fail(LogRtError::kThreadStartFailed);
    }

    state_ = State::kStarted;
    return LogRtError::kOk;
}

void LogRtThreadBase::stop() noexcept
{
    if (State::kStarted != state_) {
        return;
    }
    stop_requested_.store(true, std::memory_order_release);
    /* Wake the writer (it may be blocked in wait_block), let it drain both
       lanes, then block until it exits. */
    (void)rt_sem_release(&wake_sem_);
    (void)rt_sem_take(&join_sem_, static_cast<rt_int32_t>(RT_WAITING_FOREVER));
    state_ = State::kStopped;
}

bool LogRtThreadBase::bound() const noexcept
{
    return logger_.bound();
}

LogRtThreadBase::LoggerT& LogRtThreadBase::logger() noexcept
{
    return logger_;
}

const LogRtThreadBase::LoggerT& LogRtThreadBase::logger() const noexcept
{
    return logger_;
}

uint32_t LogRtThreadBase::sink_write_count() const noexcept
{
    return sink_write_count_;
}

uint32_t LogRtThreadBase::tick_counter_read(void* /*ctx*/) noexcept
{
    return static_cast<uint32_t>(rt_tick_get());
}

bool LogRtThreadBase::kprintf_sink(void* /*ctx*/, const char* bytes,
                                   uint16_t length) noexcept
{
    if (nullptr == bytes) {
        return false;
    }
    /* %.*s prints exactly `length` bytes; both the RT-Thread tiny vsnprintf
       (precision via '*') and the host stub (std vfprintf) support it. */
    (void)rt_kprintf("%.*s", static_cast<int>(length), bytes);
    return true;
}

void LogRtThreadBase::wake_signal_task(void* ctx) noexcept
{
    LogRtThreadBase* self = static_cast<LogRtThreadBase*>(ctx);
    if (nullptr != self) {
        (void)rt_sem_release(&self->wake_sem_);
    }
}

void LogRtThreadBase::wake_signal_isr(void* ctx) noexcept
{
    /* rt_sem_release is ISR-safe in RT-Thread: it schedules the writer but
       never blocks, so it may be called inside rt_interrupt_enter. */
    LogRtThreadBase* self = static_cast<LogRtThreadBase*>(ctx);
    if (nullptr != self) {
        (void)rt_sem_release(&self->wake_sem_);
    }
}

void LogRtThreadBase::wake_wait_block(void* ctx) noexcept
{
    LogRtThreadBase* self = static_cast<LogRtThreadBase*>(ctx);
    if (nullptr != self) {
        (void)rt_sem_take(&self->wake_sem_,
                          static_cast<rt_int32_t>(RT_WAITING_FOREVER));
    }
}

void LogRtThreadBase::wake_wait_bounded(void* ctx, uint32_t timeout_ms) noexcept
{
    LogRtThreadBase* self = static_cast<LogRtThreadBase*>(ctx);
    if (nullptr != self) {
        (void)rt_sem_take(&self->wake_sem_, ms_to_ticks_impl(timeout_ms));
    }
}

CriticalSection::Token LogRtThreadBase::irq_save(void* /*ctx*/) noexcept
{
    return static_cast<CriticalSection::Token>(rt_hw_interrupt_disable());
}

void LogRtThreadBase::irq_restore(void* /*ctx*/,
                                  CriticalSection::Token token) noexcept
{
    rt_hw_interrupt_enable(static_cast<rt_base_t>(token));
}

}  // namespace diag
}  // namespace coact
