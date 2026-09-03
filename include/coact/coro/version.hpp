// coact::coro version constants and ABI-level contract checks.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace coact {
namespace coro {

// Library version. Bump kMinor on additive changes, kMajor on any break of
// the public handle semantics (Task/Promise/AwaitableRef lifetimes, TaskId
// packing, completion-event layout).
enum : uint32_t {
    kVersionMajor = 0U,
    kVersionMinor = 1U,
    kVersionPatch = 0U
};

inline constexpr uint32_t version() noexcept
{
    return (kVersionMajor << 16U) | (kVersionMinor << 8U) | kVersionPatch;
}

// Compile-time ABI guards evaluated wherever this header is included: the
// wire types must never silently change layout.
static_assert(sizeof(TaskId) == 2U, "coact::coro: TaskId ABI is 2 bytes");

}  // namespace coro
}  // namespace coact
