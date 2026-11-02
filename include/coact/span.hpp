// coact/span.hpp -- minimal C++17 non-owning view.
//
// std::span is C++20, but coact's baseline is C++17. This type carries the
// same contract for the small surface the framework and examples need: it
// refers to memory owned elsewhere, never allocates, and never frees. The
// caller must keep the referenced range alive for the view's lifetime.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

#include <type_traits>

namespace coact {

template <typename T>
class Span {
 public:
  using element_type = T;
  using value_type = typename std::remove_cv<T>::type;
  using pointer = T*;
  using reference = T&;
  using iterator = T*;
  using size_type = std::size_t;

  constexpr Span() noexcept = default;
  constexpr Span(pointer data, size_type size) noexcept : data_(data), size_(size) {}

  template <size_type N>
  constexpr Span(element_type (&array)[N]) noexcept : data_(array), size_(N) {}

  template <typename U, typename = std::enable_if_t<std::is_convertible<U (*)[], T (*)[]>::value>>
  constexpr Span(const Span<U>& other) noexcept : data_(other.data()), size_(other.size()) {}

  [[nodiscard]] constexpr pointer data() const noexcept { return data_; }
  [[nodiscard]] constexpr size_type size() const noexcept { return size_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return 0U == size_; }
  [[nodiscard]] constexpr pointer begin() const noexcept { return data_; }
  [[nodiscard]] constexpr pointer end() const noexcept { return data_ + size_; }
  [[nodiscard]] constexpr reference operator[](size_type index) const noexcept { return data_[index]; }

 private:
  pointer data_ = nullptr;
  size_type size_ = 0U;
};

using ByteSpan = Span<const std::uint8_t>;
using MutableByteSpan = Span<std::uint8_t>;

/* Byte view of one trivially copyable object. The view does not own `value`;
   the caller keeps it alive for the span's lifetime. */
template <typename T>
[[nodiscard]] ByteSpan as_bytes(const T& value) noexcept {
  static_assert(std::is_trivially_copyable<T>::value, "coact: as_bytes requires a trivially copyable type");
  return ByteSpan(reinterpret_cast<const std::uint8_t*>(&value), sizeof(T));
}

template <typename T>
[[nodiscard]] MutableByteSpan as_mutable_bytes(T& value) noexcept {
  static_assert(std::is_trivially_copyable<T>::value, "coact: as_mutable_bytes requires a trivially copyable type");
  return MutableByteSpan(reinterpret_cast<std::uint8_t*>(&value), sizeof(T));
}

}  // namespace coact
