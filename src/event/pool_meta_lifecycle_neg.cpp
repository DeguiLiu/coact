// Negative compile contract: this TU MUST NOT compile.
//
// EventPool does not destroy application metadata on reclaim, so alloc_typed
// must reject layouts whose metadata is not trivially poolable.
// SPDX-License-Identifier: MIT

#include <cstddef>
#include <cstdint>

#include "coact/pool.hpp"

namespace {

struct NonTrivialMeta {
    std::uint32_t value;
    ~NonTrivialMeta() {}
};

struct TestPayload {
    std::uint32_t value;
};

using Layout = coact::EventBlockLayout<NonTrivialMeta, 64U, 8U>;

}  // namespace

void use_forbidden_meta()
{
    constexpr std::uint16_t kBlock = static_cast<std::uint16_t>(sizeof(Layout));
    coact::EventPool<kBlock, 4U> pool;
    Layout* layout = pool.alloc_typed<Layout, TestPayload, 8U>(1U);
    (void)layout;
}
