// coact::coro task slot: per-slot state machine, waiter registration and
// fixed result storage. This is detail - public code goes through
// TaskRegistry / Task / Promise / AwaitableRef.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "coact/config.hpp"

#include "coact/coro/config.hpp"
#include "coact/coro/detail/fixed_storage.hpp"
#include "coact/coro/error.hpp"
#include "coact/coro/task_id.hpp"

namespace coact {
namespace coro {
namespace detail {

// Task lifecycle states (plan §3.1). Valid transitions:
//   kCreated  -> kWaiting   (set_awaiter: the consumer AO registered itself)
//   kCreated  -> kCompleted (complete before anyone waited; the completion
//                            event still fires to the registered/no target)
//   kWaiting  -> kCompleted (complete / fail / cancel)
// Cancel and double-complete are explicit rejections counted by the registry,
// never silent state overwrites.
enum class TaskSlotState : uint8_t {
    kFree = 0U,     // slot unoccupied
    kCreated,       // registered, no waiter yet
    kWaiting,       // waiter AO registered (TargetId + signal)
    kCompleted      // result or error stored, awaiting release
};

// One task slot. Standard-layout, fixed size: TaskId packing, state, error,
// waiter target/signal, result storage and result/error presence flags.
// The slot itself lives inside TaskRegistry's std::array - never heap.
template <typename T>
struct TaskSlot final {
    static_assert(is_coro_result_v<T>,
                  "coact::coro: task result must satisfy the coro result "
                  "contract (trivially copyable/destructible, standard-layout)");

    // Identity (slot index implied by array position; generation identifies
    // the incarnation). id.value() == 0 marks a free slot.
    // Generation starts at 1: a first-incarnation slot-0 task must never
    // pack into TaskId 0, which collides with kInvalidTaskId (the TaskId
    // packing is [15:8] generation | [7:0] slot index).
    uint16_t generation = 1U;

    TaskSlotState state = TaskSlotState::kFree;

    TaskError error = TaskError::kOk;   // failure reason when has_error
    TargetId waiter = kInvalidTarget;   // AO to notify on completion
    uint16_t waiter_signal = 0U;        // signal for the completion event
    coact::EventQos qos{false, false};  // QoS of the completion event

    bool has_result = false;            // result bytes hold a value
    bool has_error = false;             // error field holds a failure

    FixedStorage<T> result{};

    bool occupied() const noexcept
    {
        return TaskSlotState::kFree != state;
    }

    // Reset for slot reuse: bump the generation, clear everything. The
    // generation wraps at 256 to 0 (documented on TaskId); release() maps
    // the wrapped 0 back to 1 so generation 0 is never observable (a
    // slot-0 task with generation 0 would pack into kInvalidTaskId).
    void release() noexcept
    {
        generation = static_cast<uint16_t>(generation + 1U);
        if (0U == generation) {
            generation = 1U;
        }
        state = TaskSlotState::kFree;
        error = TaskError::kOk;
        waiter = kInvalidTarget;
        waiter_signal = 0U;
        has_result = false;
        has_error = false;
        result.clear();
    }
};

}  // namespace detail
}  // namespace coro
}  // namespace coact
