// coact::diag - static embedded logging for coact (design 2026-08-12-static-
// embedded-logging-design.md). Fixed-length 24-byte binary records, compile-time
// level gating, two static queues, admission-only drop, writer-side rendering.
//
// Design decisions (project decision log 2026-08-12):
//   - Drop is admission-only: a record rejected at enqueue counts
//     dropped_at_enqueue; once accepted it is never dropped, so the
//     conservation identity drained + queued + dropped_at_enqueue == accepted
//     holds with NO after-accept term.
//   - Wake is two-state: wait_block() is the production path (pure blocking
//     semaphore, no timeout, honoring "no polling backoff"); wait_bounded() is
//     only for tests/heartbeat (a documented bounded poll).
//
// This header is the platform-independent core. The platform adapter injects
// ClockOps / SinkOps / WakeOps and the queue backend as template parameters;
// the cmdfw catalog binds EventId -> Level/Category/args.
//
// Dependency direction (design §3): event/pool/staging/dispatcher must NOT
// include this header. Optional sibling component of coact core.
//
// Constraints: C++17, -fno-exceptions, -fno-rtti, no dynamic allocation, no
// TLS, no atexit, no producer printf/vsnprintf, no logging recursion.
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "coact/pal.hpp"  // CriticalSection (lane gate)

namespace coact {
namespace diag {

// ---------------------------------------------------------------------------
// LogLevel / lanes
// ---------------------------------------------------------------------------
enum class LogLevel : uint8_t {
    kDebug = 0U,
    kInfo = 1U,
    kWarn = 2U,
    kError = 3U,
};

static constexpr uint8_t kLevelDebug = 0U;
static constexpr uint8_t kLevelInfo = 1U;
static constexpr uint8_t kLevelWarn = 2U;
static constexpr uint8_t kLevelError = 3U;

// Logical lane: Error -> Critical; Debug/Info/Warn -> Normal. A hard split so
// an Error burst never consumes Normal capacity (design §5). Fatal is handled
// by a separate PanicSinkOps, not a queue lane.
enum class LogLane : uint8_t {
    kCritical = 0U,
    kNormal = 1U,
};

static constexpr LogLane lane_of(LogLevel level) noexcept
{
    return (LogLevel::kError == level) ? LogLane::kCritical : LogLane::kNormal;
}

// Argument display kind; the writer renders a fixed-width value, never a
// general printf format string (design §7).
enum class LogArgKind : uint8_t {
    kU32 = 0U,
    kHex = 1U,
    kBool = 2U,
};

static constexpr uint16_t kMaxArgs = 4U;

// ---------------------------------------------------------------------------
// 24-byte fixed-length record. Trivially copyable & destructible so the queue
// backend can memcpy/move it and the consumer needs no destructor. Level /
// category / argument kinds are static catalog facts, NOT stored per record
// (design §4.1).
// ---------------------------------------------------------------------------
struct alignas(4) LogRecord {
    uint32_t counter;    // raw 32-bit hardware / rt_tick counter snapshot
    uint16_t event_id;   // catalog event id (1-based; 0 reserved/invalid)
    uint16_t source_id;  // producer identity
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
};

static_assert(sizeof(LogRecord) == 24U,
              "coact::diag: LogRecord must be exactly 24 bytes");
static_assert(std::is_trivially_copyable<LogRecord>::value,
              "coact::diag: LogRecord must be trivially copyable");
static_assert(std::is_trivially_destructible<LogRecord>::value,
              "coact::diag: LogRecord must be trivially destructible");

// Optional writer-side render result. Product catalogs use this fixed-size
// return value without participating in producer-side logging.
struct LogRenderResult {
    uint16_t length;
    bool handled;
    bool truncated;
};

static_assert(std::is_trivially_copyable<LogRenderResult>::value,
              "coact::diag: LogRenderResult must be trivially copyable");

// ---------------------------------------------------------------------------
// DiagRing - fixed-capacity raw ring used as the lane storage. It carries NO
// internal lock: every access must happen inside the Logger's single injected
// CriticalSection. This is the design-correct decoupling - using a queue that
// already owns a CriticalSection (e.g. SingleCoreCriticalRing) inside the
// Logger would double-lock (nested save/restore) and break on a non-reentrant
// CS. Each lane has its own DiagRing with its own capacity, so Normal and
// Critical can be sized independently (design §5.1).
// ---------------------------------------------------------------------------
template <typename T, uint16_t Capacity>
struct DiagRing {
    static_assert(Capacity > 0U, "coact::diag: lane ring capacity must be non-zero");

    // push under the caller-held CS. Returns false when full.
    bool try_push(T&& v) noexcept
    {
        if (count_ >= Capacity) {
            return false;
        }
        cells_[(write_index_ + count_) % Capacity] = std::move(v);
        ++count_;
        return true;
    }

    bool try_pop(T& out) noexcept
    {
        if (0U == count_) {
            return false;
        }
        out = std::move(cells_[write_index_]);
        write_index_ = static_cast<uint16_t>((write_index_ + 1U) % Capacity);
        --count_;
        return true;
    }

    uint16_t size() const noexcept { return count_; }

    void clear() noexcept
    {
        count_ = 0U;
        write_index_ = 0U;
    }

    T cells_[Capacity];
    uint16_t write_index_ = 0U;
    uint16_t count_ = 0U;
};

// ---------------------------------------------------------------------------
// Catalog descriptor (design §4.2). One constexpr entry per event_id; sorted
// by event_id and validated by static_assert at the instantiation site.
// ---------------------------------------------------------------------------
struct LogDescriptor {
    uint16_t event_id;                        // non-zero, unique
    LogLevel level;                           // drives lane + compile-time gate
    uint16_t argument_count;                  // 0..kMaxArgs
    std::array<LogArgKind, kMaxArgs> argument_kinds;
    const char* event_name;                   // ROM string
};

// Static sorted catalog; writer scans it linearly (few events). The producer
// path never touches it.
struct LogCatalog {
    const LogDescriptor* entries;
    uint16_t count;

    const LogDescriptor* find(uint16_t event_id) const noexcept
    {
        for (uint16_t i = 0U; i < count; ++i) {
            if (entries[i].event_id == event_id) {
                return &entries[i];
            }
        }
        return nullptr;  // catalog_miss
    }
};

// ---------------------------------------------------------------------------
// Platform hooks (design §6.1). Function-pointer tables injected at
// initialize(), validated once, frozen at start. No std::function, no vtable.
// ---------------------------------------------------------------------------
struct LogClockOps {
    uint32_t (*read_counter)(void* context) noexcept;
    uint32_t frequency_hz;
    void* context;
};

struct LogSinkOps {
    bool (*write)(void* context, const char* bytes, uint16_t length) noexcept;
    void* context;
};

struct LogWakeOps {
    void (*signal_from_task)(void* context) noexcept;
    void (*signal_from_isr)(void* context) noexcept;
    void (*wait_block)(void* context) noexcept;
    void (*wait_bounded)(void* context, uint32_t timeout_ms) noexcept;
    void* context;
};

// ---------------------------------------------------------------------------
// Saturation + conservation counters (design §9). Snapshot through the same CS
// so a reader gets a consistent view.
// ---------------------------------------------------------------------------
struct LogStats {
    uint32_t accepted_critical;
    uint32_t accepted_normal;
    uint32_t dropped_at_enqueue_critical;
    uint32_t dropped_at_enqueue_normal;
    uint32_t drained_critical;
    uint32_t drained_normal;
    uint16_t critical_high_watermark;
    uint16_t normal_high_watermark;
    uint32_t catalog_miss;
    uint32_t formatter_truncated;
    uint32_t sink_failed;
    uint32_t wake_signal;
    uint32_t panic_count;

    void reset() noexcept
    {
        *this = LogStats{};
    }
};

// ---------------------------------------------------------------------------
// Compile-time level gate (design §4.3). Preprocessor-level: a disabled branch
// never references the incoming statement, so its arguments are never evaluated
// and its strings vanish from Release objects. Producer level is the product
// threshold (kept in fw_profile / build flags). Levels are namespace-qualified
// (so the macro works outside namespace coact::diag), and the gate is variadic
// (so a template-id argument such as `record<kInfo,kX>` — whose angle-bracket
// comma would otherwise split a single-argument macro — is captured whole).
//
//   CMDFW_LOG_INFO(logger.record<LogLevel::kInfo, LogEventId::kX>(src, a));
//
// CMDFW_LOG_MIN_LEVEL defaults to 0 (all enabled); override at build time.
// ---------------------------------------------------------------------------
#if !defined(CMDFW_LOG_MIN_LEVEL)
#define CMDFW_LOG_MIN_LEVEL 0U
#endif

#define CMDFW_DIAG_GATE(level_num, statement) \
    do { if (CMDFW_LOG_MIN_LEVEL <= (level_num)) { statement; } } while (false)

#define CMDFW_LOG_DEBUG(...) CMDFW_DIAG_GATE(::coact::diag::kLevelDebug, (__VA_ARGS__))
#define CMDFW_LOG_INFO(...)  CMDFW_DIAG_GATE(::coact::diag::kLevelInfo, (__VA_ARGS__))
#define CMDFW_LOG_WARN(...)  CMDFW_DIAG_GATE(::coact::diag::kLevelWarn, (__VA_ARGS__))
#define CMDFW_LOG_ERROR(...) CMDFW_DIAG_GATE(::coact::diag::kLevelError, (__VA_ARGS__))

// ---------------------------------------------------------------------------
// Logger<NormalCap, CriticalCap, ...>. Lane storage is two fixed-capacity
// DiagRings owned directly by the Logger, guarded by ONE injected
// CriticalSection. Each lane has its own capacity, so Normal and Critical are
// sized independently (design §5.1) and the double-lock hazard of compositing
// the Logger's CS with a self-locking queue (e.g. SingleCoreCriticalRing) is
// structurally removed: DiagRing holds no lock.
//
// Producer path (design §5.2):
//   read_counter (outside CS) -> one CS {admit/drop, copy record, bump stats}
//   -> (outside CS) signal writer. Clock/sink/wake are never called inside CS.
// ---------------------------------------------------------------------------
template <uint16_t NormalCapacity,
          uint16_t CriticalCapacity,
          uint8_t NormalDebugWatermarkPercent,
          uint8_t NormalInfoWatermarkPercent>
class Logger {
    // Watermark is a percentage of capacity; 100 == full (no early admission
    // rejection, only the queue-full condition drops).
    static constexpr uint8_t kPercentFull = 100U;
    static constexpr uint8_t kFullWatermark = kPercentFull;

    static_assert(NormalCapacity > 0U && CriticalCapacity > 0U,
                  "coact::diag: queues must be non-empty");
    static_assert(NormalDebugWatermarkPercent <= kPercentFull &&
                      NormalInfoWatermarkPercent <= kPercentFull,
                  "coact::diag: watermark percentages must be 0..100");

public:
    Logger() noexcept = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // ---- Producer: typed record entry (compiled per level/event). -------
    // record() is the task-path entry; record_from_isr() is the ISR-path entry
    // (design §8). Arity overloads 0..4; the producer never formats or allocates.
    // The ISR version never initializes / blocks / allocates and wakes the
    // writer via signal_from_isr (ISR-safe). Catalog arity/arg-kind validation is
    // a writer-side / build-time concern, not a producer-path check.
    template <LogLevel Level, uint16_t EventId>
    void record(uint16_t source_id) noexcept
    {
        record_impl<Level, EventId>(false, source_id, 0U, 0U, 0U, 0U);
    }
    template <LogLevel Level, uint16_t EventId>
    void record(uint16_t source_id, uint32_t a0) noexcept
    {
        record_impl<Level, EventId>(false, source_id, a0, 0U, 0U, 0U);
    }
    template <LogLevel Level, uint16_t EventId>
    void record(uint16_t source_id, uint32_t a0, uint32_t a1) noexcept
    {
        record_impl<Level, EventId>(false, source_id, a0, a1, 0U, 0U);
    }
    template <LogLevel Level, uint16_t EventId>
    void record(uint16_t source_id, uint32_t a0, uint32_t a1,
                uint32_t a2) noexcept
    {
        record_impl<Level, EventId>(false, source_id, a0, a1, a2, 0U);
    }
    template <LogLevel Level, uint16_t EventId>
    void record(uint16_t source_id, uint32_t a0, uint32_t a1,
                uint32_t a2, uint32_t a3) noexcept
    {
        record_impl<Level, EventId>(false, source_id, a0, a1, a2, a3);
    }

    template <LogLevel Level, uint16_t EventId>
    void record_from_isr(uint16_t source_id) noexcept
    {
        record_impl<Level, EventId>(true, source_id, 0U, 0U, 0U, 0U);
    }
    template <LogLevel Level, uint16_t EventId>
    void record_from_isr(uint16_t source_id, uint32_t a0) noexcept
    {
        record_impl<Level, EventId>(true, source_id, a0, 0U, 0U, 0U);
    }
    template <LogLevel Level, uint16_t EventId>
    void record_from_isr(uint16_t source_id, uint32_t a0, uint32_t a1) noexcept
    {
        record_impl<Level, EventId>(true, source_id, a0, a1, 0U, 0U);
    }
    template <LogLevel Level, uint16_t EventId>
    void record_from_isr(uint16_t source_id, uint32_t a0, uint32_t a1,
                         uint32_t a2) noexcept
    {
        record_impl<Level, EventId>(true, source_id, a0, a1, a2, 0U);
    }
    template <LogLevel Level, uint16_t EventId>
    void record_from_isr(uint16_t source_id, uint32_t a0, uint32_t a1,
                         uint32_t a2, uint32_t a3) noexcept
    {
        record_impl<Level, EventId>(true, source_id, a0, a1, a2, a3);
    }

    // ---- Consumer: writer drains one record from a lane. ----------------
    // Lane drain order (up to 4 Critical per 1 Normal, design §5.4) is the
    // caller's loop policy, not enforced here. No-op (false) before bind().
    bool pop_from_lane(LogLane lane, LogRecord& out) noexcept
    {
        if (!bound()) {
            return false;
        }
        const CriticalSection::Token token = cs_.save(cs_.ctx);
        bool ok = false;
        DiagRing<LogRecord, CriticalCapacity>& crit = critical_ring_;
        if (LogLane::kCritical == lane) {
            ok = crit.try_pop(out);
        }
        else {
            ok = normal_ring_.try_pop(out);
        }
        if (ok) {
            if (LogLane::kCritical == lane) {
                ++stats_.drained_critical;
            }
            else {
                ++stats_.drained_normal;
            }
        }
        cs_.restore(cs_.ctx, token);
        return ok;
    }

    uint16_t size(LogLane lane) const noexcept
    {
        if (!bound()) {
            return 0U;
        }
        const CriticalSection::Token token = cs_.save(cs_.ctx);
        const uint16_t n = (LogLane::kCritical == lane)
                               ? critical_ring_.size()
                               : normal_ring_.size();
        cs_.restore(cs_.ctx, token);
        return n;
    }

    // Consistent snapshot under one CS (design §9).
    LogStats snapshot() noexcept
    {
        const CriticalSection::Token token = cs_.save(cs_.ctx);
        LogStats s = stats_;
        s.critical_high_watermark = critical_hwm_;
        s.normal_high_watermark = normal_hwm_;
        cs_.restore(cs_.ctx, token);
        return s;
    }

    // Writer-side stat write entries (design §9). The writer / platform adapter
    // reports formatter truncation / sink failure / catalog miss here; these are
    // bumped under the same CS so snapshot() reads a consistent view. No-op
    // before bind() (cs_ is a null hook then).
    void note_formatter_truncated() noexcept
    {
        if (!bound()) {
            return;
        }
        const CriticalSection::Token token = cs_.save(cs_.ctx);
        ++stats_.formatter_truncated;
        cs_.restore(cs_.ctx, token);
    }
    void note_sink_failed() noexcept
    {
        if (!bound()) {
            return;
        }
        const CriticalSection::Token token = cs_.save(cs_.ctx);
        ++stats_.sink_failed;
        cs_.restore(cs_.ctx, token);
    }
    void note_catalog_miss() noexcept
    {
        if (!bound()) {
            return;
        }
        const CriticalSection::Token token = cs_.save(cs_.ctx);
        ++stats_.catalog_miss;
        cs_.restore(cs_.ctx, token);
    }

    // Reset for tests / init-failure path. Requires quiescent producers.
    void reset() noexcept
    {
        if (!bound()) {
            return;
        }
        const CriticalSection::Token token = cs_.save(cs_.ctx);
        critical_ring_.clear();
        normal_ring_.clear();
        stats_.reset();
        critical_hwm_ = 0U;
        normal_hwm_ = 0U;
        cs_.restore(cs_.ctx, token);
    }

    // ---- Platform wiring (design §6.2): validate + freeze at start. -----
    // Sets the CS and the injected hooks. Must be called exactly once before any
    // producer/consumer work. Returns false if already bound or a required hook
    // is null.
    bool bind(LogClockOps clock,
              LogSinkOps sink,
              LogWakeOps wake,
              CriticalSection cs) noexcept
    {
        if (bound()) {
            return false;  // already bound
        }
        if (nullptr == cs.save || nullptr == cs.restore) {
            return false;  // a null queue gate is invalid for this core
        }
        if (nullptr == wake.signal_from_task) {
            return false;  // wake is required for the production no-poll path
        }
        clock_ = clock;
        sink_ = sink;
        wake_ = wake;
        cs_ = cs;
        return true;
    }
    bool bound() const noexcept { return (nullptr != cs_.save); }
    LogClockOps clock() const noexcept { return clock_; }
    LogSinkOps sink() const noexcept { return sink_; }
    LogWakeOps wake() const noexcept { return wake_; }

private:
    template <LogLevel Level, uint16_t EventId>
    void record_impl(bool isr, uint16_t source_id, uint32_t a0, uint32_t a1,
                     uint32_t a2, uint32_t a3) noexcept
    {
        static_assert(EventId != 0U, "coact::diag: event id 0 is reserved");
        if (!bound()) {
            return;  // recording before bind() is a config error: drop silently
        }

        const LogLane lane = lane_of(Level);
        const uint32_t counter = read_counter();

        LogRecord r;
        r.counter = counter;
        r.event_id = EventId;
        r.source_id = source_id;
        r.arg0 = a0;
        r.arg1 = a1;
        r.arg2 = a2;
        r.arg3 = a3;

        const CriticalSection::Token token = cs_.save(cs_.ctx);
        bool accepted = false;
        const bool can_signal = isr ? (nullptr != wake_.signal_from_isr)
                                   : (nullptr != wake_.signal_from_task);
        if (LogLane::kCritical == lane) {
            accepted = push_lane(critical_ring_, r, kFullWatermark,
                                 CriticalCapacity, stats_.accepted_critical,
                                 stats_.dropped_at_enqueue_critical,
                                 critical_hwm_);
        }
        else {
            // Normal lane admission watermarks by level (design §5.1):
            // Debug 50%, Info 75%, Warn full-only.
            uint8_t watermark = kFullWatermark;
            if (LogLevel::kDebug == Level) {
                watermark = NormalDebugWatermarkPercent;
            }
            else if (LogLevel::kInfo == Level) {
                watermark = NormalInfoWatermarkPercent;
            }
            accepted = push_lane(normal_ring_, r, watermark, NormalCapacity,
                                 stats_.accepted_normal,
                                 stats_.dropped_at_enqueue_normal, normal_hwm_);
        }
        if (accepted && can_signal) {
            saturating_increment(stats_.wake_signal);
        }
        cs_.restore(cs_.ctx, token);

        // Signal the writer only after the CS is dropped (design §5.2). The
        // ISR entry wakes via signal_from_isr (ISR-safe); the task entry via
        // signal_from_task.
        if (accepted) {
            if (isr) {
                if (nullptr != wake_.signal_from_isr) {
                    wake_.signal_from_isr(wake_.context);
                }
            }
            else if (nullptr != wake_.signal_from_task) {
                wake_.signal_from_task(wake_.context);
            }
        }
    }

    uint32_t read_counter() const noexcept
    {
        return (nullptr != clock_.read_counter) ? clock_.read_counter(clock_.context)
                                                : 0U;
    }

    static void saturating_increment(uint32_t& value) noexcept
    {
        if (value < 0xFFFFFFFFU) {
            ++value;
        }
    }

    // Admit-or-drop under an already-held CS. Rejects when the lane is past its
    // admission watermark of capacity; this is the ONLY drop point (design
    // §5.1, admission-only). On accept, pushes the record (moved into the ring),
    // bumps the accepted counter, and updates the lane HWM. Returns true iff
    // accepted. If try_push still fails despite the pre-check (defensive), the
    // record is counted as dropped so the conservation identity holds.
    template <typename Ring>
    bool push_lane(Ring& q, LogRecord& r, uint8_t watermark_percent,
                   uint16_t cap, uint32_t& accepted, uint32_t& dropped,
                   uint16_t& hwm) noexcept
    {
        const uint16_t threshold = (watermark_percent * cap) / kPercentFull;
        if (q.size() >= threshold) {
            ++dropped;
            return false;
        }
        if (q.try_push(std::move(r))) {
            ++accepted;
            const uint16_t now = q.size();
            if (now > hwm) {
                hwm = now;
            }
            return true;
        }
        ++dropped;
        return false;
    }


    // Lane storage is fixed-capacity DiagRings owned directly by the Logger.
    // They hold no lock; every access is inside the Logger's single cs_.
    DiagRing<LogRecord, NormalCapacity> normal_ring_;
    DiagRing<LogRecord, CriticalCapacity> critical_ring_;

    LogClockOps clock_{};
    LogSinkOps sink_{};
    LogWakeOps wake_{};
    CriticalSection cs_{};

    LogStats stats_{};
    uint16_t critical_hwm_ = 0U;
    uint16_t normal_hwm_ = 0U;
};

}  // namespace diag
}  // namespace coact
