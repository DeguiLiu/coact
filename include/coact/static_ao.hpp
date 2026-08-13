// coact static AO dispatch entry prototype.
// SPDX-License-Identifier: MIT
#pragma once

#include "coact/config.hpp"
#include "coact/event.hpp"

namespace coact {

// A small, non-owning entry for board-defined AO tables. The concrete AO is
// retained by static application storage; this entry only erases the dispatch
// call into a captureless function pointer. It deliberately does not replace
// AoBase or Runtime registration until target measurements prove a benefit.
struct StaticAoEntry {
    using DispatchFn = void (*)(void*, const Event&) noexcept;

    void* object = nullptr;
    DispatchFn dispatch = nullptr;
    LogicalPrio logical_prio = kInvalidPrio;
    PriorityClass priority_class = PriorityClass::Low;
    bool direct_eligible = false;
    bool isr_direct_safe = false;

    bool valid() const noexcept
    {
        return (object != nullptr) && (dispatch != nullptr)
            && (logical_prio != kInvalidPrio);
    }

    void dispatch_event(const Event& event) const noexcept
    {
        if (valid()) {
            dispatch(object, event);
        }
    }
};

template <typename AoT, typename Traits>
constexpr StaticAoEntry make_static_ao_entry(AoT& ao) noexcept
{
    return StaticAoEntry{
        &ao,
        [](void* object, const Event& event) noexcept {
            static_cast<AoT*>(object)->dispatch(event);
        },
        Traits::logical_prio(),
        Traits::priority_class(),
        Traits::direct_eligible(),
        Traits::isr_direct_safe()
    };
}

}  // namespace coact
