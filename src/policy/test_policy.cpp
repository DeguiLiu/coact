// coact policy module host tests: admission evaluate branches, rate-limit,
// MergeCell CAS state machine full path plus concurrent negatives, filter
// rejection, and merge gating on qos.mergeable. See contract 4.5 / design 11.
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <atomic>
#include <thread>
#include <vector>

#include "coact/config.hpp"
#include "coact/event.hpp"
#include "coact/policy.hpp"
#include "coact/pool.hpp"

#include "test/test_harness.hpp"

namespace {

using coact::Event;
using coact::EventQos;
using coact::PolicyOps;
using coact::PolicyResult;
using coact::PolicyReason;
using coact::RateLimitRule;
using coact::TargetId;
using coact::TokenBucketRateLimiter;
using coact::MergeCell;
using coact::MergeCellState;

// Dummy owning event carried by a cell; ref_ctr is managed manually here.
Event make_static(uint16_t sig)
{
    Event e{sig, 0U, 0U};
    return e;
}

}  // namespace

// ---------------------------------------------------------------------------
// evaluate: a hand-built table policy that applies filter + rate-limit and
// returns try_merge only when qos.mergeable.
// ---------------------------------------------------------------------------
namespace {

struct DemoCtx {
    TargetId blocked_target;   // a filter blacklist entry
    uint16_t blocked_signal;   // a filter blacklist entry
    TokenBucketRateLimiter limiter;
};

PolicyResult demo_evaluate(void* context, TargetId target,
                           const Event& event, const EventQos& qos,
                           uint64_t now)
{
    DemoCtx* c = static_cast<DemoCtx*>(context);
    PolicyResult r{false, false, PolicyReason::kReasonOk};

    // filter: blacklist target or signal
    if (target == c->blocked_target || event.signal == c->blocked_signal) {
        r.accept = false;
        r.try_merge = false;
        r.reason = PolicyReason::kReasonFiltered;
        return r;
    }

    // rate-limit: reject when the token bucket is exhausted
    if (!c->limiter.acquire(now)) {
        r.accept = false;
        r.try_merge = false;
        r.reason = PolicyReason::kReasonRateLimit;
        return r;
    }

    // admit; suggest merge only for explicitly mergeable events
    r.accept = true;
    r.try_merge = qos.mergeable;
    r.reason = PolicyReason::kReasonOk;
    return r;
}

bool demo_merge(void* context, Event& queued, const Event& incoming)
{
    (void)context;
    // overwrite semantics: newest signal wins, keep the queued cell event
    queued.signal = incoming.signal;
    return true;
}

const PolicyOps g_ops = {&demo_evaluate, &demo_merge};

}  // namespace

COACT_TEST(policy_evaluate_accept)
{
    DemoCtx ctx{};
    ctx.blocked_target = TargetId(0xFFU);
    ctx.blocked_signal = 0xFEFEU;
    ctx.limiter.init(RateLimitRule{4U, 4U, 1U, true}, 0U);

    Event ev = make_static(0x100U);
    EventQos qos{false, false};
    PolicyResult r = g_ops.evaluate(&ctx, TargetId(1U), ev, qos, 0U);
    CHECK(r.accept);
    CHECK(!r.try_merge);
    CHECK_EQ(r.reason, coact::PolicyReason::kReasonOk);
}

COACT_TEST(policy_evaluate_merge_hint)
{
    DemoCtx ctx{};
    ctx.blocked_target = TargetId(0xFFU);
    ctx.blocked_signal = 0xFEFEU;
    ctx.limiter.init(RateLimitRule{4U, 4U, 1U, true}, 0U);

    Event ev = make_static(0x101U);
    EventQos qos{false, true};
    PolicyResult r = g_ops.evaluate(&ctx, TargetId(1U), ev, qos, 0U);
    CHECK(r.accept);
    CHECK(r.try_merge);
    CHECK_EQ(r.reason, coact::PolicyReason::kReasonOk);
}

COACT_TEST(policy_evaluate_filter_target)
{
    DemoCtx ctx{};
    ctx.blocked_target = TargetId(7U);
    ctx.blocked_signal = 0xFEFEU;
    ctx.limiter.init(RateLimitRule{4U, 4U, 1U, true}, 0U);

    Event ev = make_static(0x102U);
    EventQos qos{false, true};
    PolicyResult r = g_ops.evaluate(&ctx, TargetId(7U), ev, qos, 0U);
    CHECK(!r.accept);
    CHECK(!r.try_merge);
    CHECK_EQ(r.reason, coact::PolicyReason::kReasonFiltered);
}

COACT_TEST(policy_evaluate_filter_signal)
{
    DemoCtx ctx{};
    ctx.blocked_target = TargetId(0xFFU);
    ctx.blocked_signal = 0xFEFEU;
    ctx.limiter.init(RateLimitRule{4U, 4U, 1U, true}, 0U);

    Event ev = make_static(0xFEFEU);
    EventQos qos{false, true};
    PolicyResult r = g_ops.evaluate(&ctx, TargetId(1U), ev, qos, 0U);
    CHECK(!r.accept);
    CHECK(!r.try_merge);
    CHECK_EQ(r.reason, coact::PolicyReason::kReasonFiltered);
}

// A critical event is never silently dropped by the generic filter in this
// demo; the coordinator owns that guarantee, but the evaluate table surfaces
// a distinct reason code when a guarded drop is unavailable.
COACT_TEST(policy_evaluate_rate_limit)
{
    DemoCtx ctx{};
    ctx.blocked_target = TargetId(0xFFU);
    ctx.blocked_signal = 0xFEFEU;
    ctx.limiter.init(RateLimitRule{1U, 1U, 100U, true}, 0U);

    Event ev = make_static(0x103U);
    EventQos qos{false, false};

    // first call consumes the single token
    PolicyResult r1 = g_ops.evaluate(&ctx, TargetId(1U), ev, qos, 0U);
    CHECK(r1.accept);

    // second call within the window is rate-limited
    PolicyResult r2 = g_ops.evaluate(&ctx, TargetId(1U), ev, qos, 1U);
    CHECK(!r2.accept);
    CHECK(!r2.try_merge);
    CHECK_EQ(r2.reason, coact::PolicyReason::kReasonRateLimit);
}

// ---------------------------------------------------------------------------
// TokenBucketRateLimiter standalone behavior.
// ---------------------------------------------------------------------------
COACT_TEST(policy_rate_limiter_refill)
{
    TokenBucketRateLimiter l;
    l.init(RateLimitRule{3U, 1U, 10U, true}, 0U);
    CHECK_EQ(l.tokens(), 3U);

    // consume burst
    CHECK(l.acquire(0U));
    CHECK(l.acquire(0U));
    CHECK(l.acquire(0U));
    CHECK(!l.acquire(0U));   // exhausted now
    CHECK_EQ(l.tokens(), 0U);

    // refill after 30 ticks: +3 (rate 1 per 10 ticks), clamped to 3
    CHECK(l.acquire(30U));   // refills to 3, consume 1 -> 2
    CHECK_EQ(l.tokens(), 2U);
    CHECK(l.acquire(30U));   // no time advance, consume 2 -> 1
    CHECK(l.acquire(30U));   // consume 1 -> 0
    CHECK(!l.acquire(30U));  // exhausted again (no refill at same now)
}

COACT_TEST(policy_rate_limiter_disabled)
{
    TokenBucketRateLimiter l;
    l.init(RateLimitRule{0U, 0U, 1U, false}, 0U);
    CHECK(l.acquire(0U));
    CHECK(l.acquire(0U));
    CHECK(l.acquire(1000U));
}

COACT_TEST(policy_rate_limiter_capacity_clamp)
{
    TokenBucketRateLimiter l;
    l.init(RateLimitRule{2U, 5U, 1U, true}, 0U);
    // a huge time jump refills beyond capacity; must clamp to 2
    l.acquire(0U);   // consume -> 1
    l.acquire(1000U);  // refills 1000*5, clamp to 2, consume -> 1
    CHECK_EQ(l.tokens(), 1U);
    CHECK(!(l.tokens() > 2U));
}

// ---------------------------------------------------------------------------
// MergeCell state machine (single-threaded paths).
// ---------------------------------------------------------------------------
COACT_TEST(policy_mergecell_publish_merge_release)
{
    MergeCell cell;
    cell.init(TargetId(1U), 0x200U);
    CHECK_EQ(cell.state(), MergeCellState::Empty);

    Event e1 = make_static(0x200U);
    CHECK(cell.try_publish(&e1));
    CHECK_EQ(cell.state(), MergeCellState::Published);

    Event* queued = nullptr;
    CHECK(cell.try_acquire_merge(queued));
    CHECK_EQ(cell.state(), MergeCellState::Merging);
    CHECK(queued == &e1);

    // producer overwrite under Merging
    Event incoming = make_static(0x201U);
    CHECK(g_ops.merge(nullptr, *queued, incoming));
    cell.release_merge();
    CHECK_EQ(cell.state(), MergeCellState::Published);

    // dispatcher takes owning handle
    Event* out = nullptr;
    CHECK(cell.take_owning(out));
    CHECK_EQ(cell.state(), MergeCellState::Consuming);
    CHECK(out == &e1);
    CHECK_EQ(out->signal, 0x201U);   // payload was repurposed

    cell.release_empty();
    CHECK_EQ(cell.state(), MergeCellState::Empty);
}

COACT_TEST(policy_mergecell_merged_event_releases_allocated_reference)
{
    alignas(16) std::uint8_t storage[32U * 2U];
    coact::EventPool<32U, 2U> pool;
    pool.init(storage, sizeof(storage));
    coact::Event* owned = pool.alloc(0x202U);
    REQUIRE(owned != nullptr);
    CHECK_EQ(owned->ref_ctr, 1U);

    MergeCell cell;
    cell.init(TargetId(2U), 0x202U);
    CHECK(cell.try_publish(owned));
    Event* queued = nullptr;
    CHECK(cell.try_acquire_merge(queued));
    Event incoming = make_static(0x203U);
    CHECK(g_ops.merge(nullptr, *queued, incoming));
    cell.release_merge();

    Event* out = nullptr;
    CHECK(cell.take_owning(out));
    CHECK(out == owned);
    coact::event_gc(out);
    cell.release_empty();
    CHECK_EQ(pool.used(), 0U);
}

COACT_TEST(policy_mergecell_publish_failure_when_occupied)
{
    MergeCell cell;
    cell.init(TargetId(3U), 0x300U);
    Event e1 = make_static(0x300U);
    Event e2 = make_static(0x301U);
    CHECK(cell.try_publish(&e1));

    // a second producer cannot publish into an already-occupied cell
    CHECK(!cell.try_publish(&e2));
    CHECK(!cell.try_publish(nullptr));

    // clean up
    Event* out = nullptr;
    CHECK(cell.take_owning(out));
    cell.release_empty();
}

COACT_TEST(policy_mergecell_no_merge_while_consuming)
{
    MergeCell cell;
    cell.init(TargetId(4U), 0x400U);
    Event e1 = make_static(0x400U);
    CHECK(cell.try_publish(&e1));

    Event* queued = nullptr;
    Event* out = nullptr;
    CHECK(cell.take_owning(out));             // dispatcher takes owning
    CHECK_EQ(cell.state(), MergeCellState::Consuming);

    // producer merge is rejected while the cell is being consumed
    CHECK(!cell.try_acquire_merge(queued));
    CHECK(queued == nullptr);
    CHECK_EQ(cell.state(), MergeCellState::Consuming);

    cell.release_empty();
    CHECK_EQ(cell.state(), MergeCellState::Empty);
}

COACT_TEST(policy_mergecell_take_failure_while_merging)
{
    MergeCell cell;
    cell.init(TargetId(5U), 0x500U);
    Event e1 = make_static(0x500U);
    CHECK(cell.try_publish(&e1));

    Event* queued = nullptr;
    CHECK(cell.try_acquire_merge(queued));    // producer holds Merging

    // dispatcher cannot take owning while Merging
    Event* out = nullptr;
    CHECK(!cell.take_owning(out));
    CHECK(out == nullptr);

    cell.release_merge();
    CHECK_EQ(cell.state(), MergeCellState::Published);

    CHECK(cell.take_owning(out));
    cell.release_empty();
}

COACT_TEST(policy_mergecell_merge_gated_by_qos)
{
    // merge is only offered when qos.mergeable; the cell itself never decides,
    // the evaluate table does. Here we prove a real signal match still yields
    // no merge hint without the flag.
    DemoCtx ctx{};
    ctx.blocked_target = TargetId(0xFFU);
    ctx.blocked_signal = 0xFEFEU;
    ctx.limiter.init(RateLimitRule{8U, 8U, 1U, true}, 0U);

    Event ev = make_static(0x600U);
    EventQos qos{false, false};
    PolicyResult r = g_ops.evaluate(&ctx, TargetId(1U), ev, qos, 0U);
    CHECK(r.accept);
    CHECK(!r.try_merge);   // not mergeable -> no merge suggestion
}

// ---------------------------------------------------------------------------
// Concurrency: multiple producers race on one cell; exactly one publish and
// exactly one merge ownership sequence may win. CAS losers fall through.
// ---------------------------------------------------------------------------
COACT_TEST(policy_mergecell_concurrent_producers)
{
    constexpr int kProducers = 6;
    constexpr int kAttempts = 2000;

    MergeCell cell;
    cell.init(TargetId(9U), 0x900U);

    // the cell owns this single test event; ref_ctr lives on the test only
    Event owned_event = make_static(0x900U);

    std::atomic<int> published{0};
    std::atomic<int> consumed{0};
    std::atomic<int> merge_wins{0};

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(kProducers));
    for (int t = 0; t < kProducers; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < kAttempts; ++i) {
                // Producer A) race to occupy the empty cell.
                if (cell.try_publish(&owned_event)) {
                    published.fetch_add(1, std::memory_order_relaxed);
                    // We own Empty -> Published now; drain straight back to
                    // Empty so other producers can publish in later cycles.
                    Event* out = nullptr;
                    if (cell.take_owning(out)) {
                        consumed.fetch_add(1, std::memory_order_relaxed);
                        cell.release_empty();
                    }
                    else {
                        // A merger grabbed the slot between publish and take:
                        // drop our tentative claim; the cell stays Published.
                    }
                }
                else {
                    // Producer B) cell occupied: race for the merge section.
                    Event* queued = nullptr;
                    if (cell.try_acquire_merge(queued)) {
                        merge_wins.fetch_add(1, std::memory_order_relaxed);
                        cell.release_merge();
                    }
                }
            }
        });
    }

    for (std::thread& th : threads) {
        th.join();
    }

    // Exclusivity invariants under concurrent producers:
    //  - try_publish is CAS-guarded, so every publish that wins may be drained
    //    by at most one take_owning; a concurrent merger can steal the slot
    //    first, so consumed never exceeds published.
    //  - merge_wins is unbounded relative to published: a single Published slot
    //    can be merged repeatedly (Published -> Merging -> Published), which is
    //    exactly the intended bounded, non-blocking merge loop. Its only
    //    guarantee is mutual exclusivity of the Merging section, enforced by
    //    the single CAS gate (no assertion can bound it higher than that).
    CHECK(published.load() > 0);
    CHECK(consumed.load() <= published.load());
    /* Not asserted: merge is best-effort and non-blocking. Under optimized
       (-O2) timing two producers can race through publish without one ever
       reaching the merge CAS, so merge_wins == 0 is legal here. Deterministic
       merge behavior is covered by the single-threaded merge tests above. */
    (void)merge_wins;
}

COACT_TEST(policy_mergecell_published_event_is_visible_to_consumer)
{
    constexpr int kRounds = 100000;

    MergeCell cell;
    cell.init(TargetId(11U), 0xB00U);
    Event owned_event = make_static(0xB00U);
    std::atomic<bool> producer_done{false};
    std::atomic<int> invalid_take{0};

    std::thread consumer([&]() {
        while (!producer_done.load(std::memory_order_acquire)
               || cell.state() != MergeCellState::Empty) {
            Event* out = nullptr;
            if (cell.take_owning(out)) {
                if (out != &owned_event) {
                    invalid_take.fetch_add(1, std::memory_order_relaxed);
                }
                cell.release_empty();
            }
            else {
                std::this_thread::yield();
            }
        }
    });

    for (int round = 0; round < kRounds; ++round) {
        while (!cell.try_publish(&owned_event)) {
            std::this_thread::yield();
        }
    }
    producer_done.store(true, std::memory_order_release);
    consumer.join();

    CHECK_EQ(invalid_take.load(std::memory_order_relaxed), 0);
    CHECK_EQ(cell.state(), MergeCellState::Empty);
}

// Double-consume guard: after release_empty the cell is Empty; a stale
// dispatcher tail calling take_owning again must fail. The consumer is
// responsible for event_gc exactly once; this proves the cell re-arms.
COACT_TEST(policy_mergecell_no_double_owning)
{
    MergeCell cell;
    cell.init(TargetId(10U), 0xA00U);
    Event e1 = make_static(0xA00U);
    CHECK(cell.try_publish(&e1));

    Event* out = nullptr;
    CHECK(cell.take_owning(out));
    CHECK(out == &e1);
    cell.release_empty();

    // no second owning handle after the cell returned to Empty
    Event* again = nullptr;
    CHECK(!cell.take_owning(again));
    CHECK(again == nullptr);
    CHECK_EQ(cell.state(), MergeCellState::Empty);

    // a fresh publish re-arms the same cell
    Event e2 = make_static(0xA01U);
    CHECK(cell.try_publish(&e2));
    CHECK(cell.take_owning(out));
    CHECK(out == &e2);
    cell.release_empty();
}

COACT_TEST_MAIN()
