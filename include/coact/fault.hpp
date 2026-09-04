// coact fault reporting injection point (R1).
// SPDX-License-Identifier: MIT
//
// Structure borrowed from newosp's fault_collector.hpp FaultReporter and
// system_monitor.hpp StateCrossed (newosp is MIT-licensed); this file is a
// coact-style rewrite, not a copy.
//
// FaultReporter is the core-neutral fault boundary, mirroring TraceOps: the
// core stores a POD {fn, ctx} pair and forwards on the hot path with a single
// null check. A null fn is zero overhead - no fault is generated. The
// application layer wires fn to its fault sink at startup.
//
// state_crossed() is a constexpr pure edge detector used to report state
// transitions (e.g. a watermark crossing a threshold) rather than levels, so
// sinks are notified only when the state actually changes.
#pragma once

#include <cstdint>

namespace coact {

// ---------------------------------------------------------------------------
// FaultPriority: severity carried with every fault report.
//
// coact ordering is ASCENDING (kLow=0 .. kCritical=3), matching BreakerLevel
// and LogLevel where a higher value means higher severity. newosp's
// FaultPriority uses the OPPOSITE direction (kCritical=0). The two encodings
// must never be compared numerically across the boundary: an adapter wiring
// a coact FaultReporter to a newosp-style sink owns the mapping.
// ---------------------------------------------------------------------------
enum class FaultPriority : uint8_t {
    kLow = 0U,
    kMedium = 1U,
    kHigh = 2U,
    kCritical = 3U,
};

// Fault report callback signature. A callback may be invoked from either task
// or ISR context (e.g. sample_watermark runs on the Dispatcher thread, but a
// future ISR-side fault point may call from a handler). Every implementation
// must therefore be ISR-SAFE, not merely task-safe:
//   allowed:   fixed-width stores, lock-free atomic ops, ISR-safe wake hints.
//   forbidden: mutex, blocking semaphore, formatting, allocation, thread
//              creation. The adapter that binds fn owns this guarantee - a
//              comment on the caller side is not sufficient.
//   fault_index: module-defined fault point identifier.
//   detail:      module-defined payload (may pack several fields).
//   priority:    severity of this report.
//   ctx:         user context pointer bound with the callback.
using FaultReportFn = void (*)(uint16_t fault_index, uint32_t detail,
                               FaultPriority priority, void* ctx) noexcept;

// ---------------------------------------------------------------------------
// FaultReporter: POD injection point. Default (null fn) disables reporting.
// ---------------------------------------------------------------------------
struct FaultReporter {
    FaultReportFn fn;   // null = disabled
    void* ctx;          // user context, forwarded verbatim to fn

    // Forward a fault if a sink is bound; a no-op otherwise.
    void report(uint16_t fault_index, uint32_t detail,
                FaultPriority priority) const noexcept
    {
        if (nullptr != fn) {
            fn(fault_index, detail, priority, ctx);
        }
    }
};

// ---------------------------------------------------------------------------
// state_crossed: threshold edge detector. True when the value transitioned
// across the threshold between the previous and current sample, in either
// direction:
//   up:   prev <  threshold && curr >= threshold
//   down: prev >= threshold && curr <  threshold
// ---------------------------------------------------------------------------
template <typename T>
constexpr bool state_crossed(T prev, T curr, T threshold) noexcept
{
    return ((prev < threshold) && (curr >= threshold)) ||
           ((prev >= threshold) && (curr < threshold));
}

}  // namespace coact
