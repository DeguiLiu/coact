# coact hsm module: header-only core + host differential test.
# SPDX-License-Identifier: MIT
if(COACT_PORTABLE_ONLY)
    coact_skip(test_hsm "fork()/waitpid negative COACT_ASSERT cases (POSIX only)")
else()
coact_add_test(test_hsm test_hsm.cpp)

# The test harness interface library only adds <repo>/test to the include
# path, while contract 0/2 mandates `#include "test/test_harness.hpp"`.
# Add <repo> locally so the mandated include form resolves.
target_include_directories(test_hsm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
endif()
