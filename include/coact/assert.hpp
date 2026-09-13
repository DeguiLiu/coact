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
// __builtin_expect is a GCC/Clang extension (host Linux, Xuantie RISC-V GCC,
// and MSVC's clang-cl); plain MSVC has no equivalent, so the expression is
// used unchanged there. Hint only the branches whose direction is stable under
// load - never sprinkle.
#if defined(__clang__) || defined(__GNUC__)
#define COACT_LIKELY(x)     __builtin_expect(!!(x), 1)
#define COACT_UNLIKELY(x)   __builtin_expect(!!(x), 0)
#else
#define COACT_LIKELY(x)     (x)
#define COACT_UNLIKELY(x)   (x)
#endif
