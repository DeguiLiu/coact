add_library(coact_test_harness INTERFACE)
target_include_directories(coact_test_harness INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}     # allows #include "test_harness.hpp"
    ${CMAKE_SOURCE_DIR})            # allows #include "test/test_harness.hpp"

# Helper to declare a host test executable wired to the harness.
# Usage: coact_add_test(<target_name> <source.cpp>...)
#
# Every test gets a 120 s wall-clock bound. The whole suite runs in ~20 s, so
# this only fires on a genuine hang - and a bounded failure with ctest's own
# timeout diagnostic is far easier to act on than a binary that never returns.
# The known case it catches: on a host with high-entropy ASLR
# (vm.mmap_rnd_bits=32) the sync-primitive test can deadlock inside a futex
# rather than fail, because the kernel rejects libpthread's mappings. Run that
# test through .ai/run-tsan.sh (which disables ASLR with setarch -R) to get a
# real pass/fail instead of the timeout.
function(coact_add_test target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE coact_core coact_test_harness)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 120)
endfunction()
