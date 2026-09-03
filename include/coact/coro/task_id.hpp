// coact::coro strong identity types: TaskId (generation-guarded) and
// TaskSlotId (raw slot index). Zero-overhead value wrappers in the
// coact::TargetId style.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "coact/coro/config.hpp"

namespace coact {
namespace coro {

// Raw slot index (0 .. Capacity-1). Never crosses an AO boundary by itself;
// TaskId is the wire identity.
struct TaskSlotId {
    uint16_t value = 0U;
    constexpr TaskSlotId() noexcept = default;
    constexpr explicit TaskSlotId(uint16_t raw) noexcept : value(raw) {}
    constexpr uint16_t raw() const noexcept { return value; }
    constexpr bool operator==(TaskSlotId o) const noexcept { return value == o.value; }
    constexpr bool operator!=(TaskSlotId o) const noexcept { return value != o.value; }
};

// Task identity handed to callers: slot index in the low bits, a generation
// counter in the high bits. The generation increments on every slot release,
// so a stale handle (from a released slot) can never alias a recycled task:
// lookup fails with kInvalidId instead of corrupting an unrelated task.
//
// Layout with AsyncConfig::kSlotGenerationBits == 8:
//   [15:8] generation (wraps after 256 releases per slot - callers must not
//                     hold a handle across 256 subsequent uses of that slot)
//   [7:0]  slot index (capacity <= 256)
// Capacities above 256 slots are rejected at compile time.
struct TaskId {
    uint16_t value = 0U;
    constexpr TaskId() noexcept = default;
    constexpr explicit TaskId(uint16_t raw) noexcept : value(raw) {}
    constexpr explicit operator bool() const noexcept { return 0U != value; }

    constexpr TaskSlotId slot() const noexcept
    {
        return TaskSlotId(static_cast<uint16_t>(
            value & static_cast<uint16_t>(0xFFU)));
    }

    constexpr uint16_t generation() const noexcept
    {
        return static_cast<uint16_t>(value >> 8U);
    }

    static constexpr TaskId make(TaskSlotId slot, uint16_t gen) noexcept
    {
        return TaskId(static_cast<uint16_t>(
            ((gen & 0xFFU) << 8U) | (slot.value & 0xFFU)));
    }

    constexpr uint16_t raw() const noexcept { return value; }
    constexpr bool operator==(TaskId o) const noexcept { return value == o.value; }
    constexpr bool operator!=(TaskId o) const noexcept { return value != o.value; }
};

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
