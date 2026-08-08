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

// Static branch hints for the hot path (event_gc, pool alloc, batch select).
// __builtin_expect is supported by GCC/Clang (host Linux and Xuantie RISC-V
// GCC); on single-core Cortex-M it compiles to a no-op. Hint only the branches
// whose direction is stable under load - never sprinkle.
#define COACT_LIKELY(x)     __builtin_expect((x), 1)
#define COACT_UNLIKELY(x)   __builtin_expect((x), 0)
