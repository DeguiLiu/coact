// Static AOP worker-layer tests (design_isp_pipeline_static_aop A4):
// force the completion-reject fault path and prove the aspect chain is
// alive - the review found the path unreachable and untested when the
// result was a constant kOk.
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <atomic>
#include <cstdint>

#include "coact/fault.hpp"
#include "coact/pal_posix.hpp"
#include "isp_pipeline/sensor_irsc.hpp"

namespace {

using isp_demo::CompletionWorkerBase;
using isp_demo::PoolT;
using isp_demo::Rt;

struct FaultProbe {
    std::atomic<uint32_t> reports{0U};
    std::atomic<uint32_t> last_detail{0U};
    std::atomic<uint32_t> last_priority{0U};

    coact::FaultReporter ops() noexcept
    {
        return coact::FaultReporter{
            [](uint16_t, uint32_t detail, coact::FaultPriority priority,
               void* ctx) noexcept {
                FaultProbe* p = static_cast<FaultProbe*>(ctx);
                p->reports.fetch_add(1U, std::memory_order_relaxed);
                p->last_detail.store(detail, std::memory_order_relaxed);
                p->last_priority.store(static_cast<uint32_t>(priority),
                                       std::memory_order_relaxed);
            }, this};
    }
};

/* A worker whose execute_job always allocates through the base: with a
   zero-capacity pool every allocation fails, so the rejects-delta in
   execute() must yield kFault on every job. */
struct RejectWorker : CompletionWorkerBase<RejectWorker, uint16_t, 2U> {
    uint32_t completion_rejects{0U};
    static constexpr const char* name() noexcept { return "reject_w"; }
    static constexpr uint16_t kWorkerId = 0xFU;
    uint16_t instance_id{kWorkerId};

    void execute_job(const uint16_t&)
    {
        isp_demo::Layout* dead = allocate_completion(1U);
        (void)dead;
    }
};

/* A worker whose completion submission targets an UNBOUND AO: the
   coordinator returns RejectedState, which submit_completion must count as
   a lost completion (review P1) - the fault path must fire even though the
   pool allocation itself succeeds. */
struct UnboundTargetWorker
    : CompletionWorkerBase<UnboundTargetWorker, uint16_t, 2U> {
    uint32_t completion_rejects{0U};
    static constexpr const char* name() noexcept { return "unbound_w"; }
    static constexpr uint16_t kWorkerId = 0xEU;
    uint16_t instance_id{kWorkerId};

    void execute_job(const uint16_t&)
    {
        isp_demo::Layout* done = allocate_completion(1U);
        submit_completion(coact::kInvalidTarget, done);
    }
};

COACT_TEST(worker_aop_alloc_reject_fires_fault)
{
    static coact::pal::Posix pal;
    isp_demo::g_pal = &pal;
    /* Zero-capacity storage: every alloc_typed fails. */
    alignas(isp_demo::kPayloadAlign) static uint8_t tiny[16]{};
    PoolT pool;
    pool.init(tiny, sizeof(tiny), coact::detail::noop_cs());

    Rt rt(pal);
    RejectWorker w;
    FaultProbe probe;
    w.bind_fault(probe.ops());
    REQUIRE(w.start(&pool, &rt));
    REQUIRE(w.submit(1U));
    for (int32_t i = 0; i < 100; ++i) {
        if (w.executed_count() >= 1U) { break; }
        pal.sleep_us(1000);
    }
    w.stop();
    CHECK_EQ(1U, w.executed_count());
    CHECK_EQ(probe.reports.load(std::memory_order_relaxed), 1U);
    /* detail packs result<<16 | rejects: result kFault(3), rejects 1. */
    CHECK_EQ(probe.last_detail.load(std::memory_order_relaxed),
             (3U << 16U) | 1U);
    CHECK_EQ(probe.last_priority.load(std::memory_order_relaxed),
             static_cast<uint32_t>(coact::FaultPriority::kHigh));
}

COACT_TEST(worker_aop_dropped_completion_fires_fault)
{
    static coact::pal::Posix pal;
    isp_demo::g_pal = &pal;
    /* Real pool: allocation succeeds; the SUBMISSION is dropped because the
       target AO is not bound (RejectedState). */
    alignas(isp_demo::kPayloadAlign)
        static uint8_t storage[sizeof(isp_demo::Layout) * 4U
                               + isp_demo::kPayloadAlign]{};
    PoolT pool;
    pool.init(storage, sizeof(storage), coact::detail::noop_cs());

    Rt rt(pal);
    UnboundTargetWorker w;
    FaultProbe probe;
    w.bind_fault(probe.ops());
    REQUIRE(w.start(&pool, &rt));
    REQUIRE(w.submit(1U));
    for (int32_t i = 0; i < 100; ++i) {
        if (w.executed_count() >= 1U) { break; }
        pal.sleep_us(1000);
    }
    w.stop();
    CHECK_EQ(1U, w.executed_count());
    /* The alloc succeeded but the coordinator dropped the completion: the
       rejects-delta fault path must still fire exactly once. */
    CHECK_EQ(probe.reports.load(std::memory_order_relaxed), 1U);
    CHECK_EQ(0U, pool.used());
}

}  // namespace

COACT_TEST_MAIN()
