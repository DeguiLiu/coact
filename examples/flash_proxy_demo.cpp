// coact complex example: Flash (NAND) owner-AO serialization with a non-AO
// DMA worker, simulated interrupt callbacks, and resource contention.
//
// Demonstrates the "owner AO" pattern for a shared device: the NAND and its
// data have a single owning AO; every other AO requests read/write through
// events, and the owner serializes access by running one HSM to completion per
// request. No mutex, no blackboard.
//
// Complexity beyond a toy:
//   1. A non-AO worker thread models the async NAND hardware + DMA controller.
//      The owner AO never blocks on flash: it fills a shared DMA buffer, hands
//      the job to the worker, and returns. The worker sleeps to simulate
//      erase/program/read latency, drives the "DMA transfer", then posts
//      completion as an event OUTSIDE the dispatcher (a simulated interrupt
//      callback). The interrupt-arrival span and the dispatch span are timed
//      separately, matching the qactive_demo_nonblock proxy-thread model.
//   2. Resource contention is explicit: the DMA buffer is a single shared
//      resource between the owner AO (producer) and the worker (consumer,
//      acting as the DMA engine). Single-flight is enforced because the owner
//      AO is single-execution and at most one job is outstanding. A
//      deliberately narrow pool exercises under-allocation back-pressure (the
//      fan-out drops a notice when the pool is exhausted).
//   3. Reference-counted fan-out: a committed write fans out a data-updated
//      event to every subscriber AO.
//   4. Request/response queries: the requester carries its own TargetId and
//      receives a kDataReady response with an inlined snapshot (zero sharing
//      with the owner's private buffer).
//
// Modern C++17 throughout: standard-layout composed EventBlockLayout with a
// typed alloc (meta region + trivial payload), constexpr constants, scoped
// enums, std::array, std::atomic flags, no raw new/delete, no dynamic
// allocation on the hot path.
//
// SPDX-License-Identifier: MIT

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>

#include <pthread.h>
#include <unistd.h>

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/pal_posix.hpp"
#include "coact/pool.hpp"
#include "coact/runtime.hpp"

namespace flash_demo {

using coact::Event;
using coact::EventQos;
using coact::Hsm;
using coact::LogicalPrio;
using coact::PriorityClass;
using coact::StateDef;
using coact::TargetId;
using coact::TransitionDef;
using coact::TransitionKind;

// ---------------------------------------------------------------------------
// Sizes and capacities (compile-time, one place to tune).
// ---------------------------------------------------------------------------
constexpr uint8_t  kSubscriberCount = 2U;   // writer + reader observers
constexpr uint16_t kFlashSize        = 256U; // simulated flash capacity
constexpr uint16_t kFlashDmaMax      = 128U; // max bytes per DMA transfer
constexpr uint16_t kPoolBlocks       = 24U;  // deliberately tight -> back-pressure

// ---------------------------------------------------------------------------
// Event vocabulary. One unified layout: a routing Meta header + a fixed-size
// trivial Payload (the DMA data). A single pool serves every event, which is
// the natural fit for the composed-layout typed alloc.
// ---------------------------------------------------------------------------

// Signals (0 is reserved for the init event).
enum class Sig : uint16_t {
    kWriteReq     = 1U,
    kReadReq      = 2U,
    kHwDone       = 3U,
    kDataUpdated  = 4U,
    kDataReady    = 5U,
    kTriggerWrite = 6U,  // demo script: ask the writer AO to issue a write
    kTriggerRead  = 7U   // demo script: ask the reader AO to issue a query
};

// Routing metadata carried in the event-block meta region. Read/written by the
// owner AO and callers; never shared with the worker except via the DMA buffer.
struct IoMeta {
    TargetId reply_to{};    // for request/response: who receives the answer
    uint32_t addr{0U};      // flash offset
    uint16_t len{0U};       // bytes transferred
    uint16_t flags{0U};     // bit0 = is_write, bit1 = reply_expected
    uint32_t seq{0U};       // DMA job sequence
    int32_t  result{0};     // hardware completion status
};

// Payload: the DMA data region. Must be trivial (pool reclaim runs no dtor).
struct Payload {
    std::array<uint8_t, kFlashDmaMax> bytes{};
};

constexpr size_t kPayloadAlign = 64U;   // model a DMA-cacheline-aligned buffer

// One layout for every event: Event + IoMeta + aligned Payload.
using Layout = coact::EventBlockLayout<IoMeta, sizeof(Payload), kPayloadAlign>;

using PoolT = coact::EventPool<static_cast<uint16_t>(sizeof(Layout)),
                               kPoolBlocks,
                               coact::HostSmpProfile,
                               kPayloadAlign>;

using Rt = coact::Runtime<coact::DefaultConfig, coact::pal::Posix>;

// Convenience: a typed event block with its payload already cast.
struct Evt {
    Layout block;
    Payload* data() { return reinterpret_cast<Payload*>(&block.payload[0]); }
};

// ---------------------------------------------------------------------------
// Non-AO worker: models the NAND controller + DMA engine. It is NOT on the
// dispatcher thread. It communicates with the owner AO only through:
//   (a) a shared DMA buffer + an atomic handoff (the "resource"), and
//   (b) submitting kHwDone events (the "signal").
// ---------------------------------------------------------------------------
struct NandWorker {
    struct Job {
        bool     valid{false};
        bool     is_write{false};
        uint32_t addr{0U};
        uint16_t len{0U};
        uint32_t seq{0U};
    };

    // Shared resource: the DMA-aligned buffer, plus the handoff job mailbox.
    // Guarded by mutex/cond on host; on RT-Thread this maps to a DMA
    // descriptor + completion IRQ.
    alignas(kPayloadAlign) std::array<uint8_t, kFlashDmaMax> dma_buf{};
    pthread_mutex_t mtx{};
    pthread_cond_t  cond{};
    Job             job{};
    bool            running{false};
    uint32_t        seq_counter{0U};

    // coact plumbing for the completion submit (set during start).
    PoolT*   pool{nullptr};
    Rt*      rt{nullptr};
    TargetId owner_target{};

    void start(PoolT* p, Rt* r, TargetId owner)
    {
        pool = p;
        rt = r;
        owner_target = owner;
        pthread_mutex_init(&mtx, nullptr);
        pthread_cond_init(&cond, nullptr);
        running = true;
        pthread_create(&thread_, nullptr, &NandWorker::run_tramp, this);
    }

    bool submit(Job j)
    {
        pthread_mutex_lock(&mtx);
        if (job.valid) {
            pthread_mutex_unlock(&mtx);
            return false;   // single-flight
        }
        job = j;
        job.valid = true;
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&mtx);
        return true;
    }

    void stop()
    {
        pthread_mutex_lock(&mtx);
        running = false;
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&mtx);
        pthread_join(thread_, nullptr);
    }

private:
    pthread_t thread_{};

    static void* run_tramp(void* arg)
    {
        static_cast<NandWorker*>(arg)->run();
        return nullptr;
    }

    void run()
    {
        for (;;) {
            pthread_mutex_lock(&mtx);
            while (running && !job.valid) {
                pthread_cond_wait(&cond, &mtx);
            }
            if (!running && !job.valid) {
                pthread_mutex_unlock(&mtx);
                break;
            }
            Job j = job;
            job.valid = false;
            pthread_mutex_unlock(&mtx);

            // --- model physical NAND latency + DMA transfer ---
            const char* op = j.is_write ? "ERASE+PROG" : "READ";
            const uint64_t t0 = now_ns();
            usleep(j.is_write ? 40000U : 5000U);   // erase/prog slower than read
            const uint64_t t_done = now_ns();

            if (j.is_write) {
                std::printf("[nand] DMA-out addr=0x%x len=%u seq=%u\n",
                            j.addr, j.len, j.seq);
            } else {
                // "DMA completes": fill the shared buffer with flash contents.
                for (uint16_t i = 0; i < j.len; ++i) {
                    dma_buf[i] = static_cast<uint8_t>((j.addr + i) & 0xFFu);
                }
                std::printf("[nand] DMA-in  addr=0x%x len=%u seq=%u\n",
                            j.addr, j.len, j.seq);
            }

            // --- simulated interrupt callback: submit kHwDone off-dispatcher.
            const uint64_t t_irq = now_ns();
            Layout* hd = pool->alloc_typed<Layout, Payload, kPayloadAlign>(
                static_cast<uint16_t>(Sig::kHwDone));
            if (nullptr == hd) {
                std::printf("[nand] pool exhausted, completion dropped\n");
                continue;
            }
            hd->meta.seq = j.seq;
            hd->meta.flags = j.is_write ? 1U : 0U;
            hd->meta.addr = j.addr;
            hd->meta.len = j.len;
            hd->meta.result = 0;
            std::printf("[nand] IRQ: %s done %lluus, irq->submit %lluus\n",
                        op,
                        static_cast<unsigned long long>((t_done - t0) / 1000ULL),
                        static_cast<unsigned long long>((t_irq - t_done) / 1000ULL));
            EventQos qos{false, false};
            rt->coordinator().submit_from_task(owner_target, &hd->event, qos);
        }
    }

    static uint64_t now_ns()
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL
             + static_cast<uint64_t>(ts.tv_nsec);
    }
};

// ---------------------------------------------------------------------------
// Owner AO: the single home of the NAND + its data.
// ---------------------------------------------------------------------------
struct OwnerCtx {
    NandWorker* worker{nullptr};
    Rt*         rt{nullptr};
    PoolT*      pool{nullptr};

    // Private data home — the simulated flash contents.
    std::array<uint8_t, kFlashSize> storage{};

    // Subscribers fanned out on commit.
    std::array<TargetId, kSubscriberCount> subscribers{};
    uint8_t subscriber_count{0U};

    // Single-flight request in progress.
    TargetId pending_reply_to{};
    uint32_t pending_addr{0U};
    uint16_t pending_len{0U};
    bool     pending_is_write{false};
};

enum OwnerState : int8_t { kOwnerRoot = 0, kOwnerIdle = 1, kOwnerBusy = 2 };

void ownerIdleEntry(OwnerCtx&) {}
void ownerBusyEntry(OwnerCtx&) {}

void ownerWriteReq(OwnerCtx& ctx, const Event& evt)
{
    const Layout& w = *reinterpret_cast<const Layout*>(&evt);
    NandWorker::Job j{};
    j.valid = true;
    j.is_write = true;
    j.addr = w.meta.addr;
    j.len = w.meta.len;
    j.seq = ctx.worker->seq_counter++;
    // Handoff: copy caller payload into the shared DMA buffer.
    const Payload* src = reinterpret_cast<const Payload*>(&w.payload[0]);
    std::memcpy(ctx.worker->dma_buf.data(), src->bytes.data(), j.len);
    ctx.pending_reply_to = w.meta.reply_to;
    ctx.pending_addr = j.addr;
    ctx.pending_len = j.len;
    ctx.pending_is_write = true;
    if (!ctx.worker->submit(j)) {
        std::printf("[owner] worker busy, write dropped\n");
    }
}

void ownerReadReq(OwnerCtx& ctx, const Event& evt)
{
    const Layout& r = *reinterpret_cast<const Layout*>(&evt);
    NandWorker::Job j{};
    j.valid = true;
    j.is_write = false;
    j.addr = r.meta.addr;
    j.len = r.meta.len;
    j.seq = ctx.worker->seq_counter++;
    ctx.pending_reply_to = r.meta.reply_to;
    ctx.pending_addr = j.addr;
    ctx.pending_len = j.len;
    ctx.pending_is_write = false;
    if (!ctx.worker->submit(j)) {
        std::printf("[owner] worker busy, read dropped\n");
    }
}

void ownerHwDone(OwnerCtx& ctx, const Event& evt)
{
    const Layout& hd = *reinterpret_cast<const Layout*>(&evt);
    std::printf("[owner] hw done seq=%u result=%d\n", hd.meta.seq, hd.meta.result);

    if (ctx.pending_is_write) {
        // Commit the DMA buffer into flash.
        std::memcpy(ctx.storage.data() + ctx.pending_addr,
                    ctx.worker->dma_buf.data(), ctx.pending_len);
        std::printf("[owner] write committed addr=0x%x len=%u\n",
                    ctx.pending_addr, ctx.pending_len);

        // Fan out a data-updated notice to every subscriber. A tight pool may
        // drop some notices — that is the intended back-pressure demo.
        for (uint8_t i = 0; i < ctx.subscriber_count; ++i) {
            Layout* n = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(
                static_cast<uint16_t>(Sig::kDataUpdated));
            if (nullptr == n) {
                std::printf("[owner] pool exhausted, dropped update notice\n");
                continue;
            }
            EventQos qos{false, false};
            ctx.rt->coordinator().submit_from_task(ctx.subscribers[i], &n->event, qos);
        }
    } else {
        // Respond to the requester with an inlined snapshot read straight from
        // the owner's private flash home. (The worker only modeled the read
        // latency; the data itself never leaves the owner AO, which is the
        // point of the owner-AO pattern — no shared read buffer.)
        Layout* resp = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(Sig::kDataReady));
        if (nullptr == resp) {
            std::printf("[owner] pool exhausted, dropped response\n");
            return;
        }
        resp->meta.reply_to = ctx.pending_reply_to;
        resp->meta.addr = ctx.pending_addr;
        resp->meta.len = ctx.pending_len;
        Payload* p = reinterpret_cast<Payload*>(&resp->payload[0]);
        std::memcpy(p->bytes.data(), ctx.storage.data() + ctx.pending_addr,
                    ctx.pending_len);
        EventQos qos{false, false};
        ctx.rt->coordinator().submit_from_task(ctx.pending_reply_to, &resp->event, qos);
    }
}

inline const StateDef<OwnerCtx> kOwnerStates[] = {
    { -1, nullptr, nullptr, "Root" },
    { kOwnerRoot, ownerIdleEntry, nullptr, "Idle" },
    { kOwnerRoot, ownerBusyEntry, nullptr, "Busy" },
};

inline const TransitionDef<OwnerCtx> kOwnerTransitions[] = {
    { kOwnerIdle, static_cast<uint16_t>(Sig::kWriteReq), kOwnerBusy,
      TransitionKind::External, nullptr, ownerWriteReq },
    { kOwnerIdle, static_cast<uint16_t>(Sig::kReadReq), kOwnerBusy,
      TransitionKind::External, nullptr, ownerReadReq },
    { kOwnerBusy, static_cast<uint16_t>(Sig::kHwDone), kOwnerIdle,
      TransitionKind::External, nullptr, ownerHwDone },
};

// ---------------------------------------------------------------------------
// Caller AOs: one writer (issues writes), one reader (issues queries). Both
// also subscribe to data-updated notices.
// ---------------------------------------------------------------------------
struct CallerCtx {
    const char* name{""};
    TargetId    self{};
    TargetId    owner{};
    Rt*         rt{nullptr};
    PoolT*      pool{nullptr};
    uint32_t    writes_done{0U};
    uint32_t    reads_done{0U};
    uint32_t    updates_received{0U};
};

void callerWriteTrigger(CallerCtx& ctx, const Event&)
{
    Layout* w = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(
        static_cast<uint16_t>(Sig::kWriteReq));
    if (nullptr == w) {
        std::printf("[%s] pool exhausted, write not issued\n", ctx.name);
        return;
    }
    w->meta.addr = 0x40U;
    w->meta.len = 16U;
    w->meta.flags = 1U;   // is_write
    Payload* p = reinterpret_cast<Payload*>(&w->payload[0]);
    for (uint16_t i = 0; i < w->meta.len; ++i) {
        p->bytes[i] = static_cast<uint8_t>(0xB0U + (i & 0x0FU));
    }
    ++ctx.writes_done;
    std::printf("[%s] -> write req addr=0x%x len=%u\n", ctx.name, w->meta.addr, w->meta.len);
    EventQos qos{false, false};
    ctx.rt->coordinator().submit_from_task(ctx.owner, &w->event, qos);
}

void callerReadTrigger(CallerCtx& ctx, const Event&)
{
    Layout* r = ctx.pool->alloc_typed<Layout, Payload, kPayloadAlign>(
        static_cast<uint16_t>(Sig::kReadReq));
    if (nullptr == r) {
        std::printf("[%s] pool exhausted, query not issued\n", ctx.name);
        return;
    }
    r->meta.addr = 0x40U;
    r->meta.len = 16U;
    r->meta.flags = 0U;   // read
    r->meta.reply_to = ctx.self;   // route kDataReady back to this caller
    std::printf("[%s] -> read req addr=0x%x len=%u\n", ctx.name, r->meta.addr, r->meta.len);
    EventQos qos{false, false};
    ctx.rt->coordinator().submit_from_task(ctx.owner, &r->event, qos);
}

void callerOnUpdate(CallerCtx& ctx, const Event&)
{
    ++ctx.updates_received;
    std::printf("[%s] <- data-updated (total=%u)\n", ctx.name, ctx.updates_received);
}

void callerOnReady(CallerCtx& ctx, const Event& evt)
{
    const Layout& d = *reinterpret_cast<const Layout*>(&evt);
    const Payload* p = reinterpret_cast<const Payload*>(&d.payload[0]);
    ++ctx.reads_done;
    std::printf("[%s] <- data-ready addr=0x%x len=%u first=0x%02x\n",
                ctx.name, d.meta.addr, d.meta.len,
                static_cast<unsigned>(p->bytes[0]));
}

// Both caller AOs share the same static tables; each owns its own ctx + HSM.
inline const StateDef<CallerCtx> kCallerStates[] = {
    { -1, nullptr, nullptr, "Root" },
    { 0, nullptr, nullptr, "Active" },
};

inline const TransitionDef<CallerCtx> kCallerTransitions[] = {
    { 1, static_cast<uint16_t>(Sig::kTriggerWrite), 1, TransitionKind::Internal, nullptr, callerWriteTrigger },
    { 1, static_cast<uint16_t>(Sig::kTriggerRead),  1, TransitionKind::Internal, nullptr, callerReadTrigger },
    { 1, static_cast<uint16_t>(Sig::kDataUpdated),  1, TransitionKind::Internal, nullptr, callerOnUpdate },
    { 1, static_cast<uint16_t>(Sig::kDataReady),    1, TransitionKind::Internal, nullptr, callerOnReady },
};

// ---------------------------------------------------------------------------
// Traits. Unique logical priorities per AO (registry enforces uniqueness).
// ---------------------------------------------------------------------------
template <uint8_t Prio>
struct AoTrait {
    static LogicalPrio logical_prio() { return static_cast<LogicalPrio>(Prio); }
    static PriorityClass priority_class() { return PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};

using OwnerAo  = coact::Ao<OwnerCtx, Hsm<OwnerCtx>, AoTrait<30>>;
using WriterAo = coact::Ao<CallerCtx, Hsm<CallerCtx>, AoTrait<25>>;
using ReaderAo = coact::Ao<CallerCtx, Hsm<CallerCtx>, AoTrait<20>>;

}  // namespace flash_demo

// ---------------------------------------------------------------------------
// Demo driver.
// ---------------------------------------------------------------------------
int main()
{
    using namespace flash_demo;

    coact::pal::Posix pal;

    alignas(kPayloadAlign) std::array<uint8_t,
        sizeof(Layout) * kPoolBlocks + kPayloadAlign> storage{};
    PoolT pool;
    pool.init(storage.data(), storage.size(), coact::make_critical_section(pal));

    Rt rt(pal);

    OwnerAo owner(kOwnerStates, static_cast<uint16_t>(std::size(kOwnerStates)),
                  kOwnerTransitions, static_cast<uint16_t>(std::size(kOwnerTransitions)),
                  kOwnerIdle, 2U);
    WriterAo writer(kCallerStates, static_cast<uint16_t>(std::size(kCallerStates)),
                    kCallerTransitions, static_cast<uint16_t>(std::size(kCallerTransitions)),
                    1, 2U);
    ReaderAo reader(kCallerStates, static_cast<uint16_t>(std::size(kCallerStates)),
                    kCallerTransitions, static_cast<uint16_t>(std::size(kCallerTransitions)),
                    1, 2U);

    rt.bind(&owner);
    rt.bind(&writer);
    rt.bind(&reader);

    NandWorker worker;
    owner.context().worker = &worker;
    owner.context().rt = &rt;
    owner.context().pool = &pool;
    owner.context().subscribers[0] = TargetId(2U);
    owner.context().subscribers[1] = TargetId(3U);
    owner.context().subscriber_count = 2U;

    writer.context().name = "writer";
    writer.context().self = TargetId(2U);
    writer.context().owner = TargetId(1U);
    writer.context().rt = &rt;
    writer.context().pool = &pool;

    reader.context().name = "reader";
    reader.context().self = TargetId(3U);
    reader.context().owner = TargetId(1U);
    reader.context().rt = &rt;
    reader.context().pool = &pool;

    Event init_e{0U, 0U, 0U};
    owner.init(init_e);
    writer.init(init_e);
    reader.init(init_e);

    rt.initialize();
    rt.start();
    worker.start(&pool, &rt, TargetId(1U));

    std::printf("=== flash_proxy_demo: owner AO + DMA worker + IRQ timing ===\n");

    EventQos qos{false, false};
    auto submit_trigger = [&](TargetId t, Sig s) {
        Layout* n = pool.alloc_typed<Layout, Payload, kPayloadAlign>(
            static_cast<uint16_t>(s));
        if (nullptr != n) {
            rt.coordinator().submit_from_task(t, &n->event, qos);
        }
    };

    usleep(5000);
    submit_trigger(TargetId(2U), Sig::kTriggerWrite);   // writer issues a write
    usleep(60000);
    submit_trigger(TargetId(3U), Sig::kTriggerRead);    // reader issues a query
    usleep(20000);
    submit_trigger(TargetId(2U), Sig::kTriggerWrite);   // second write, more contention
    usleep(60000);
    submit_trigger(TargetId(3U), Sig::kTriggerRead);    // read back the 2nd write
    usleep(20000);

    coact::AoBase* aos[] = { &owner, &writer, &reader };
    for (int w = 0; w < 400; ++w) {
        bool drained = true;
        for (coact::AoBase* a : aos) {
            if (0U != a->pending().load()) {
                drained = false;
            }
        }
        if (drained) {
            break;
        }
        usleep(5000);
    }

    worker.stop();
    rt.stop();

    std::printf("\n=== final state ===\n");
    std::printf("writer: writes=%u updates=%u\n",
                writer.context().writes_done, writer.context().updates_received);
    std::printf("reader: reads=%u updates=%u\n",
                reader.context().reads_done, reader.context().updates_received);
    std::printf("flash[0x40]=0x%02X  (expect 0xB0 after 1st write, 0xB0 after 2nd)\n",
                static_cast<unsigned>(owner.context().storage[0x40U]));
    std::printf("pool.used=%u (expect 0 = full reclaim), hwm=%u\n",
                static_cast<unsigned>(pool.used()),
                static_cast<unsigned>(pool.high_watermark()));

    return 0;
}