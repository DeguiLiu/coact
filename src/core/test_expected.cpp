// coact Expected lifetime tests.
// SPDX-License-Identifier: MIT
#include <cstdint>
#include <utility>

#include "coact/expected.hpp"
#include "test_harness.hpp"

namespace {

struct Tracked final {
    static uint32_t constructions;
    static uint32_t destructions;

    uint32_t value = 0U;

    explicit Tracked(uint32_t initial) noexcept : value(initial) {
        ++constructions;
    }

    Tracked(Tracked&& other) noexcept : value(std::exchange(other.value, 0U)) {
        ++constructions;
    }

    Tracked& operator=(Tracked&& other) noexcept
    {
        value = std::exchange(other.value, 0U);
        return *this;
    }

    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;

    ~Tracked() noexcept {
        ++destructions;
    }
};

uint32_t Tracked::constructions = 0U;
uint32_t Tracked::destructions = 0U;

void reset_tracked() noexcept
{
    Tracked::constructions = 0U;
    Tracked::destructions = 0U;
}

}  // namespace

COACT_TEST(expected_move_relinquishes_source_storage) {
    reset_tracked();
    {
        Tracked input(9U);
        auto source = coact::Expected<Tracked, uint8_t>::success(std::move(input));
        auto destination = std::move(source);

        CHECK(!source.has_value());
        CHECK(destination.has_value());
        CHECK_EQ(destination.value().value, 9U);
        CHECK_EQ(Tracked::constructions, 3U);
        CHECK_EQ(Tracked::destructions, 1U);
    }
    CHECK_EQ(Tracked::constructions, Tracked::destructions);
}

COACT_TEST(expected_move_assignment_relinquishes_source_storage) {
    reset_tracked();
    {
        Tracked first(1U);
        Tracked second(2U);
        auto destination = coact::Expected<Tracked, uint8_t>::success(std::move(first));
        auto source = coact::Expected<Tracked, uint8_t>::success(std::move(second));

        destination = std::move(source);

        CHECK(!source.has_value());
        CHECK(destination.has_value());
        CHECK_EQ(destination.value().value, 2U);
        CHECK_EQ(Tracked::constructions, 5U);
        CHECK_EQ(Tracked::destructions, 2U);
    }
    CHECK_EQ(Tracked::constructions, Tracked::destructions);
}

COACT_TEST_MAIN()
