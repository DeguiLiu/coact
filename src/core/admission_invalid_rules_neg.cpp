// Negative compile contract: this TU MUST NOT compile.
//
// StaticAdmission validates a board's rule set at the instantiation site, so
// an unbound target id, a duplicate target, an out-of-range interval or more
// rules than Config::kMaxAo is a compile error instead of a row that is
// silently ignored at boot. One -D switch per case; src/core/CMakeLists.txt
// registers each as its own EXCLUDE_FROM_ALL / WILL_FAIL target.
// SPDX-License-Identifier: MIT

#include <cstdint>

#include "coact/admission.hpp"

namespace {

struct Cfg {
    static constexpr std::uint8_t kMaxAo = 4U;
};

#if defined(COACT_TEST_ADM_TARGET_ZERO)
// TargetId 0 is kInvalidTarget and can never be bound: a rule naming it is
// dead configuration that would silently protect nothing.
struct Rules {
    static constexpr auto kRules = coact::make_rules(
        coact::TargetAdmission{coact::TargetId(0U), coact::MinArrivalNs{100U},
                               coact::MaxBacklog{0U}});
};
#elif defined(COACT_TEST_ADM_TARGET_OVER_MAX_AO)
// Past the config's AO budget: the generated lookup has no slot for it.
struct Rules {
    static constexpr auto kRules = coact::make_rules(
        coact::TargetAdmission{coact::TargetId(5U), coact::MinArrivalNs{100U},
                               coact::MaxBacklog{0U}});
};
#elif defined(COACT_TEST_ADM_DUPLICATE_TARGET)
// Two rows for one target have no defined precedence; the runtime table used
// to answer "first match wins", the compile-time table refuses the question.
struct Rules {
    static constexpr auto kRules = coact::make_rules(
        coact::TargetAdmission{coact::TargetId(1U), coact::MinArrivalNs{1000U},
                               coact::MaxBacklog{0U}},
        coact::TargetAdmission{coact::TargetId(1U), coact::MinArrivalNs{0U},
                               coact::MaxBacklog{2U}});
};
#elif defined(COACT_TEST_ADM_INTERVAL_UNBOUNDED)
// Above kMaxMinArrivalNs: a target closed for the lifetime of the product,
// in practice a ms/ns unit mistake.
struct Rules {
    static constexpr auto kRules = coact::make_rules(
        coact::TargetAdmission{coact::TargetId(1U),
                               coact::MinArrivalNs{coact::kMaxMinArrivalNs + 1U},
                               coact::MaxBacklog{0U}});
};
#elif defined(COACT_TEST_ADM_TOO_MANY_RULES)
// More rows than Config::kMaxAo. Distinct in-range targets cannot exceed
// kMaxAo, so the duplicate-target assert necessarily fires as well; both
// messages are emitted and the TU still must not compile.
struct Rules {
    static constexpr auto kRules = coact::make_rules(
        coact::TargetAdmission{coact::TargetId(1U), coact::MinArrivalNs{10U},
                               coact::MaxBacklog{0U}},
        coact::TargetAdmission{coact::TargetId(2U), coact::MinArrivalNs{10U},
                               coact::MaxBacklog{0U}},
        coact::TargetAdmission{coact::TargetId(3U), coact::MinArrivalNs{10U},
                               coact::MaxBacklog{0U}},
        coact::TargetAdmission{coact::TargetId(4U), coact::MinArrivalNs{10U},
                               coact::MaxBacklog{0U}},
        coact::TargetAdmission{coact::TargetId(1U), coact::MinArrivalNs{20U},
                               coact::MaxBacklog{0U}});
};
#else
#error "admission_invalid_rules_neg.cpp needs one COACT_TEST_ADM_* switch"
#endif

}  // namespace

void instantiate_admission()
{
    /* Compile the policy type, not an object: the deleted default ctor would
       make this TU fail even if the rule validation were removed, turning the
       gate green for the wrong reason. */
    using Adm = coact::StaticAdmission<Rules, Cfg, coact::AdmissionSpinSync>;
    static_assert(sizeof(Adm) > 0U, "instantiate the policy");
}
