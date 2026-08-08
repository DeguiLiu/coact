add_library(coact_test_harness INTERFACE)
target_include_directories(coact_test_harness INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}     # allows #include "test_harness.hpp"
    ${CMAKE_SOURCE_DIR})            # allows #include "test/test_harness.hpp"

# Helper to declare a host test executable wired to the harness.
# Usage: coact_add_test(<target_name> <source.cpp>...)
function(coact_add_test target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE coact_core coact_test_harness)
    add_test(NAME ${target} COMMAND ${target})
endfunction()
