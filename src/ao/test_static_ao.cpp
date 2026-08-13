// coact static AO entry prototype tests.
// SPDX-License-Identifier: MIT
#include <cstdint>

#include "coact/static_ao.hpp"
#include "test/test_harness.hpp"

namespace {

struct ProbeAo final {
    uint32_t calls = 0U;
    uint16_t last_signal = 0U;

    void dispatch(const coact::Event& event) noexcept
    {
        ++calls;
        last_signal = event.signal;
    }
};

struct ProbeTraits final {
    static constexpr coact::LogicalPrio logical_prio() noexcept { return 31U; }
    static constexpr coact::PriorityClass priority_class() noexcept
    {
        return coact::PriorityClass::High;
    }
    static constexpr bool direct_eligible() noexcept { return true; }
    static constexpr bool isr_direct_safe() noexcept { return false; }
};

}  // namespace

COACT_TEST(static_ao_entry_dispatches_concrete_object) {
    ProbeAo ao{};
    const auto entry = coact::make_static_ao_entry<ProbeAo, ProbeTraits>(ao);
    const coact::Event event{77U, 0U, 0U};

    CHECK(entry.valid());
    CHECK_EQ(entry.logical_prio, 31U);
    CHECK(entry.priority_class == coact::PriorityClass::High);
    CHECK(entry.direct_eligible);
    CHECK(!entry.isr_direct_safe);

    entry.dispatch_event(event);
    CHECK_EQ(ao.calls, 1U);
    CHECK_EQ(ao.last_signal, 77U);
}

COACT_TEST_MAIN()
