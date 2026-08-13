// log_rtthread_demo.cpp - host demo for the single-core RT-Thread static log
// adapter (coact::diag::LogRtThread). Builds with -DCOACT_RTT_STUB so the
// RT-Thread API is backed by pthreads (test/rtthread_stub.h) and the whole
// adapter runs on Linux without a BSP.
//
// Verifies one end-to-end path through the writer:
//   [1] initialize() binds + start() launches the static writer thread.
//   [2] task-producer records (Info -> normal lane) are drained + rendered.
//   [3] ISR-producer records (Error -> critical lane, ISR-safe path) drained.
//   [4] normal-lane over-subscription drops at admission (queue-full drop).
//   [5] stop() drains both lanes and joins the writer; conservation holds.
//
// The demo produces a fixed final marker "[log-demo] TEST DONE" and exits 0 on
// all-pass so CTest / CI can grep-verify it. SPDX-License-Identifier: MIT
#include <cstdint>
#include <thread>
#include <chrono>

#include "coact/diag/log_rtthread.hpp"

namespace {

using coact::diag::LogRtError;
using coact::diag::LogLevel;
using coact::diag::LogLane;

/* Event ids (1-based; 0 reserved). */
enum : uint16_t {
    kEvtBoot = 1U,
    kEvtTask = 2U,
    kEvtIsr = 3U,
    kEvtBurst = 4U,
};

coact::diag::LogRtThread<> g_log;

volatile int g_pass = 0;
volatile int g_fail = 0;

void check(bool cond, const char* what) noexcept
{
    if (cond) {
        ++g_pass;
    }
    else {
        ++g_fail;
        (void)rt_kprintf("[log-demo] FAIL: %s\n", what);
    }
}

void sleep_ms(uint32_t ms) noexcept
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

int main()
{
    /* [1] lifecycle. */
    const LogRtError ie = g_log.initialize();
    check(LogRtError::kOk == ie, "initialize");
    if (LogRtError::kOk != ie) {
        (void)rt_kprintf("[log-demo] RESULT: PASS=%d FAIL=%d\n", g_pass, g_fail);
        (void)rt_kprintf("[log-demo] TEST DONE\n");
        return (0 == g_fail) ? 0 : 1;
    }
    const LogRtError se = g_log.start();
    check(LogRtError::kOk == se, "start");
    if (LogRtError::kOk != se) {
        (void)rt_kprintf("[log-demo] RESULT: PASS=%d FAIL=%d\n", g_pass, g_fail);
        (void)rt_kprintf("[log-demo] TEST DONE\n");
        return (0 == g_fail) ? 0 : 1;
    }

    /* [2] task producer: 20 Info records -> normal lane. */
    for (uint32_t i = 0U; i < 20U; ++i) {
        CMDFW_LOG_INFO(g_log.record_from_task<LogLevel::kInfo, kEvtTask>(1U, i));
    }
    /* [3] ISR producer: 4 Error records -> critical lane (ISR-safe path). */
    for (uint32_t i = 0U; i < 4U; ++i) {
        CMDFW_LOG_ERROR(g_log.record_from_isr<LogLevel::kError, kEvtIsr>(2U, i));
    }
    /* [4] burst: force normal-lane admission drops (the writer cannot drain
       this many records faster than the enqueue loop runs). */
    for (uint32_t i = 0U; i < 500U; ++i) {
        CMDFW_LOG_INFO(g_log.record_from_task<LogLevel::kInfo, kEvtBurst>(3U, i));
    }

    /* Wait for the writer to drain both lanes (bounded poll, test-only). */
    uint32_t waited = 0U;
    while (waited < 4000U) {
        const coact::diag::LogRtThread<>::LoggerT& lg = g_log.logger();
        if ((0U == lg.size(LogLane::kCritical)) && (0U == lg.size(LogLane::kNormal))) {
            break;
        }
        sleep_ms(2U);
        ++waited;
    }

    /* [5] stop: drain + join. */
    g_log.stop();

    const coact::diag::LogStats s = g_log.logger().snapshot();
    const uint32_t normal_enqueued = 20U + 500U;
    const uint32_t critical_enqueued = 4U;

    /* Conservation identity (design §9): accepted + dropped == enqueued. */
    check((s.accepted_normal + s.dropped_at_enqueue_normal) == normal_enqueued,
          "normal conservation");
    check((s.accepted_critical + s.dropped_at_enqueue_critical) ==
              critical_enqueued,
          "critical conservation");
    check(0U < s.dropped_at_enqueue_normal, "normal lane dropped on burst");
    check(0U == s.dropped_at_enqueue_critical, "critical lane never dropped");
    check(s.accepted_critical == critical_enqueued, "all critical accepted");
    check(s.drained_normal == s.accepted_normal, "normal drained == accepted");
    check(s.drained_critical == s.accepted_critical, "critical drained == accepted");
    check(0U == g_log.logger().size(LogLane::kCritical), "critical lane empty");
    check(0U == g_log.logger().size(LogLane::kNormal), "normal lane empty");
    check(g_log.sink_write_count() == (s.accepted_normal + s.accepted_critical),
          "sink writes == drained");
    check(0U == s.formatter_truncated, "formatter not truncated");
    check(0U == s.sink_failed, "sink never failed");

    (void)rt_kprintf("[log-demo] stats: crit_acc=%lu norm_acc=%lu norm_drop=%lu "
                     "sink=%lu\n",
                     (unsigned long)s.accepted_critical,
                     (unsigned long)s.accepted_normal,
                     (unsigned long)s.dropped_at_enqueue_normal,
                     (unsigned long)g_log.sink_write_count());
    (void)rt_kprintf("[log-demo] RESULT: PASS=%d FAIL=%d\n", g_pass, g_fail);
    (void)rt_kprintf("[log-demo] %s\n", (0 == g_fail) ? "ALL PASS" : "FAILURES");
    (void)rt_kprintf("[log-demo] TEST DONE\n");
    return (0 == g_fail) ? 0 : 1;
}
