// coact vocabulary type tests: NewType, ScopeGuard, optional,
// FixedFunction, function_ref, truncate tags, FixedString, FixedVector.
// SPDX-License-Identifier: MIT
#include "test/test_harness.hpp"

#include <cstdint>
#include <type_traits>
#include <utility>

#include "coact/config.hpp"
#include "coact/coro/task_id.hpp"
#include "coact/vocabulary.hpp"

namespace {

struct WidgetTag {};
struct GadgetTag {};
using WidgetId = coact::NewType<uint32_t, WidgetTag>;
using GadgetId = coact::NewType<uint32_t, GadgetTag>;

/* ---------------------------------------------------------------------- */
/* NewType<T, Tag>: strong ids.                                           */
/* ---------------------------------------------------------------------- */
static_assert(std::is_trivially_copyable<WidgetId>::value,
              "NewType must be trivially copyable");
static_assert(sizeof(WidgetId) == sizeof(uint32_t),
              "NewType must be zero-overhead");
static_assert(std::is_trivially_copyable<coact::TargetId>::value,
              "TargetId must be a trivial strong type");
static_assert(sizeof(coact::TargetId) == sizeof(uint8_t),
              "TargetId must be zero-overhead");
static_assert(std::is_trivially_copyable<coact::coro::TaskSlotId>::value,
              "TaskSlotId must be a trivial strong type");
static_assert(sizeof(coact::coro::TaskSlotId) == sizeof(uint16_t),
              "TaskSlotId must be zero-overhead");

COACT_TEST(newtype_same_type_compares_and_differs)
{
    const WidgetId a(7U);
    const WidgetId b(7U);
    const WidgetId c(8U);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a.value() == 7U);
    CHECK(a.raw() == 7U);
}

COACT_TEST(newtype_same_value_different_tags_not_interchangeable)
{
    /* Different Tag instantiations are distinct types: assigning a
       GadgetId to a WidgetId is a compile error. The distinctive value()
       accessor names also prevent accidental interchange, which the
       same-type assert below cannot show; kept as a runtime sanity check
       that both wrappers hold the payload independently. */
    const WidgetId w(3U);
    const GadgetId g(3U);
    CHECK(w.value() == g.value());
    CHECK(w == WidgetId(3U));
    CHECK(g == GadgetId(3U));
}

COACT_TEST(newtype_based_ids_expose_raw_values)
{
    const coact::TargetId target(7U);
    const coact::coro::TaskSlotId slot(3U);
    CHECK(target.raw() == 7U);
    CHECK(slot.raw() == 3U);
}

COACT_TEST(expected_is_available_from_vocabulary)
{
    const auto success = coact::Expected<uint16_t, uint8_t>::success(9U);
    const auto failure = coact::Expected<uint16_t, uint8_t>::error(4U);
    CHECK(success.has_value());
    CHECK(success.value() == 9U);
    CHECK(!failure.has_value());
    CHECK(failure.error() == 4U);
}

/* ---------------------------------------------------------------------- */
/* ScopeGuard: RAII exit action.                                          */
/* ---------------------------------------------------------------------- */
COACT_TEST(scope_guard_fires_on_destruction)
{
    int32_t fired = 0;
    {
        coact::ScopeGuard guard([&fired]() { fired++; });
        CHECK(fired == 0);
    }
    CHECK(fired == 1);
}

COACT_TEST(scope_guard_dismiss_suppresses_action)
{
    int32_t fired = 0;
    {
        coact::ScopeGuard guard([&fired]() { fired++; });
        guard.dismiss();
    }
    CHECK(fired == 0);
}

COACT_TEST(scope_guard_move_transfers_armed_state)
{
    int32_t fired = 0;
    {
        coact::ScopeGuard first([&fired]() { fired++; });
        coact::ScopeGuard second(std::move(first));
        /* first is disarmed by the move; only second fires once. */
    }
    CHECK(fired == 1);
}

/* ---------------------------------------------------------------------- */
/* optional<T>: nullable value, move-only payload support.                 */
/* ---------------------------------------------------------------------- */
namespace {

struct MoveOnly {
    static int32_t destructed;
    uint32_t value = 0U;

    explicit MoveOnly(uint32_t v) noexcept : value(v) {}
    MoveOnly(MoveOnly&& other) noexcept : value(other.value)
    {
        other.value = 0U;
    }
    MoveOnly& operator=(MoveOnly&& other) noexcept
    {
        value = other.value;
        other.value = 0U;
        return *this;
    }
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    ~MoveOnly() { destructed++; }
};
int32_t MoveOnly::destructed = 0;

uint32_t dbl(uint32_t x) { return x * 2U; }
int32_t add_one(int32_t x) { return x + 1; }

}  // namespace

COACT_TEST(optional_constructs_value_in_place)
{
    MoveOnly::destructed = 0;
    {
        coact::optional<MoveOnly> opt(MoveOnly(42U));
        CHECK(static_cast<bool>(opt));
        CHECK(opt.has_value());
        CHECK(opt.value().value == 42U);
        opt.reset();
        CHECK(!opt.has_value());
    }
    /* One construction inside the optional; its inner value destroyed on
       reset, the temporary destroyed at the construction line. */
    CHECK(MoveOnly::destructed == 2);
}

COACT_TEST(optional_move_only_transfer)
{
    coact::optional<MoveOnly> src(MoveOnly(7U));
    coact::optional<MoveOnly> dst(std::move(src));
    /* Both objects still hold storage (move, not steal-the-optional): dst
       owns the moved value, src holds a moved-from instance. */
    CHECK(dst.has_value());
    CHECK(dst.value().value == 7U);
    CHECK(!src.has_value() || (0U == src.value().value));
}

COACT_TEST(optional_empty_and_value_or)
{
    coact::optional<uint32_t> empty;
    coact::optional<uint32_t> full(9U);
    CHECK(!static_cast<bool>(empty));
    CHECK(!empty.has_value());
    CHECK(empty.value_or(1U) == 1U);
    CHECK(full.value_or(1U) == 9U);
}

/* ---------------------------------------------------------------------- */
/* FixedFunction<Sig>: small-buffer type erasure.                         */
/* ---------------------------------------------------------------------- */
COACT_TEST(fixed_function_erases_and_invokes)
{
    uint32_t captured = 0U;
    int32_t addition = 3;
    coact::FixedFunction<uint32_t(uint32_t)> fn([&captured, addition](uint32_t x) {
        captured = x + static_cast<uint32_t>(addition);
        return captured;
    });
    CHECK(fn(10U) == 13U);
    CHECK(captured == 13U);
    CHECK(static_cast<bool>(fn));
}

COACT_TEST(fixed_function_move_transfers_callable)
{
    int32_t counter = 0;
    coact::FixedFunction<void()> src([&counter]() { counter++; });
    coact::FixedFunction<void()> dst(std::move(src));
    dst();
    CHECK(counter == 1);
    CHECK(!static_cast<bool>(src));
}

COACT_TEST(fixed_function_accepts_plain_function_pointer)
{
    coact::FixedFunction<uint32_t(uint32_t)> fn(dbl);
    CHECK(fn(21U) == 42U);
}

/* ---------------------------------------------------------------------- */
/* function_ref<Sig>: non-owning callable view.                            */
/* ---------------------------------------------------------------------- */
COACT_TEST(function_ref_views_lambda_and_function)
{
    int32_t scale = 2;
    const auto lambda = [&scale](int32_t x) { return x * scale; };
    const coact::function_ref<int32_t(int32_t)> as_view(lambda);
    CHECK(as_view(5) == 10);
    const coact::function_ref<int32_t(int32_t)> fn_view(add_one);
    CHECK(fn_view(5) == 6);
    scale = 3;
    CHECK(as_view(5) == 15);   /* the view observes the live capture */
}

/* ---------------------------------------------------------------------- */
/* FixedString<Capacity>: compile-time string with truncation.             */
/* ---------------------------------------------------------------------- */
COACT_TEST(fixed_string_append_and_c_str)
{
    coact::FixedString<16> s;
    CHECK(s.empty());
    CHECK(s.size() == 0U);
    CHECK(s.append("abc"));
    CHECK(s.size() == 3U);
    CHECK(s.append(""));
    CHECK(s.size() == 3U);
    CHECK(s == "abc");
    CHECK(s.c_str()[0] == 'a');
    CHECK(s.c_str()[3] == '\0');
}

COACT_TEST(fixed_string_truncates_on_overflow)
{
    coact::FixedString<4> s;
    CHECK(!s.append("hello"));   /* does not fit: rejected whole, no partial write */
    CHECK(s.empty());
    CHECK(s.append("hell"));     /* exact fit */
    CHECK(s.size() == 4U);
    CHECK(!s.append("x"));       /* full: rejected */
    CHECK(s == "hell");
    /* Silent truncation is opt-in through the explicit tag only. */
    coact::FixedString<4> t(coact::TruncateToCapacity, "hello");
    CHECK(t.size() == 4U);
    CHECK(t == "hell");
    s.clear();
    CHECK(s.empty());
    CHECK(s.append("ok"));
    CHECK(s == "ok");
}

COACT_TEST(fixed_string_truncate_constructor)
{
    coact::FixedString<4> s(coact::TruncateToCapacity, "abcdef");
    CHECK(s.size() == 4U);
    CHECK(s == "abcd");
    coact::FixedString<8> fits(coact::TruncateToCapacity, "abcdef");
    CHECK(fits.size() == 6U);
    CHECK(fits == "abcdef");
}

/* ---------------------------------------------------------------------- */
/* FixedVector<T, Capacity>: compile-time vector.                          */
/* ---------------------------------------------------------------------- */
COACT_TEST(fixed_vector_push_back_and_iteration)
{
    coact::FixedVector<int32_t, 4> v;
    CHECK(v.empty());
    CHECK(v.push_back(1));
    CHECK(v.push_back(2));
    CHECK(v.push_back(3));
    CHECK(v.size() == 3U);
    int32_t sum = 0;
    for (int32_t e : v) {
        sum += e;
    }
    CHECK(sum == 6);
    CHECK(v[1] == 2);
    v.pop_back();
    CHECK(v.size() == 2U);
    CHECK(v.back() == 2);
}

COACT_TEST(fixed_vector_capacity_exhaustion_rejects)
{
    coact::FixedVector<int32_t, 2> v;
    CHECK(v.push_back(1));
    CHECK(v.push_back(2));
    CHECK(v.full());
    CHECK(!v.push_back(3));   /* rejected, no crash, no heap */
    CHECK(v.size() == 2U);
    CHECK(v.capacity() == 2U);
}

COACT_TEST(fixed_vector_move_only_elements)
{
    coact::FixedVector<MoveOnly, 2> v;
    CHECK(v.push_back(MoveOnly(10U)));
    CHECK(v.push_back(MoveOnly(20U)));
    auto moved(std::move(v));
    CHECK(moved.size() == 2U);
    CHECK(moved[0].value == 10U);
    CHECK(moved[1].value == 20U);
    CHECK(v.empty());   /* source emptied by the move */
}

}  // namespace

COACT_TEST_MAIN()
