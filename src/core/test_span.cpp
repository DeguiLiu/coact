// coact Span test: C++17 non-owning view over byte buffers and objects.
// SPDX-License-Identifier: MIT
#include "coact/span.hpp"
#include "test/test_harness.hpp"

#include <cstdint>

#include <type_traits>

namespace {

struct Sample {
  std::uint16_t first;
  std::uint16_t second;
};

}  // namespace

COACT_TEST(span_constructs_iterates_and_converts) {
  std::uint8_t bytes[4] = {1U, 2U, 3U, 4U};
  coact::MutableByteSpan mutable_view(bytes);
  CHECK_EQ(mutable_view.size(), static_cast<std::size_t>(4U));
  CHECK(!mutable_view.empty());
  CHECK(mutable_view.data() == bytes);
  CHECK_EQ(mutable_view[2], static_cast<std::uint8_t>(3U));

  std::uint32_t sum = 0U;
  for (const std::uint8_t byte : mutable_view) {
    sum += byte;
  }
  CHECK_EQ(sum, static_cast<std::uint32_t>(10U));

  const coact::ByteSpan read_only = mutable_view;
  CHECK_EQ(read_only.size(), static_cast<std::size_t>(4U));
  CHECK(read_only.data() == bytes);

  const coact::ByteSpan empty{};
  CHECK(empty.empty());
  CHECK_EQ(empty.size(), static_cast<std::size_t>(0U));
}

COACT_TEST(span_as_bytes_views_trivially_copyable_object) {
  Sample sample{0x1234U, 0x5678U};
  const coact::ByteSpan view = coact::as_bytes(sample);
  CHECK_EQ(view.size(), sizeof(Sample));
  CHECK(view.data() == reinterpret_cast<const std::uint8_t*>(&sample));
  CHECK_EQ(view[0], reinterpret_cast<const std::uint8_t*>(&sample)[0]);

  coact::MutableByteSpan writable = coact::as_mutable_bytes(sample);
  CHECK_EQ(writable.size(), sizeof(Sample));
  CHECK(writable.data() == reinterpret_cast<std::uint8_t*>(&sample));
}

static_assert(std::is_trivially_copyable<coact::ByteSpan>::value, "coact: ByteSpan must be trivially copyable");
static_assert(std::is_trivially_copyable<coact::MutableByteSpan>::value,
              "coact: MutableByteSpan must be trivially copyable");

COACT_TEST_MAIN()
