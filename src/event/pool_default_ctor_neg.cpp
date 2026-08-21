// Negative compile contract: this TU MUST NOT compile.
//
// Value-initialized pool layouts and payloads must have noexcept default
// constructors because EventPool is built without exception support.
// SPDX-License-Identifier: MIT

#include <cstddef>
#include <cstdint>

#include "coact/pool.hpp"

namespace {

struct TestMeta {
    std::uint32_t value;
};

struct TestPayload {
    std::uint32_t value;
};

struct ThrowingDefaultPayload {
    std::uint32_t value;
    ThrowingDefaultPayload() : value(0U) {}
};

using TrivialLayout = coact::EventBlockLayout<TestMeta, 16U, 8U>;

struct ThrowingDefaultLayout {
    coact::Event event;
    TestMeta meta;
    alignas(8U) std::byte payload[16U];

    ThrowingDefaultLayout() : event{}, meta{}, payload{} {}
};

}  // namespace

void use_throwing_default_constructor()
{
#if defined(COACT_TEST_LAYOUT_DEFAULT_CTOR)
    using Layout = ThrowingDefaultLayout;
    using Payload = TestPayload;
#else
    using Layout = TrivialLayout;
    using Payload = ThrowingDefaultPayload;
#endif
    constexpr std::uint16_t kBlock = static_cast<std::uint16_t>(sizeof(Layout));
    coact::EventPool<kBlock, 4U> pool;
    Layout* layout = pool.alloc_typed<Layout, Payload, 8U>(1U);
    (void)layout;
}
