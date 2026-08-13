coact_add_test(test_queue test_queue.cpp)
target_link_libraries(test_queue PRIVATE pthread)

# SpscRing prototype tests (design 5.2/5.4, stage-1 hard checkpoint A).
coact_add_test(test_spsc_ring test_spsc_ring.cpp)
target_link_libraries(test_spsc_ring PRIVATE pthread)

# Negative compile contract: SpscRing capacity must be a power of two >= 2 and
# <= 0x7FFF. The target instantiates an invalid capacity and CTest inverts the
# expected compiler failure (mirrors src/core rtt_compile_gate pattern).
add_library(test_spsc_gate EXCLUDE_FROM_ALL STATIC spsc_compile_gate.cpp)
target_link_libraries(test_spsc_gate PRIVATE coact_core)
add_test(NAME test_spsc_gate
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test_spsc_gate)
set_tests_properties(test_spsc_gate PROPERTIES WILL_FAIL TRUE)

# Host microbenchmark: SpscRing vs the existing irq-mask SingleCoreCriticalRing.
add_executable(bench_spsc bench_spsc.cpp)
target_link_libraries(bench_spsc PRIVATE coact_core pthread)
target_compile_options(bench_spsc PRIVATE -O2)
