// coact::coro fixed aligned storage: aligned std::byte arrays plus launder
// accessors for in-place construction of trivially destructible results.
// Only std::byte + alignas + std::launder are used - the only placement-new
// in the whole library is here and in coact::Expected (infrastructure).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

#include "coact/coro/config.hpp"

namespace coact {
namespace coro {
namespace detail {

// Fixed-capacity aligned byte storage for one T. T must satisfy
// is_coro_result_v (checked by the user, TaskSlot asserts it as well).
// Trivially destructible => no destructor call is ever needed on clear;
// resetting means zeroing the bytes (keeps sanitizers happy on re-use).
template <typename T>
struct FixedStorage final {
    static_assert(is_coro_result_v<T>,
                  "coact::coro: task result must be standard-layout, "
                  "trivially copyable, trivially destructible and nothrow "
                  "movable (pass large data by descriptor)");

    alignas(alignof(T)) std::byte bytes[sizeof(T)]{};

    // In-place construct from a value (memcpy semantics for trivially
    // copyable T; the placement-new form avoids reading uninitialized bytes
    // under MSan-style tools).
    void store(const T& value) noexcept
    {
        ::new (static_cast<void*>(bytes)) T(value);
    }

    // Typed view of the stored value. The value lifetime started in store().
    T* ptr() noexcept { return std::launder(reinterpret_cast<T*>(bytes)); }
    const T* ptr() const noexcept
    {
        return std::launder(reinterpret_cast<const T*>(bytes));
    }

    // Drop the value without running a destructor (T is trivially
    // destructible) and wipe the bytes so re-use never observes stale data.
    void clear() noexcept
    {
        for (size_t i = 0U; i < sizeof(bytes); ++i) {
            bytes[i] = std::byte{0};
        }
    }
};

// Compile-time rejection helper for tests: instantiating
// FixedStorageContract<T> evaluates is_coro_result_v<T> without asserting,
// so a test can CHECK(false == FixedStorageContract<Bad>::value).
template <typename T>
struct FixedStorageContract final {
    static constexpr bool value = is_coro_result_v<T>;
};

}  // namespace detail
}  // namespace coro
}  // namespace coact
