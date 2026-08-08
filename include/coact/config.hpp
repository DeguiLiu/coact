// coact minimal configuration and vocabulary types.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace coact {

// ---------------------------------------------------------------------------
// Logical priority: higher value == higher priority. Native priorities never
// leave the PAL. See design 3.2.
// ---------------------------------------------------------------------------
typedef uint8_t LogicalPrio;

enum : LogicalPrio {
    kInvalidPrio = 0U,
    kMinPrio = 1U,
    kMaxPrio = 63U
};

enum class PriorityClass : uint8_t {
    High,
    Normal,
    Low
};

// ---------------------------------------------------------------------------
// Execution context of the current producer. See design 3.1.
// ---------------------------------------------------------------------------
enum class ContextKind : uint8_t {
    Task,
    Dispatcher,
    Isr
};

struct ExecutionContext {
    ContextKind kind;
    uint8_t logical_prio;
    uint8_t direct_depth;
    bool prio_valid;
};

// ---------------------------------------------------------------------------
// Event QoS. The target AO's fixed PriorityClass is the sole authority for
// partition selection; qos does not carry a per-event priority class.
// See design 3.3.
// ---------------------------------------------------------------------------
struct EventQos {
    bool critical;
    bool mergeable;
};

typedef uint8_t TargetId;
enum : TargetId {
    kInvalidTarget = 0U
};

typedef uint16_t Signal;

// ---------------------------------------------------------------------------
// Submit disposition. See design 8.2.
// ---------------------------------------------------------------------------
enum class SubmitDisposition : uint8_t {
    Direct,
    Queued,
    Merged,
    DroppedPolicy,
    DroppedRateLimit,
    DroppedOverload,
    RejectedFull,
    RejectedState
};

struct SubmitResult {
    SubmitDisposition disposition;
    uint16_t reason;
};

// ---------------------------------------------------------------------------
// Module error enums.
// ---------------------------------------------------------------------------
enum class PoolError : uint8_t {
    kPoolExhausted,
    kInvalidPointer
};

enum class InitError : uint8_t {
    kPalFailed,
    kConfigMismatch,
    kDuplicatePrio,
    kCapacityOverflow,
    kAlreadyBound,
    kNoAo
};

// ---------------------------------------------------------------------------
// Default configuration. See design 14.1.
// ---------------------------------------------------------------------------
struct DefaultConfig {
    enum : uint8_t {
        kMaxAo = 16U,
        kMaxStateDepth = 6U,
        kMaxDirectDepth = 4U,
        kBatchSizeMax = 8U
    };

    enum : uint16_t {
        kHighCapacity = 32U,
        kNormalCapacity = 64U,
        kLowCapacity = 128U,
        kCooldownCycles = 100U
    };

    enum : uint32_t {
        kBatchTimeoutMs = 5U,
        kLowMaxWaitMs = 100U,
        /* Dispatcher thread stack size. Board Configs override this to match
           the deepest HSM action chain (measure first, then tune). */
        kDispatcherStackBytes = 4096U
    };

    enum : uint64_t {
        kDirectBudgetNs = 50000ULL,
        kRtcBudgetNs = 1000000ULL
    };
};

}  // namespace coact
