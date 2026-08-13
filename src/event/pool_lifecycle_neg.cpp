// Negative compile contract: this TU MUST NOT compile.
//
// It drives the typed allocation with a non-trivial payload. Because the event
// pool has no payload destroy hook (design §7.2), the EventPool::alloc_typed
// static_assert must reject it at compile time.
//
// The CMake target is EXCLUDE_FROM_ALL (never part of the default build) and is
// wired as a WILL_FAIL ctest: the test command builds this target and expects
// that build to FAIL. If this file ever compiles, the static_assert has been
// broken and the test suite fails.
// SPDX-License-Identifier: MIT

#include <cstddef>
#include <cstdint>

#include "coact/pool.hpp"

namespace {

struct TestMeta {
    std::uint32_t request_id;
    std::uint32_t deadline_tick;
    std::uint16_t descriptor_index;
    std::uint16_t payload_size;
};

using Layout = coact::EventBlockLayout<TestMeta, 64, 8>;

// Non-trivially-destructible payload: forbidden in the pool.
struct NonTrivialPayload {
    std::uint32_t value;
    ~NonTrivialPayload() {}
};

}  // namespace

void use_forbidden_payload()
{
    constexpr std::uint16_t kBlock = static_cast<std::uint16_t>(sizeof(Layout));
    coact::EventPool<kBlock, 4U> pool;
    Layout* l = pool.alloc_typed<Layout, NonTrivialPayload, 8>(1U);
    (void)l;
}
