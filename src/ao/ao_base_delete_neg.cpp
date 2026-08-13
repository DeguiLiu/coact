// Negative compile contract: this TU MUST NOT compile.
//
// It drives `delete` through the type-erased base AoBase*. Ao objects live at
// automatic/static storage duration and the AoRegistry / Runtime hold only
// non-owning AoBase* (never delete them). AoBase therefore exposes a protected,
// non-virtual destructor: deleting through a base pointer is a contract
// violation and MUST be rejected at compile time.
//
// The CMake target is EXCLUDE_FROM_ALL (never part of the default build) and is
// wired as a WILL_FAIL ctest: the test command builds this target and expects
// that build to FAIL. If this file ever compiles, the protected-destructor
// guard has been broken and the test suite fails.
// SPDX-License-Identifier: MIT

#include <cstdint>

#include "coact/ao.hpp"
#include "coact/event.hpp"
#include "coact/hsm.hpp"

namespace {

struct Ctx {
    std::uint32_t count;
};

static void on_count(Ctx&, const coact::Event&) noexcept {}

static const coact::StateDef<Ctx> kStates[] = {
    /* 0 root */ {-1, nullptr, nullptr},
    /* 1 leaf */ {0, nullptr, nullptr}
};

static const coact::TransitionDef<Ctx> kTrans[] = {
    {1, 1U, 0, coact::TransitionKind::Internal, nullptr, on_count}
};

struct Traits {
    static coact::LogicalPrio logical_prio() noexcept { return 1U; }
    static coact::PriorityClass priority_class() noexcept
    {
        return coact::PriorityClass::Normal;
    }
    static bool direct_eligible() noexcept { return true; }
    static bool isr_direct_safe() noexcept { return false; }
    static constexpr std::uint64_t kRtcBudgetNs = coact::DefaultConfig::kRtcBudgetNs;
};

}  // namespace

void use_forbidden_delete()
{
    coact::Ao<Ctx, coact::Hsm<Ctx>, Traits> ao(
        kStates, 2U, kTrans, 1U, 1, 4U);
    coact::AoBase* base = &ao;
    delete base;  // MUST NOT compile: AoBase dtor is protected non-virtual
}
