// coact::coro awaitable traits and event bridging.
// SPDX-License-Identifier: MIT
//
// This header adapts external completion sources (timers, IO completions,
// future cooperative workers) to the TaskRegistry event plane:
//   - AsyncCompletionSource trait: anything that can register a
//     (TargetId, signal) pair against a task id can drive a Task/Promise.
//   - CompletionEventPayload: decodes a CompletionBlock received by an AO
//     back into (task id, status, error) so the HSM action can resume.
//
// The wait action never stores a callback pointer: it writes TargetId +
// resume signal into the task slot; TaskRegistry generates the completion
// event (see task_registry.hpp). This is exactly the seam a future
// single-core cooperative worker (execute-logic sliced by the dispatcher)
// would plug into - its slices just complete promises, nothing else changes.
//
// Single-core model: no bridging component spawns a thread; external sources
// drive completion from the same dispatcher loop (or a poll()-driven
// TimerScheduler with ManualTickSource).
#pragma once

#include <cstdint>

#include "coact/coro/config.hpp"
#include "coact/coro/error.hpp"
#include "coact/coro/task.hpp"
#include "coact/coro/task_id.hpp"
#include "coact/config.hpp"
#include "coact/expected.hpp"

namespace coact {
namespace coro {

// Decoded view of a completion event received by a waiter AO. Decoding never
// casts: the AO reads the payload region of the composed block through the
// offset helpers of coact::EventBlockLayout.
struct CompletionEventPayload {
    TaskId id;
    CompletionStatus status;
    TaskError error;
};

// Decode a completion event payload from a raw pool block. `block` must be a
// CompletionBlock allocated by TaskRegistry (the waiter AO received it with
// its registered signal). Returns the decoded payload; no validation of the
// signal is possible here (the HSM table already selected the action by it).
inline CompletionEventPayload decode_completion(const void* block) noexcept
{
    const auto* layout = static_cast<const CompletionBlock*>(block);
    CompletionEventPayload out;
    out.id = TaskId(layout->meta.task_id);
    out.status = static_cast<CompletionStatus>(layout->meta.status);
    out.error = static_cast<TaskError>(layout->meta.error_code);
    return out;
}

// Decode from a coact::Event base pointer when the AO holds the Event& the
// dispatcher handed it. Equivalent to decode_completion(block) with the
// composed-layout offset math done once.
inline CompletionEventPayload decode_completion(const Event& event) noexcept
{
    // The Event header sits at offset 0 of a CompletionBlock, so the block
    // address equals the event address (coact::EventBlockLayout contract).
    return decode_completion(static_cast<const void*>(&event));
}

// Trait: a completion source can bind a waiter (TargetId + resume signal)
// against a task id. Implementations write the pair into their own storage
// (TaskSlot for the registry; external sources map it to their own tables).
// Primary use: generic glue in combinators and future worker slices that
// must not depend on TaskRegistry's concrete type.
template <typename Source>
struct CompletionSourceTraits {
    // By default any registry-like type exposes set_awaiter(id, t, s):
    // the trait adapts the argument order so glue code reads uniformly.
    static Expected<void, TaskError> bind_waiter(Source& src, TaskId id,
                                                 TargetId waiter,
                                                 uint16_t signal) noexcept
    {
        return src.set_awaiter(id, waiter, signal);
    }
};

}  // namespace coro
}  // namespace coact
