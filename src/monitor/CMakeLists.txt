# coact monitor module (M6) tests.
# SPDX-License-Identifier: MIT
coact_add_test(test_monitor test_monitor.cpp)
# The skeleton's coact_test_harness exposes include/test as the include root,
# so the contract-faithful "#include \"test/test_harness.hpp\"" needs the
# repository root on the include path.
target_include_directories(test_monitor PRIVATE ${CMAKE_SOURCE_DIR})
