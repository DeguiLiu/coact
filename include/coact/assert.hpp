// coact assertion support.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdlib>

namespace coact {

// Invoked on an irrecoverable invariant break. Default aborts; applications
// may provide a strong definition (declared here without inline) to install a
// product fatal handler, e.g. enter safe-state. See design 12.5.
inline void fatal_assert(const char* file, int line) noexcept {
    (void)file;
    (void)line;
    std::abort();
}

}  // namespace coact

#define COACT_ASSERT(cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            ::coact::fatal_assert(__FILE__, __LINE__);                  \
        }                                                               \
    } while (0)
