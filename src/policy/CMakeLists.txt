# coact policy module: header-only strategy engine + host tests.
# SPDX-License-Identifier: MIT
coact_add_test(test_policy test_policy.cpp)

# The test harness interface library only adds <repo>/test to the include
# path, while contract 0/2 mandates `#include "test/test_harness.hpp"`.
# Add <repo> locally so the mandated include form resolves.
target_include_directories(test_policy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(test_policy PRIVATE pthread)
