// coact::coro TaskRegistry: fixed-capacity task-slot registry over
// caller-owned storage. Create/complete/fail/cancel/release plus completion
// event posting through the coact EventPool / DispatchCoordinator pipeline.
// SPDX-License-Identifier: MIT
//
// Design (plan §3.1):
//   - TaskRegistry<T, PoolT, CoordinatorT, Capacity> owns a fixed
//     std::array<TaskSlot<T>, Capacity>: no heap, no hashing, lookup is a
//     bounded linear scan.
//   - TaskId packs (slot index, generation); release bumps the generation so
//     stale handles fail with kInvalidId instead of aliasing a recycled slot.
//   - Lifecycle: kCreated -> kWaiting -> kCompleted. Double completion and
//     cancel-after-complete are explicit rejections counted in
//     rejected_count(); they never overwrite stored state.
//   - Completion posts ONE typed event (CompletionBlock) to the registered
//     waiter TargetId. The result itself stays in the slot - only fixed-width
//     metadata travels (descriptor rule).
//
// Single-core model: the default execution plane is ONE thread. All coro
// components run on the coact Dispatcher's event plane (or the application's
// own single-threaded drive loop); no component ever spawns a thread. The
// std::mutex below is uncontended in that model and exists only to guard the
// optional posix.hpp escape hatch (a blocking-I/O worker thread completing
// tasks back into the registry).
//
// Thread-safety: a std::mutex guards the fixed-slot scan and state writes
// (same discipline as coact::TimerScheduler: the lock window is a bounded
// Capacity scan; atomicization is a follow-up optimization). All cross-AO
// interaction still flows through the coordinator - the registry never pokes
// an AO directly.
#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <type_traits>
#include <utility>

#include "coact/coro/config.hpp"
#include "coact/coro/detail/task_slot.hpp"
#include "coact/coro/error.hpp"
#include "coact/coro/task_id.hpp"
#include "coact/coordinator.hpp"
#include "coact/event.hpp"
#include "coact/expected.hpp"
#include "coact/pool.hpp"

namespace coact {
namespace coro {

template <typename T, typename RegistryT>
class Task;
template <typename T, typename RegistryT>
class Promise;
template <typename T, typename RegistryT>
class AwaitableRef;

template <typename T, typename PoolT, typename CoordinatorT,
          uint16_t Capacity = AsyncConfig::kDefaultMaxTasks>
class TaskRegistry final {
    static_assert(Capacity <= 256U,
                  "coact::coro: TaskId packs an 8-bit slot index: "
                  "Capacity <= 256");
    static_assert(Capacity > 0U, "coact::coro: Capacity must be non-zero");
    static_assert(is_coro_result_v<T>,
                  "coact::coro: task result must satisfy the coro result "
                  "contract (standard-layout, trivially copyable/destructible, "
                  "nothrow movable; pass large data by descriptor)");

public:
    using This = TaskRegistry;
    using ResultType = T;
    using TaskT = Task<T, This>;
    using PromiseT = Promise<T, This>;
    using AwaitableT = AwaitableRef<T, This>;

    // Creator-side pair: exactly one Task (consumer handle) + one Promise
    // (completer handle) per create() call.
    struct TaskPair {
        TaskT task;
        PromiseT promise;
    };

    TaskRegistry(PoolT& pool, CoordinatorT& coordinator) noexcept
        : pool_(pool), coordinator_(coordinator)
    {
    }

    TaskRegistry(const TaskRegistry&) = delete;
    TaskRegistry& operator=(const TaskRegistry&) = delete;
    TaskRegistry(TaskRegistry&&) = delete;
    TaskRegistry& operator=(TaskRegistry&&) = delete;

    static constexpr uint16_t capacity() noexcept { return Capacity; }

    // Register a new task. The completion event goes to `waiter` with
    // `signal` when the task completes; kInvalidTarget creates a
    // fire-and-poll task (no event). Returns kSlotsFull when exhausted.
    Expected<TaskPair, TaskError> create(TargetId waiter, uint16_t signal,
                                         const EventQos& qos) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint16_t i = 0U; i < Capacity; ++i) {
            if (detail::TaskSlotState::kFree == slots_[i].state) {
                slots_[i].state = detail::TaskSlotState::kCreated;
                slots_[i].waiter = waiter;
                slots_[i].waiter_signal = signal;
                slots_[i].qos = qos;
                const TaskId id =
                    TaskId::make(TaskSlotId(i), slots_[i].generation);
                TaskPair pair{TaskT(*this, id), PromiseT(*this, id)};
                return Expected<TaskPair, TaskError>::success(
                    std::move(pair));
            }
        }
        return Expected<TaskPair, TaskError>::error(TaskError::kSlotsFull);
    }

    // Complete with a result. Builds the result in place, then posts the
    // completion event. On event-pool exhaustion or coordinator rejection
    // the slot is released (task dead) and the submit failure counted.
    Expected<void, TaskError> complete(TaskId id, const T& value) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        detail::TaskSlot<T>* slot = find_locked(id);
        if (nullptr == slot) {
            return Expected<void, TaskError>::error(TaskError::kInvalidId);
        }
        if (detail::TaskSlotState::kCompleted == slot->state) {
            rejected_count_++;
            const TaskError err = (slot->has_error &&
                                   TaskError::kCancelled == slot->error)
                                      ? TaskError::kCancelled
                                      : TaskError::kAlreadyCompleted;
            return Expected<void, TaskError>::error(err);
        }
        slot->result.store(value);
        slot->has_result = true;
        slot->has_error = false;
        slot->state = detail::TaskSlotState::kCompleted;
        return post_completion_locked(id, *slot, CompletionStatus::kSucceeded,
                                      TaskError::kOk);
    }

    // Complete with a failure code (result stays absent).
    Expected<void, TaskError> fail(TaskId id, TaskError err) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        detail::TaskSlot<T>* slot = find_locked(id);
        if (nullptr == slot) {
            return Expected<void, TaskError>::error(TaskError::kInvalidId);
        }
        if (detail::TaskSlotState::kCompleted == slot->state) {
            rejected_count_++;
            return Expected<void, TaskError>::error(
                TaskError::kAlreadyCompleted);
        }
        slot->has_result = false;
        slot->has_error = true;
        slot->error = err;
        slot->state = detail::TaskSlotState::kCompleted;
        return post_completion_locked(id, *slot, CompletionStatus::kFailed,
                                      err);
    }

    // Cancel a pending task: the stored state becomes (kCancelled, no
    // result) and the waiter (if any) receives a kCancelled completion
    // event so it can observe the cancellation. Cancelling an already
    // completed task is a counted rejection.
    Expected<void, TaskError> cancel(TaskId id) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        detail::TaskSlot<T>* slot = find_locked(id);
        if (nullptr == slot) {
            return Expected<void, TaskError>::error(TaskError::kInvalidId);
        }
        if (detail::TaskSlotState::kCompleted == slot->state) {
            rejected_count_++;
            return Expected<void, TaskError>::error(
                TaskError::kAlreadyCompleted);
        }
        slot->has_result = false;
        slot->has_error = true;
        slot->error = TaskError::kCancelled;
        slot->state = detail::TaskSlotState::kCompleted;
        return post_completion_locked(id, *slot, CompletionStatus::kCancelled,
                                      TaskError::kCancelled);
    }

    // Register the waiter AO for a created (not yet waiting, not completed)
    // task. A second registration is a counted rejection (kAwaiterBusy);
    // registering on a completed task returns kAlreadyCompleted.
    Expected<void, TaskError> set_awaiter(TaskId id, TargetId waiter,
                                          uint16_t signal) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        detail::TaskSlot<T>* slot = find_locked(id);
        if (nullptr == slot) {
            return Expected<void, TaskError>::error(TaskError::kInvalidId);
        }
        if (detail::TaskSlotState::kWaiting == slot->state) {
            rejected_count_++;
            return Expected<void, TaskError>::error(TaskError::kAwaiterBusy);
        }
        if (detail::TaskSlotState::kCompleted == slot->state) {
            return Expected<void, TaskError>::error(
                TaskError::kAlreadyCompleted);
        }
        slot->waiter = waiter;
        slot->waiter_signal = signal;
        slot->state = detail::TaskSlotState::kWaiting;
        return Expected<void, TaskError>::success();
    }

    // Observers (AwaitableRef path).
    bool is_ready(TaskId id) const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const detail::TaskSlot<T>* slot = find_locked(id);
        return (nullptr != slot) &&
               (detail::TaskSlotState::kCompleted == slot->state);
    }

    bool has_error(TaskId id) const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const detail::TaskSlot<T>* slot = find_locked(id);
        return (nullptr != slot) && slot->has_error;
    }

    // Move the result out of a completed slot. One-shot: a second call
    // returns kResultTaken. Not-yet-completed -> kResultUnavailable.
    Expected<T, TaskError> take_result(TaskId id) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        detail::TaskSlot<T>* slot = find_locked(id);
        if (nullptr == slot) {
            return Expected<T, TaskError>::error(TaskError::kInvalidId);
        }
        if (detail::TaskSlotState::kCompleted != slot->state) {
            return Expected<T, TaskError>::error(TaskError::kResultUnavailable);
        }
        if (slot->has_error) {
            return Expected<T, TaskError>::error(slot->error);
        }
        if (!slot->has_result) {
            return Expected<T, TaskError>::error(TaskError::kResultTaken);
        }
        T value = *slot->result.ptr();
        slot->has_result = false;
        return Expected<T, TaskError>::success(value);
    }

    // Stored error of a completed task, or kOk when none. Awaiting callers
    // should check is_ready() first; kCancelled counts as the stored error.
    TaskError take_error(TaskId id) const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const detail::TaskSlot<T>* slot = find_locked(id);
        if ((nullptr != slot) && slot->has_error) {
            return slot->error;
        }
        return TaskError::kOk;
    }

    // Release a slot for reuse. Handles (Task/Promise/AwaitableRef holding
    // this TaskId) become stale: their next operation returns kInvalidId.
    Expected<void, TaskError> release_task(TaskId id) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        detail::TaskSlot<T>* slot = find_locked(id);
        if (nullptr == slot) {
            return Expected<void, TaskError>::error(TaskError::kInvalidId);
        }
        slot->release();
        return Expected<void, TaskError>::success();
    }

    // Monitoring counters (fixed counters, no formatting).
    uint16_t used() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint16_t count = 0U;
        for (uint16_t i = 0U; i < Capacity; ++i) {
            if (detail::TaskSlotState::kFree != slots_[i].state) {
                ++count;
            }
        }
        return count;
    }

    // Explicit rejections: double completion, cancel-after-complete,
    // duplicate set_awaiter.
    uint32_t rejected_count() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return rejected_count_;
    }

    // Completion events that could not be delivered (pool exhausted or the
    // coordinator rejected the submission). The slot was released in that
    // case: the task is dead, not silently retried.
    uint32_t submit_failures() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return submit_failures_;
    }

private:
    // Slot lookup with generation validation. Caller must hold mutex_.
    detail::TaskSlot<T>* find_locked(TaskId id) noexcept
    {
        if (kInvalidTaskId == id) {
            return nullptr;
        }
        const uint16_t index = id.slot().value;
        if (index >= Capacity) {
            return nullptr;
        }
        detail::TaskSlot<T>& slot = slots_[index];
        if (detail::TaskSlotState::kFree == slot.state) {
            return nullptr;
        }
        if (slot.generation != id.generation()) {
            return nullptr;  // stale handle from a released incarnation
        }
        return &slot;
    }

    const detail::TaskSlot<T>* find_locked(TaskId id) const noexcept
    {
        return const_cast<TaskRegistry*>(this)->find_locked(id);
    }

    // Post the completion event for a slot that just reached kCompleted.
    // Caller must hold mutex_. On any delivery failure the slot is released
    // and the failure counted (the coordinator already consumed the event
    // reference on rejection paths - it owns it from submit on).
    Expected<void, TaskError> post_completion_locked(
        TaskId id, detail::TaskSlot<T>& slot, CompletionStatus status,
        TaskError err) noexcept
    {
        if (kInvalidTarget == slot.waiter) {
            return Expected<void, TaskError>::success();
        }
        CompletionBlock* block =
            pool_.template alloc_typed<CompletionBlock, CompletionPayload,
                                       alignof(CompletionPayload)>(
                slot.waiter_signal);
        if (nullptr == block) {
            submit_failures_++;
            slot.release();
            return Expected<void, TaskError>::error(TaskError::kPoolExhausted);
        }
        block->meta.task_id = id.raw();
        block->meta.status = static_cast<uint8_t>(status);
        block->meta.reserved = 0U;
        block->meta.error_code = static_cast<uint32_t>(err);
        const SubmitResult result = coordinator_.submit_from_task(
            slot.waiter, &block->event, slot.qos);
        const bool delivered =
            (SubmitDisposition::Direct == result.disposition) ||
            (SubmitDisposition::Queued == result.disposition) ||
            (SubmitDisposition::Merged == result.disposition);
        if (!delivered) {
            submit_failures_++;
            slot.release();
            return Expected<void, TaskError>::error(
                TaskError::kTargetRejected);
        }
        return Expected<void, TaskError>::success();
    }

    PoolT& pool_;
    CoordinatorT& coordinator_;
    std::array<detail::TaskSlot<T>, Capacity> slots_{};

    mutable std::mutex mutex_;
    uint32_t rejected_count_ = 0U;
    uint32_t submit_failures_ = 0U;
};

}  // namespace coro
}  // namespace coact
