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

coact_add_test(test_static_ao test_static_ao.cpp)
target_include_directories(test_static_ao PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Negative compile contract: this TU MUST NOT compile (`delete` through the
# type-erased AoBase*). EXCLUDE_FROM_ALL keeps it out of the default build; the
# WILL_FAIL ctest builds the target on demand and inverts the expected build
# failure into a pass.
add_library(ao_base_delete_neg EXCLUDE_FROM_ALL STATIC ao_base_delete_neg.cpp)
target_link_libraries(ao_base_delete_neg PRIVATE coact_core)
add_test(NAME ao_base_delete_neg
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ao_base_delete_neg)
set_tests_properties(ao_base_delete_neg PROPERTIES WILL_FAIL TRUE)
