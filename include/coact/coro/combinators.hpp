// coact::coro combinators: when_all / when_any / when_some as fixed-
// capacity aggregates. One composite slot per group; one aggregate completion
// event when the condition is met.
// SPDX-License-Identifier: MIT
//
// Design (plan §3.3):
//   - A group works on a registry whose result type is GroupStatus: the
//     aggregate task completes once with the group status (success count /
//     failure count / first error) so combinators reuse the registry's event
//     machinery instead of a parallel one.
//   - The group tracks input slots (bounded linear scan over the input ids);
//     evaluation is driven by whoever observes an input completion. In the
//     single-core event plane the natural drive points are the AO action
//     that receives an input completion event and calls evaluate(), and the
//     creation path when inputs are already complete.
//   - completed_mask is a uint32_t bitmap: Capacity <= 32 (static_assert).
//   - First-error policy: a failure settles when_all (and when_any /
//     when_some only when ALL inputs failed); the group status carries the
//     first error and the failure count.
//   - No recursion, no dynamic containers: plain loops over fixed arrays.
//
// Single-core model: pure evaluation functions, no threads.
#pragma once

#include <array>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "coact/coro/config.hpp"
#include "coact/coro/error.hpp"
#include "coact/coro/task.hpp"
#include "coact/coro/task_id.hpp"
#include "coact/coro/task_registry.hpp"
#include "coact/config.hpp"
#include "coact/expected.hpp"

namespace coact {
namespace coro {

// Aggregate outcome of a settled TaskGroup.
struct GroupStatus {
    uint16_t succeeded;     // completed-with-result input count
    uint16_t failed;        // completed-with-error input count
    TaskError first_error;  // kOk unless at least one failure
};

// Group evaluation policy.
enum class GroupMode : uint8_t {
    kAll = 0U,
    kAny,
    kSome
};

// Forward declarations.
template <typename T, typename RegistryT, uint16_t Capacity>
class TaskGroup;
template <typename T, typename RegistryT, typename InputRegistryT,
          uint16_t Capacity>
class GroupBuilder;

// Registry type whose result is a GroupStatus (the aggregate slot type).
template <typename PoolT, typename CoordinatorT,
          uint16_t RegistryCapacity = AsyncConfig::kDefaultMaxTasks>
using GroupRegistry = TaskRegistry<GroupStatus, PoolT, CoordinatorT,
                                   RegistryCapacity>;

// Fixed-capacity group of input task ids plus its aggregate slot.
//
// Drive protocol (all on the single event-plane thread):
//   1. when_all()/when_any()/when_some() bind the inputs and the aggregate
//      task (one slot in the group registry).
//   2. The waiter AO receives input completion events; each action calls
//      evaluate(); the FIRST satisfied condition completes the aggregate
//      task (which posts ONE event to the group waiter).
//   3. evaluate() after settlement is a no-op (idempotent guard).
template <typename T, typename RegistryT, uint16_t Capacity>
class TaskGroup final {
    static_assert(Capacity <= 32U,
                  "coact::coro: TaskGroup bitmap is uint32_t: "
                  "Capacity <= 32");
    static_assert(Capacity > 0U, "coact::coro: Capacity must be non-zero");

public:
    TaskGroup() noexcept = default;

    TaskGroup(TaskGroup&& other) noexcept
        : inputs_(other.inputs_),
          group_id_(other.group_id_),
          waiter_(other.waiter_),
          waiter_signal_(other.waiter_signal_),
          completed_mask_(other.completed_mask_),
          success_count_(other.success_count_),
          failure_count_(other.failure_count_),
          first_error_(other.first_error_),
          settled_(other.settled_),
          live_count_(other.live_count_),
          promise_(std::move(other.promise_))
    {
        other.group_id_ = kInvalidTaskId;
        other.settled_ = false;
        other.live_count_ = 0U;
    }

    TaskGroup& operator=(TaskGroup&& other) noexcept
    {
        if (this != &other) {
            inputs_ = other.inputs_;
            group_id_ = other.group_id_;
            waiter_ = other.waiter_;
            waiter_signal_ = other.waiter_signal_;
            completed_mask_ = other.completed_mask_;
            success_count_ = other.success_count_;
            failure_count_ = other.failure_count_;
            first_error_ = other.first_error_;
            settled_ = other.settled_;
            live_count_ = other.live_count_;
            promise_ = std::move(other.promise_);
            other.group_id_ = kInvalidTaskId;
            other.settled_ = false;
            other.live_count_ = 0U;
        }
        return *this;
    }

    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;

    TaskId group_id() const noexcept { return group_id_; }
    bool is_valid() const noexcept { return static_cast<bool>(group_id_); }
    bool is_settled() const noexcept { return settled_; }

    GroupStatus status() const noexcept
    {
        return GroupStatus{success_count_, failure_count_, first_error_};
    }

    // Count of satisfied completions (successes + failures).
    uint16_t satisfied_count() const noexcept
    {
        return static_cast<uint16_t>(success_count_ + failure_count_);
    }

    // Release the aggregate slot (after the waiter consumed the result).
    Expected<void, TaskError> release(RegistryT& registry) noexcept
    {
        if (!is_valid()) {
            return Expected<void, TaskError>::error(TaskError::kInvalidId);
        }
        const Expected<void, TaskError> result =
            registry.release_task(group_id_);
        group_id_ = kInvalidTaskId;
        settled_ = false;
        return result;
    }

private:
    friend class GroupBuilder<T, RegistryT, RegistryT, Capacity>;
    template <typename T2, typename R2, typename IR2, uint16_t C2>
    friend class GroupBuilder;

    void set_group(TaskId id) noexcept { group_id_ = id; }
    void bind_input(uint16_t index, TaskId id) noexcept
    {
        inputs_[index] = id;
    }

    // Complete the aggregate task with the current status snapshot. One
    // event to the group waiter; the group registry's result type is T
    // (GroupStatus for the group registries used with combinators).
    Expected<bool, TaskError> settle(RegistryT& /*registry*/) noexcept
    {
        if (settled_ || !static_cast<bool>(group_id_) || !promise_.is_valid()) {
            return Expected<bool, TaskError>::success(false);
        }
        settled_ = true;
        /* The aggregate ALWAYS completes with the full GroupStatus snapshot:
           failures are DATA (first_error + counts), not a task-level error.
           A waiter that must react to failure reads first_error != kOk. */
        GroupStatus snapshot = status();
        const Expected<void, TaskError> done = promise_.complete(snapshot);
        if (!done) {
            return Expected<bool, TaskError>::error(done.error());
        }
        return Expected<bool, TaskError>::success(true);
    }

    std::array<TaskId, Capacity> inputs_{};
    TaskId group_id_ = kInvalidTaskId;
    TargetId waiter_ = kInvalidTarget;
    uint16_t waiter_signal_ = 0U;
    uint32_t completed_mask_ = 0U;
    uint16_t success_count_ = 0U;
    uint16_t failure_count_ = 0U;
    TaskError first_error_ = TaskError::kOk;
    bool settled_ = false;
    uint16_t live_count_ = 0U;
    Promise<T, RegistryT> promise_{};
};

// Builder: registers the aggregate task in the GROUP registry and polls the
// INPUT tasks in the input registry. `count` is the live input count
// (entries beyond count are ignored). Two registries: input tasks may live
// in any TaskRegistry instantiation; the aggregate slot always lives in a
// GroupRegistry (GroupStatus result).
template <typename T, typename RegistryT, typename InputRegistryT,
          uint16_t Capacity>
class GroupBuilder final {
public:
    using Group = TaskGroup<T, RegistryT, Capacity>;

    static Expected<Group, GroupError> make(RegistryT& registry,
                                            InputRegistryT& input_registry,
                                            const std::array<TaskId, Capacity>& inputs,
                                            uint16_t count, TargetId waiter,
                                            uint16_t signal) noexcept
    {
        /* The input registry is captured for symmetry with evaluate(); make()
           only validates ids (slot presence is checked lazily at evaluate). */
        (void)input_registry;
        if (0U == count) {
            return Expected<Group, GroupError>::error(GroupError::kZeroCount);
        }
        if (count > Capacity) {
            return Expected<Group, GroupError>::error(
                GroupError::kTooManyInputs);
        }
        auto created = registry.create(waiter, signal,
                                       EventQos{false, false});
        if (!created) {
            return Expected<Group, GroupError>::error(
                GroupError::kRegistryFull);
        }
        Group group;
        group.set_group(created.value().task.id());
        for (uint16_t i = 0U; i < count; ++i) {
            if (kInvalidTaskId == inputs[i]) {
                (void)registry.release_task(created.value().task.id());
                return Expected<Group, GroupError>::error(
                    GroupError::kInvalidTask);
            }
            group.bind_input(i, inputs[i]);
        }
        group.live_count_ = count;
        group.promise_ = std::move(created.value().promise);
        return Expected<Group, GroupError>::success(std::move(group));
    }

    // Evaluate the group after an input completion observation. Returns
    // true when the group just satisfied its condition (aggregate completed
    // with one event); after settlement further calls are idempotent.
    static Expected<bool, GroupError> evaluate(RegistryT& registry,
                                               InputRegistryT& input_registry,
                                               Group& group, GroupMode mode,
                                               uint16_t threshold) noexcept
    {
        if (!group.is_valid()) {
            return Expected<bool, GroupError>::error(GroupError::kInvalidTask);
        }
        if (group.settled_) {
            return Expected<bool, GroupError>::success(true);
        }
        for (uint16_t i = 0U; i < group.live_count_; ++i) {
            const uint32_t bit = (1U << i);
            if (0U != (group.completed_mask_ & bit)) {
                continue;
            }
            if (!input_registry.is_ready(group.inputs_[i])) {
                continue;
            }
            group.completed_mask_ |= bit;
            if (input_registry.has_error(group.inputs_[i])) {
                group.failure_count_++;
                if (TaskError::kOk == group.first_error_) {
                    group.first_error_ = input_registry.take_error(
                        group.inputs_[i]);
                }
            } else {
                group.success_count_++;
            }
        }

        bool satisfied = false;
        switch (mode) {
        case GroupMode::kAll:
            satisfied = (group.satisfied_count() == group.live_count_);
            break;
        case GroupMode::kAny:
            satisfied = (0U != group.satisfied_count());
            break;
        case GroupMode::kSome:
            satisfied = (group.satisfied_count() >= threshold);
            break;
        default:
            satisfied = false;
            break;
        }

        if (satisfied) {
            const Expected<bool, TaskError> settled =
                group.settle(registry);
            if (!settled) {
                return Expected<bool, GroupError>::error(
                    GroupError::kRegistryFull);
            }
            return Expected<bool, GroupError>::success(true);
        }
        return Expected<bool, GroupError>::success(false);
    }

private:
    GroupBuilder() = delete;
};

// -------------------------------------------------------------------------
// Convenience entry points over a GroupRegistry (aggregate result type is
// GroupStatus). Both registries are passed: the INPUT registry owns the
// input task slots; the GROUP registry owns the aggregate slot. The policy
// is chosen at evaluate() time via the evaluate_* wrappers.
// -------------------------------------------------------------------------

template <typename PoolT, typename CoordinatorT, uint16_t Capacity,
          uint16_t RegistryCapacity = AsyncConfig::kDefaultMaxTasks,
          typename InputRegistryT>
Expected<TaskGroup<GroupStatus,
                   GroupRegistry<PoolT, CoordinatorT, RegistryCapacity>,
                   Capacity>,
         GroupError>
make_task_group(
    GroupRegistry<PoolT, CoordinatorT, RegistryCapacity>& registry,
    InputRegistryT& input_registry,
    const std::array<TaskId, Capacity>& inputs, uint16_t count,
    TargetId waiter, uint16_t signal) noexcept
{
    return GroupBuilder<
        GroupStatus, GroupRegistry<PoolT, CoordinatorT, RegistryCapacity>,
        InputRegistryT, Capacity>::make(registry, input_registry, inputs,
                                        count, waiter, signal);
}

// when_all: settle when every input completed (a failure settles the group
// as failed - first error wins).
template <typename PoolT, typename CoordinatorT, uint16_t Capacity,
          uint16_t RegistryCapacity = AsyncConfig::kDefaultMaxTasks,
          typename InputRegistryT>
Expected<TaskGroup<GroupStatus,
                   GroupRegistry<PoolT, CoordinatorT, RegistryCapacity>,
                   Capacity>,
         GroupError>
when_all(GroupRegistry<PoolT, CoordinatorT, RegistryCapacity>& registry,
         InputRegistryT& input_registry,
         const std::array<TaskId, Capacity>& inputs, uint16_t count,
         TargetId waiter, uint16_t signal) noexcept
{
    return make_task_group<PoolT, CoordinatorT, Capacity, RegistryCapacity,
                           InputRegistryT>(registry, input_registry, inputs,
                                           count, waiter, signal);
}

// when_any: settle on the first input completion (success or failure).
template <typename PoolT, typename CoordinatorT, uint16_t Capacity,
          uint16_t RegistryCapacity = AsyncConfig::kDefaultMaxTasks,
          typename InputRegistryT>
Expected<TaskGroup<GroupStatus,
                   GroupRegistry<PoolT, CoordinatorT, RegistryCapacity>,
                   Capacity>,
         GroupError>
when_any(GroupRegistry<PoolT, CoordinatorT, RegistryCapacity>& registry,
         InputRegistryT& input_registry,
         const std::array<TaskId, Capacity>& inputs, uint16_t count,
         TargetId waiter, uint16_t signal) noexcept
{
    return make_task_group<PoolT, CoordinatorT, Capacity, RegistryCapacity,
                           InputRegistryT>(registry, input_registry, inputs,
                                           count, waiter, signal);
}

// when_some: settle when `threshold` inputs completed.
template <typename PoolT, typename CoordinatorT, uint16_t Capacity,
          uint16_t RegistryCapacity = AsyncConfig::kDefaultMaxTasks,
          typename InputRegistryT>
Expected<TaskGroup<GroupStatus,
                   GroupRegistry<PoolT, CoordinatorT, RegistryCapacity>,
                   Capacity>,
         GroupError>
when_some(GroupRegistry<PoolT, CoordinatorT, RegistryCapacity>& registry,
          InputRegistryT& input_registry,
          const std::array<TaskId, Capacity>& inputs, uint16_t count,
          uint16_t threshold, TargetId waiter, uint16_t signal) noexcept
{
    /* threshold is consumed at evaluate_some(); recorded here for API
       parity with the plan signature. */
    (void)threshold;
    return make_task_group<PoolT, CoordinatorT, Capacity, RegistryCapacity,
                           InputRegistryT>(registry, input_registry, inputs,
                                           count, waiter, signal);
}

// Evaluate wrappers: the policy selected at the call site. The unused
// threshold parameters keep the plan's API shape.
template <typename PoolT, typename CoordinatorT, uint16_t Capacity,
          uint16_t RegistryCapacity = AsyncConfig::kDefaultMaxTasks,
          typename InputRegistryT>
Expected<bool, GroupError>
evaluate_all(GroupRegistry<PoolT, CoordinatorT, RegistryCapacity>& registry,
             InputRegistryT& input_registry,
             TaskGroup<GroupStatus,
                       GroupRegistry<PoolT, CoordinatorT, RegistryCapacity>,
                       Capacity>& group) noexcept
{
    return GroupBuilder<
        GroupStatus, GroupRegistry<PoolT, CoordinatorT, RegistryCapacity>,
        InputRegistryT, Capacity>::evaluate(registry, input_registry, group,
                                            GroupMode::kAll, 0U);
}

template <typename PoolT, typename CoordinatorT, uint16_t Capacity,
          uint16_t RegistryCapacity = AsyncConfig::kDefaultMaxTasks,
          typename InputRegistryT>
Expected<bool, GroupError>
evaluate_any(GroupRegistry<PoolT, CoordinatorT, RegistryCapacity>& registry,
             InputRegistryT& input_registry,
             TaskGroup<GroupStatus,
                       GroupRegistry<PoolT, CoordinatorT, RegistryCapacity>,
                       Capacity>& group) noexcept
{
    return GroupBuilder<
        GroupStatus, GroupRegistry<PoolT, CoordinatorT, RegistryCapacity>,
        InputRegistryT, Capacity>::evaluate(registry, input_registry, group,
                                            GroupMode::kAny, 0U);
}

template <typename PoolT, typename CoordinatorT, uint16_t Capacity,
          uint16_t RegistryCapacity = AsyncConfig::kDefaultMaxTasks,
          typename InputRegistryT>
Expected<bool, GroupError>
evaluate_some(GroupRegistry<PoolT, CoordinatorT, RegistryCapacity>& registry,
              InputRegistryT& input_registry,
              TaskGroup<GroupStatus,
                        GroupRegistry<PoolT, CoordinatorT, RegistryCapacity>,
                        Capacity>& group, uint16_t threshold) noexcept
{
    return GroupBuilder<
        GroupStatus, GroupRegistry<PoolT, CoordinatorT, RegistryCapacity>,
        InputRegistryT, Capacity>::evaluate(registry, input_registry, group,
                                            GroupMode::kSome, threshold);
}

}  // namespace coro
}  // namespace coact
