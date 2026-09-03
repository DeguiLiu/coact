// coact::coro configuration vocabulary: default capacities, completion-event
// block layout and compile-time contract checks.
// SPDX-License-Identifier: MIT
//
// Everything here is compile-time only: no runtime state, no heap, no
// exceptions. Board configs override the capacities through their own Config
// struct (coact::DefaultConfig style); the defaults below keep the library
// usable out of the box in host tests.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "coact/event.hpp"
#include "coact/pool.hpp"

namespace coact {
namespace coro {

// Compile-time library configuration. A board replaces this struct with its
// own (same named constants) and threads it through TaskRegistry /
// combinators. All values are fixed at compile time - there is no runtime
// capacity tuning.
struct AsyncConfig {
    // Default task-slot capacity of a TaskRegistry instantiation.
    enum : uint16_t { kDefaultMaxTasks = 8U };

    // Default combinator capacity (max tasks per TaskGroup).
    enum : uint16_t { kDefaultGroupCapacity = 8U };

    // Completion events carry the TaskId plus a status word; large results
    // stay in the task slot and travel by reference (result descriptor rule).
    // Block size must still fit a pool block (Event header + payload).
    enum : uint16_t { kCompletionEventBlockBytes = 16U };

    // Generation counter guard: a TaskId packs slot index + generation so a
    // stale handle cannot alias a recycled slot.
    enum : uint16_t { kSlotGenerationBits = 8U };

    static_assert(kDefaultGroupCapacity <= 32U,
                  "TaskGroup completed bitmap is uint32_t: capacity <= 32");
};

// Completion-event payload (trivially copyable, pool lifecycle contract).
// The result itself NEVER travels in the event: the consumer reads it from
// the task slot through the registry before release_task().
struct CompletionPayload {
    uint16_t task_id;     // raw TaskId value (slot index + generation packing)
    uint8_t status;       // 0 = success, 1 = failed, 2 = cancelled
    uint8_t reserved;     // alignment padding, always 0
    uint32_t error_code;  // TaskError raw value when status != 0
};

static_assert(std::is_standard_layout<CompletionPayload>::value &&
                  std::is_trivially_copyable<CompletionPayload>::value &&
                  std::is_trivially_destructible<CompletionPayload>::value,
              "coact::coro: CompletionPayload must satisfy the pool "
              "trivial-lifecycle contract");

// Composed completion event block: coact::Event at offset 0, then the
// payload. The pool lifecycle contract requires
// is_nothrow_default_constructible<Layout>: the payload member carries a
// defaulted (non-trivial, but nothrow) default constructor, so the layout
// default constructor must be explicitly declared defaulted here (a
// synthesized one would be non-trivial and still nothrow, but the contract
// checks nothrow only - declaring it keeps intent explicit).
struct CompletionBlock
    : EventBlockLayout<CompletionPayload, sizeof(CompletionPayload),
                       alignof(CompletionPayload)> {
    // Explicitly defaulted: keeps the layout nothrow default constructible
    // (a member initializer on CompletionPayload would otherwise synthesize
    // a constructor the pool contract still accepts, but this pins intent).
    CompletionBlock() noexcept = default;
};

// Status codes carried in CompletionPayload::status.
enum class CompletionStatus : uint8_t {
    kSucceeded = 0U,
    kFailed = 1U,
    kCancelled = 2U
};

// Compile-time check that a result type satisfies the coro result contract:
// results are stored in-place in task slots (raw byte storage + launder),
// so they must be trivially copyable/destructible and nothrow movable.
// Large data (frames, buffers, strings) must travel by descriptor instead.
template <typename T>
inline constexpr bool is_coro_result_v =
    std::is_standard_layout<T>::value &&
    std::is_trivially_copyable<T>::value &&
    std::is_trivially_destructible<T>::value &&
    std::is_nothrow_move_constructible<T>::value;

}  // namespace coro
}  // namespace coact
