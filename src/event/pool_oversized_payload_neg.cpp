// Negative compile contract: this TU MUST NOT compile.
//
// A trivial payload that is larger than Layout::payload would overwrite the
// enclosing pool block. alloc_typed must reject it at compile time.
// SPDX-License-Identifier: MIT

#include <cstddef>
#include <cstdint>

#include "coact/pool.hpp"

namespace {

struct TestMeta {
    std::uint32_t value;
};

struct OversizedTrivialPayload {
    std::byte bytes[16U];
};

using Layout = coact::EventBlockLayout<TestMeta, 8U, 8U>;

}  // namespace

void use_oversized_payload()
{
    constexpr std::uint16_t kBlock = static_cast<std::uint16_t>(sizeof(Layout));
    coact::EventPool<kBlock, 4U> pool;
    Layout* layout = pool.alloc_typed<Layout, OversizedTrivialPayload, 8U>(1U);
    (void)layout;
}
