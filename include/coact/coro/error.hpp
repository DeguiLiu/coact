// coact::coro error vocabulary: TaskError, AwaitError, GroupError and their
// name tables. All errors are strong-typed enums returned through
// coact::Expected; no exceptions anywhere.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace coact {
namespace coro {

// Task lifecycle error vocabulary (plan §3.1 / §5 Task 2).
enum class TaskError : uint8_t {
    kOk = 0U,
    kSlotsFull,          // registry has no free slot
    kInvalidId,          // stale / never-issued TaskId (generation mismatch)
    kAlreadyCompleted,   // second completion attempt on the same slot
    kCancelled,          // operation on a cancelled task
    kTargetRejected,     // completion event submission failed (pool / target)
    kPoolExhausted,      // completion event allocation failed
    kResultUnavailable,  // take_result() on a not-yet-completed slot
    kResultTaken,        // take_result() after the result was already taken
    kAwaiterBusy         // set_awaiter() on a slot that already has one
};

// Awaitable-side error vocabulary.
enum class AwaitError : uint8_t {
    kOk = 0U,
    kInvalidHandle,  // observing a moved-from / never-bound AwaitableRef
    kNotReady,       // is_ready() false when a result was requested
    kAlreadyAwaited  // duplicate await registration on the same task
};

// Combinator error vocabulary.
enum class GroupError : uint8_t {
    kOk = 0U,
    kRegistryFull,   // no free slot for the TaskGroup aggregate task
    kTooManyInputs,  // count > Capacity
    kZeroCount,      // count == 0
    kInvalidTask     // an input handle is invalid / stale
};

// Fixed error-name tables (monitor philosophy: no formatting, fixed strings).
inline const char* task_error_name(TaskError e) noexcept
{
    switch (e) {
    case TaskError::kOk:               return "kOk";
    case TaskError::kSlotsFull:        return "kSlotsFull";
    case TaskError::kInvalidId:        return "kInvalidId";
    case TaskError::kAlreadyCompleted: return "kAlreadyCompleted";
    case TaskError::kCancelled:        return "kCancelled";
    case TaskError::kTargetRejected:   return "kTargetRejected";
    case TaskError::kPoolExhausted:    return "kPoolExhausted";
    case TaskError::kResultUnavailable:return "kResultUnavailable";
    case TaskError::kResultTaken:      return "kResultTaken";
    case TaskError::kAwaiterBusy:      return "kAwaiterBusy";
    default:                           return "kUnknown";
    }
}

inline const char* await_error_name(AwaitError e) noexcept
{
    switch (e) {
    case AwaitError::kOk:             return "kOk";
    case AwaitError::kInvalidHandle:  return "kInvalidHandle";
    case AwaitError::kNotReady:       return "kNotReady";
    case AwaitError::kAlreadyAwaited: return "kAlreadyAwaited";
    default:                          return "kUnknown";
    }
}

inline const char* group_error_name(GroupError e) noexcept
{
    switch (e) {
    case GroupError::kOk:           return "kOk";
    case GroupError::kRegistryFull: return "kRegistryFull";
    case GroupError::kTooManyInputs:return "kTooManyInputs";
    case GroupError::kZeroCount:    return "kZeroCount";
    case GroupError::kInvalidTask:  return "kInvalidTask";
    default:                        return "kUnknown";
    }
}

}  // namespace coro
}  // namespace coact
