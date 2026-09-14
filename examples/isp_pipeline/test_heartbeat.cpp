// coact isp_pipeline heartbeat skeleton self-test (host-only).
// Standalone binary: exits non-zero on failure. Deterministic — the clock is
// injected via hb::SetClock() so every timing assertion is exact.
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "isp_pipeline/heartbeat.hpp"

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        ++g_passed;
    }
    else {
        ++g_failed;
        std::printf("  FAIL: %s\n", what);
    }
}

/* Controllable clock: the skeleton's timing semantics must be verifiable
   without sleeping. */
uint64_t g_now_us = 1000U;
uint64_t fake_now() { return g_now_us; }

int g_timeouts = 0;
const char* g_last_timeout_name = nullptr;

void on_timeout(const char* name, void*)
{
    ++g_timeouts;
    g_last_timeout_name = name;
}

void reset_timeout_probe()
{
    g_timeouts = 0;
    g_last_timeout_name = nullptr;
}

// ---------------------------------------------------------------------------
// Beat: the timestamp primitive.
// ---------------------------------------------------------------------------
void test_beat_records_the_clock()
{
    hb::Beat b;
    check(0U == b.LastUs(), "beat starts unstamped");

    g_now_us = 4200U;
    b.Hit();
    check(4200U == b.LastUs(), "beat records the clock on Hit");
}

// ---------------------------------------------------------------------------
// BeatLoop: the beat advice is owned by the skeleton.
// ---------------------------------------------------------------------------
void test_beatloop_beats_per_iteration()
{
    hb::Beat b;
    int iterations = 0;
    hb::BeatLoop(&b,
                 [&] { return iterations < 5; },
                 [&] { ++iterations; g_now_us += 10U; });

    check(5 == iterations, "beatloop runs the body per iteration");
    check(0U != b.LastUs(), "beatloop beats while the predicate holds");
}

void test_beatloop_false_predicate_never_beats()
{
    hb::Beat b;
    bool ran = false;
    hb::BeatLoop(&b, [] { return false; }, [&] { ran = true; });

    check(!ran, "false predicate never runs the body");
    check(0U == b.LastUs(), "false predicate does not beat early");
}

void test_beatloop_null_beat_is_safe()
{
    int n = 0;
    hb::BeatLoop(nullptr, [&] { return n < 3; }, [&] { ++n; });

    check(3 == n, "null beat pointer still runs the body");
}

void test_beatloop_bool_body_can_stop_early()
{
    hb::Beat b;
    int n = 0;
    hb::BeatLoop(&b,
                 [] { return true; },
                 [&]() -> bool { return ++n < 4; });

    check(4 == n, "bool body terminates the loop before the predicate does");
}

// ---------------------------------------------------------------------------
// Slots: per-entity timeout detection.
// ---------------------------------------------------------------------------
void test_register_stamps_the_clock()
{
    hb::Slots slots;
    g_now_us = 7000U;
    hb::Beat* b = slots.Register({"fresh", 100U});

    check(nullptr != b, "register returns a beat handle");
    check(nullptr != b && 7000U == b->LastUs(),
          "register stamps the current clock");

    g_now_us = 7050U;
    check(0U == slots.Check(), "a just-registered slot has not timed out");
}

void test_slots_reports_a_stalled_slot()
{
    hb::Slots slots;
    reset_timeout_probe();
    slots.SetOnTimeout(&on_timeout, nullptr);

    g_now_us = 1000U;
    hb::Beat* b = slots.Register({"worker_a", 500U});
    check(nullptr != b, "register before stalling");

    g_now_us = 1200U;
    check(0U == slots.Check(), "a slot inside its timeout is not reported");

    g_now_us = 1600U;
    check(1U == slots.Check(), "a stalled slot is reported");
    check(1 == g_timeouts, "the timeout callback fires once");
    check(nullptr != g_last_timeout_name &&
              0 == std::strcmp("worker_a", g_last_timeout_name),
          "the callback carries the slot name");
}

void test_timeout_fires_once_until_recovered()
{
    hb::Slots slots;
    reset_timeout_probe();
    slots.SetOnTimeout(&on_timeout, nullptr);

    g_now_us = 1000U;
    hb::Beat* b = slots.Register({"worker_b", 500U});

    g_now_us = 1600U;
    (void)slots.Check();
    check(1 == g_timeouts, "first detection reports");

    g_now_us = 1700U;
    (void)slots.Check();
    check(1 == g_timeouts, "a still-stalled slot does not re-report");

    if (nullptr != b) {
        b->Hit();
    }
    g_now_us = 1750U;
    check(0U == slots.Check(), "a beaten slot recovers");
}

void test_unregistered_slot_is_not_checked()
{
    hb::Slots slots;
    reset_timeout_probe();
    slots.SetOnTimeout(&on_timeout, nullptr);

    g_now_us = 1000U;
    hb::Beat* b = slots.Register({"worker_c", 500U});
    slots.Unregister(b);

    g_now_us = 100000U;
    check(0U == slots.Check(), "an unregistered slot is not checked");
    check(0 == g_timeouts, "an unregistered slot never fires the callback");
}

void test_register_beyond_capacity_returns_null()
{
    hb::Slots slots;
    g_now_us = 1000U;

    bool all_ok = true;
    for (uint8_t i = 0U; i < hb::Slots::kCapacity; ++i) {
        if (nullptr == slots.Register({"w", 100U})) {
            all_ok = false;
        }
    }
    check(all_ok, "register succeeds up to capacity");
    check(nullptr == slots.Register({"over", 100U}),
          "register beyond capacity returns null");
}

void test_foreach_slot_visits_only_registered()
{
    hb::Slots slots;
    g_now_us = 100U;
    (void)slots.Register({"a", 100U});
    (void)slots.Register({"b", 100U});

    int visited = 0;
    slots.ForEachSlot([&](const char*, uint64_t, bool) { ++visited; });
    check(2 == visited, "foreach visits only registered slots");
}

// ---------------------------------------------------------------------------
// ScanLoop: the scan advice is owned by the skeleton.
// ---------------------------------------------------------------------------
void test_scanloop_scans_each_iteration()
{
    hb::Slots slots;
    reset_timeout_probe();
    slots.SetOnTimeout(&on_timeout, nullptr);

    hb::Slots* previous = hb::g_slots;
    hb::g_slots = &slots;

    g_now_us = 5000U;
    (void)slots.Register({"idle", 100U});

    int ticks = 0;
    hb::ScanLoop([&] { return ticks < 3; },
                 [&] { ++ticks; g_now_us += 200U; });

    hb::g_slots = previous;

    check(3 == ticks, "scanloop runs the body per iteration");
    check(1 == g_timeouts, "scanloop scans on every iteration");
    check(nullptr == previous, "the global slot table is restored");
}

}  // namespace

int main()
{
    hb::SetClock(&fake_now);

    std::printf("heartbeat skeleton self-test\n");
    test_beat_records_the_clock();
    test_beatloop_beats_per_iteration();
    test_beatloop_false_predicate_never_beats();
    test_beatloop_null_beat_is_safe();
    test_beatloop_bool_body_can_stop_early();
    test_register_stamps_the_clock();
    test_slots_reports_a_stalled_slot();
    test_timeout_fires_once_until_recovered();
    test_unregistered_slot_is_not_checked();
    test_register_beyond_capacity_returns_null();
    test_foreach_slot_visits_only_registered();
    test_scanloop_scans_each_iteration();

    std::printf("RESULT: %s (passed=%d failed=%d)\n",
                (0 == g_failed) ? "ALL PASS" : "FAILURES", g_passed, g_failed);
    return (0 == g_failed) ? 0 : 1;
}
