# coact ao module: header-only core + host test.
# SPDX-License-Identifier: MIT
coact_add_test(test_ao test_ao.cpp)

# The test harness interface library only adds <repo>/test to the include
# path, while contract 0/2 mandates `#include "test/test_harness.hpp"`.
# Add <repo> locally so the mandated include form resolves.
target_include_directories(test_ao PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Tests spawn a fork/child for COACT_ASSERT (abort) negative cases.
find_package(Threads REQUIRED)
target_link_libraries(test_ao PRIVATE Threads::Threads)
