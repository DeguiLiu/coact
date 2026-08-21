// coact hot-path benchmark with a built-in "poor man's" sampling profiler.
// SPDX-License-Identifier: MIT
//
// No perf/root is required on Linux: a SIGPROF interval timer (default 1 kHz)
// fires only while the process is on CPU; the handler captures the interrupted
// thread's stack with backtrace() into a ring buffer. On exit the samples are
// symbolized (dladdr + __cxa_demangle), folded root->leaf and written as a
// flame-graph input file, plus a top-N table.
//
// Workload modes isolate the two dispatch mechanisms so each hot path is
// profiled cleanly (mixing them on one AO trips a known direct/staged lease
// race — see findings report):
//   staged:  2 non-direct AOs (Normal + High) + 2 producers -> submit -> staging
//            -> Dispatcher -> Ao::dispatch -> action -> event_gc
//   direct:  1 direct-eligible AO + 1 producer -> M1 in-thread direct dispatch
//
// Usage:
//   ./bench_hotpath --mode staged|direct [--seconds N] [--hz H]
//                   [--cores N] [--tick-hz H] [--sample out.folded]
//
//   --cores N   pin the process to CPUs 0..N-1 (threads inherit affinity);
//               --cores 1 models a single-core target where producers and the
//               Dispatcher preempt each other.
//   --tick-hz H quantize the monotonic clock to H ticks/s (e.g. 100 Hz ->
//               10 ms), matching an RT-Thread tick-based PAL.
//
// Render the folded file with tools/flamegraph_svg.py.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <map>
#include <sched.h>
#include <string>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "coact/ao.hpp"
#include "coact/config.hpp"
#include "coact/coordinator.hpp"
#include "coact/dispatcher.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"
#include "coact/monitor.hpp"
#include "coact/pal_posix.hpp"
#include "coact/pool.hpp"
#include "coact/queue.hpp"
#include "coact/runtime.hpp"
#include "coact/staging.hpp"

/* ============================ abort dump ============================ */
/* std::abort() from COACT_ASSERT raises SIGABRT; dump a backtrace so the
   crashing function is visible without gdb/core (gdb perturbs timing and
   hides the race). Debugging aid only. */
extern "C" {
static void abort_dump(int)
{
    void* frames[64];
    const int n = backtrace(frames, static_cast<int>(64));
    (void)!write(2, "\n=== SIGABRT in coact bench ===\n", 31);
    backtrace_symbols_fd(frames, n, 2);
    (void)!write(2, "================================\n", 32);
    _exit(134);
}
}

/* ============================ workload ============================ */
struct BenchCtx { std::atomic<long>* count; };
static void b_noop_entry(BenchCtx&) {}
static void b_noop_exit(BenchCtx&)  {}
static void b_count(BenchCtx& c, const coact::Event&)
{
    c.count->fetch_add(1L, std::memory_order_relaxed);
}
static bool b_ok(const BenchCtx&, const coact::Event&) { return true; }

static const coact::StateDef<BenchCtx> kStates[] = {
    { -1, nullptr, nullptr },
    {  0, b_noop_entry, b_noop_exit },
};
static const coact::TransitionDef<BenchCtx> kTrans[] = {
    { 1, 1U, 1, coact::TransitionKind::Internal, b_ok, b_count },
};

struct TraitsNormal {
    static coact::LogicalPrio   logical_prio()   { return 20U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::Normal; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
struct TraitsHigh {
    static coact::LogicalPrio   logical_prio()   { return 30U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::High; }
    static bool direct_eligible() { return false; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
struct TraitsDirect {
    static coact::LogicalPrio   logical_prio()   { return 40U; }
    static coact::PriorityClass priority_class() { return coact::PriorityClass::High; }
    static bool direct_eligible() { return true; }
    static bool isr_direct_safe() { return false; }
    static constexpr uint64_t kRtcBudgetNs = 1000000ULL;
};
using BenchHsm = coact::Hsm<BenchCtx>;
using AoNormal = coact::Ao<BenchCtx, BenchHsm, TraitsNormal>;
using AoHigh   = coact::Ao<BenchCtx, BenchHsm, TraitsHigh>;
using AoDirect = coact::Ao<BenchCtx, BenchHsm, TraitsDirect>;

static constexpr uint16_t kBlk = 16U;
static constexpr uint16_t kCap = 128U;
alignas(16) static unsigned char g_storage[kBlk * (kCap + 1U)];

/* ============================ sampler ============================ */
namespace prof {

static constexpr size_t kMaxFrames = 48;
static constexpr size_t kSlots = (1U << 14);   /* 16384 samples @ 1 kHz = 16 s */

struct Sample {
    size_t n;
    void* fr[kMaxFrames];
};
static Sample g_buf[kSlots];
static std::atomic<size_t> g_next{0};
static volatile sig_atomic_t g_armed = 0;

static void sample_handler(int)
{
    if (0 == g_armed) { return; }
    const size_t idx = g_next.fetch_add(1, std::memory_order_relaxed);
    Sample& s = g_buf[idx % kSlots];
    s.n = static_cast<size_t>(backtrace(s.fr, static_cast<int>(kMaxFrames)));
}

static void start(int hz)
{
    g_next.store(0, std::memory_order_relaxed);
    g_armed = 1;
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sample_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    struct itimerval it;
    std::memset(&it, 0, sizeof(it));
    it.it_interval.tv_usec = 1000000L / hz;
    it.it_value.tv_usec = 1000000L / hz;
    setitimer(ITIMER_PROF, &it, nullptr);
}

static void stop()
{
    g_armed = 0;
    struct itimerval it;
    std::memset(&it, 0, sizeof(it));
    setitimer(ITIMER_PROF, &it, nullptr);
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigaction(SIGPROF, &sa, nullptr);
}

static std::string demangle(const char* name)
{
    if (nullptr == name) { return "<unknown>"; }
    int status = 0;
    char* d = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    std::string out = ((status == 0) && (nullptr != d)) ? std::string(d) : std::string(name);
    std::free(d);
    return out;
}

/* Drop frames that belong to the sampler / libc signal trampoline. */
static bool skip_frame(const char* sym)
{
    if (nullptr == sym) { return true; }
    const std::string s(sym);
    if (s.find("sample_handler") != std::string::npos) { return true; }
    if (s.find("backtrace") != std::string::npos) { return true; }
    if (s.find("__kernel_rt_sigreturn") != std::string::npos) { return true; }
    if (s.find("restore_rt") != std::string::npos) { return true; }
    return false;
}

static void write_folded(FILE* out, bool verbose)
{
    size_t taken = g_next.load(std::memory_order_relaxed);
    if (taken > kSlots) { taken = kSlots; }
    if (0U == taken) { return; }

    std::map<std::string, size_t> stacks;
    for (size_t i = 0; i < taken; ++i) {
        const Sample& s = g_buf[i];
        if (0U == s.n) { continue; }
        std::vector<std::string> frames;   /* root -> leaf */
        for (size_t f = s.n; f-- > 0; ) {
            Dl_info info;
            std::memset(&info, 0, sizeof(info));
            const char* sym = nullptr;
            if (dladdr(s.fr[f], &info) && (nullptr != info.dli_sname)) {
                sym = info.dli_sname;
            }
            if (skip_frame(sym)) { continue; }
            frames.push_back(demangle(sym));
        }
        if (frames.empty()) { continue; }
        std::string path;
        for (size_t f = 0; f < frames.size(); ++f) {
            if (0U != f) { path += ";"; }
            path += frames[f];
        }
        ++stacks[path];
    }

    std::vector<std::pair<size_t, std::string>> sorted;
    sorted.reserve(stacks.size());
    for (const auto& kv : stacks) { sorted.emplace_back(kv.second, kv.first); }
    std::sort(sorted.begin(), sorted.end(),
              [](const std::pair<size_t, std::string>& a,
                 const std::pair<size_t, std::string>& b) { return a.first > b.first; });

    if (verbose) {
        std::fprintf(out, "# samples=%zu\n", taken);
        const size_t top = std::min<size_t>(25, sorted.size());
        for (size_t i = 0; i < top; ++i) {
            std::fprintf(out, "%6zu  %s\n", sorted[i].first, sorted[i].second.c_str());
        }
        std::fprintf(out, "# ----- folded (root->leaf;count) -----\n");
    }
    for (const auto& kv : stacks) {
        std::fprintf(out, "%s %zu\n", kv.first.c_str(), kv.second);
    }
}

}  // namespace prof

/* ============================ runners ============================ */

// Produce into a pool/submit loop until stop is set. Waits on pool exhaustion.
template <typename CoordinatorT>
static void producer_loop(std::atomic<bool>& stop, coact::EventPool<kBlk, kCap>& pool,
                          CoordinatorT& coord,
                          coact::TargetId target, const coact::EventQos& qos)
{
    while (!stop.load(std::memory_order_relaxed)) {
        coact::Event* e = pool.alloc(1U);
        if (nullptr == e) { continue; }
        coord.submit_from_task(target, e, qos);
    }
}

int main(int argc, char** argv)
{
    std::string mode = "staged";
    double seconds = 5.0;
    int hz = 1000;
    int cores = 0;
    int tick_hz = 0;
    const char* sample_file = nullptr;
    for (int i = 1; i < argc; ++i) {
        if ((0 == std::strcmp(argv[i], "--mode")) && (i + 1 < argc)) {
            mode = argv[++i];
        }
        else if ((0 == std::strcmp(argv[i], "--seconds")) && (i + 1 < argc)) {
            seconds = std::atof(argv[++i]);
        }
        else if ((0 == std::strcmp(argv[i], "--hz")) && (i + 1 < argc)) {
            hz = std::atoi(argv[++i]);
        }
        else if ((0 == std::strcmp(argv[i], "--cores")) && (i + 1 < argc)) {
            cores = std::atoi(argv[++i]);
        }
        else if ((0 == std::strcmp(argv[i], "--tick-hz")) && (i + 1 < argc)) {
            tick_hz = std::atoi(argv[++i]);
        }
        else if ((0 == std::strcmp(argv[i], "--sample")) && (i + 1 < argc)) {
            sample_file = argv[++i];
        }
    }
    if (seconds <= 0.0) { seconds = 5.0; }
    if (hz < 10) { hz = 1000; }
    if (mode != "staged" && mode != "direct") {
        std::fprintf(stderr, "unknown --mode '%s' (staged|direct)\n", mode.c_str());
        return 2;
    }

    /* Pin to CPUs 0..cores-1 before any thread is created so every thread
       (producers + Dispatcher) inherits the affinity. */
    if (cores > 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        const unsigned avail =
            std::thread::hardware_concurrency() > 0U
                ? std::thread::hardware_concurrency() : 1U;
        const int n = std::min(cores, static_cast<int>(avail));
        for (int i = 0; i < n; ++i) { CPU_SET(i, &set); }
        if (0 != sched_setaffinity(0, sizeof(set), &set)) {
            std::fprintf(stderr, "warning: sched_setaffinity failed\n");
        }
    }

    signal(SIGABRT, abort_dump);   /* capture assert crashes without gdb */

    std::atomic<long> cnt_normal{0};
    std::atomic<long> cnt_high{0};
    std::atomic<long> cnt_direct{0};
    AoNormal ao_n(kStates, 2U, kTrans, 1U, 1, 4U);
    AoHigh   ao_h(kStates, 2U, kTrans, 1U, 1, 4U);
    AoDirect ao_d(kStates, 2U, kTrans, 1U, 1, 4U);
    ao_n.context() = BenchCtx{&cnt_normal};
    ao_h.context() = BenchCtx{&cnt_high};
    ao_d.context() = BenchCtx{&cnt_direct};

    coact::Event init_e;
    init_e.signal = 0U; init_e.pool_id = 0U; init_e.ref_ctr = 0U;
    ao_n.init(init_e);
    ao_h.init(init_e);
    ao_d.init(init_e);

    coact::EventPool<kBlk, kCap> pool;
    pool.init(g_storage, sizeof(g_storage));

    coact::pal::Posix pal;
    if (tick_hz > 0) { pal.set_tick_hz(static_cast<uint32_t>(tick_hz)); }
    coact::Runtime<coact::DefaultConfig, coact::pal::Posix> rt(pal);
    if (mode == "staged") {
        if (!rt.bind(&ao_n) || !rt.bind(&ao_h)) { return 3; }
    }
    else {
        if (!rt.bind(&ao_d)) { return 3; }
    }
    if (!rt.initialize()) { return 4; }
    rt.start();

    std::atomic<bool> stop{false};
    const coact::EventQos qos{false, false};
    using CoordT = coact::Runtime<coact::DefaultConfig,
                                  coact::pal::Posix>::CoordinatorType;
    CoordT& coord = rt.coordinator();

    std::vector<std::thread> producers;
    if (mode == "staged") {
        producers.emplace_back([&]() { producer_loop(stop, pool, coord, coact::TargetId(1U), qos); });
        producers.emplace_back([&]() { producer_loop(stop, pool, coord, coact::TargetId(2U), qos); });
    }
    else {
        producers.emplace_back([&]() { producer_loop(stop, pool, coord, coact::TargetId(1U), qos); });
    }

    if (nullptr != sample_file) { prof::start(hz); }

    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(
               std::chrono::steady_clock::now() - t0).count() < seconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : producers) { t.join(); }
    if (nullptr != sample_file) { prof::stop(); }
    rt.stop();

    long total = 0L;
    if (mode == "staged") {
        total = cnt_normal.load() + cnt_high.load();
        std::printf("staged: normal=%ld high=%ld total=%ld in %.3fs -> %.0f ev/s pool.used=%u\n",
                    cnt_normal.load(), cnt_high.load(), total, seconds,
                    (seconds > 0.0) ? static_cast<double>(total) / seconds : 0.0,
                    static_cast<unsigned>(pool.used()));
    }
    else {
        total = cnt_direct.load();
        std::printf("direct: total=%ld in %.3fs -> %.0f ev/s pool.used=%u\n",
                    total, seconds,
                    (seconds > 0.0) ? static_cast<double>(total) / seconds : 0.0,
                    static_cast<unsigned>(pool.used()));
    }

    if (nullptr != sample_file) {
        FILE* f = std::fopen(sample_file, "w");
        if (nullptr != f) {
            prof::write_folded(f, true);
            std::fclose(f);
        }
    }
    return 0;
}
