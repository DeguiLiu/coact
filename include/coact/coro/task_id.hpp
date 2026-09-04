// coact::coro strong identity types: TaskId (generation-guarded) and
// TaskSlotId (raw slot index). Zero-overhead value wrappers: TaskId is a
// NewType alias so every id in the codebase shares one strong-typing idiom.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <type_traits>

#include "coact/coro/config.hpp"
#include "coact/vocabulary.hpp"

namespace coact {
namespace coro {

// Raw slot index (0 .. Capacity-1). Never crosses an AO boundary by itself;
// TaskId is the wire identity.
struct TaskSlotIdTag {};
using TaskSlotId = coact::NewType<uint16_t, TaskSlotIdTag>;

// Task identity handed to callers: slot index in the low bits, a generation
// counter in the high bits. The generation increments on every slot release,
// so a stale handle (from a released slot) can never alias a recycled task:
// lookup fails with kInvalidId instead of corrupting an unrelated task.
//
// Layout with AsyncConfig::kSlotGenerationBits == 8:
//   [15:8] generation (wraps after 256 releases per slot - callers must not
//                     hold a handle across 256 subsequent uses of that slot)
//   [7:0]  slot index (capacity <= 256)
// Capacities above 256 slots are rejected at compile time. Packing and
// unpacking live in the free functions below (make_task_id / slot_of /
// generation_of) because NewType is a plain strong wrapper.
struct TaskIdTag {};
using TaskId = coact::NewType<uint16_t, TaskIdTag>;

// Pack (slot index, generation) into the wire identity.
constexpr TaskId make_task_id(TaskSlotId slot, uint16_t gen) noexcept
{
    return TaskId(static_cast<uint16_t>(
        ((gen & 0xFFU) << 8U) | (slot.raw() & 0xFFU)));
}

// Slot index half of a TaskId (low 8 bits).
constexpr TaskSlotId slot_of(TaskId id) noexcept
{
    return TaskSlotId(static_cast<uint16_t>(id.value() & 0xFFU));
}

// Generation half of a TaskId (high 8 bits).
constexpr uint16_t generation_of(TaskId id) noexcept
{
    return static_cast<uint16_t>(id.value() >> 8U);
}

inline constexpr TaskId kInvalidTaskId(0U);

static_assert(sizeof(TaskId) == sizeof(uint16_t),
              "coact::coro: TaskId must be zero-overhead");
static_assert(std::is_trivially_copyable<TaskId>::value,
              "coact::coro: TaskId must be trivially copyable");

// Slot-generation packing depends on the bit layout above.
static_assert(AsyncConfig::kSlotGenerationBits == 8U,
              "coact::coro: TaskId packing is fixed at 8/8 bits");

}  // namespace coro
}  // namespace coact
