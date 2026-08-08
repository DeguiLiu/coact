# coact core module: PAL, Dispatcher, Coordinator, Runtime + integration tests.
# SPDX-License-Identifier: MIT
find_package(Threads REQUIRED)

coact_add_test(test_core
    test_core.cpp
    pal_posix.cpp)
target_link_libraries(test_core PRIVATE Threads::Threads)
target_include_directories(test_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

coact_add_test(test_integration
    test_integration.cpp
    pal_posix.cpp)
target_link_libraries(test_integration PRIVATE Threads::Threads)
target_include_directories(test_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# RT-Thread PAL host test: RT-Thread API backed by pthreads via stub header.
coact_add_test(test_rtt_pal
    test_rtt_pal.cpp
    pal_rtthread.cpp)
target_link_libraries(test_rtt_pal PRIVATE Threads::Threads)
target_compile_definitions(test_rtt_pal PRIVATE COACT_RTT_STUB)
target_include_directories(test_rtt_pal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Linux stress tests: concurrent multi-producer, overload degradation,
# zero-heap hot path (see qpc-rtthread / newosp test intent).
coact_add_test(test_stress
    test_stress.cpp
    pal_posix.cpp)
target_link_libraries(test_stress PRIVATE Threads::Threads)
target_include_directories(test_stress PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Config pass-through test: one board-level Config threads through registry,
# monitor, breaker, staging and dispatcher (compile-time + runtime).
coact_add_test(test_config
    test_config.cpp
    pal_posix.cpp)
target_link_libraries(test_config PRIVATE Threads::Threads)
target_include_directories(test_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Coordinator wakeup-on-empty optimization test (flame finding): mock PAL counts
# Dispatcher signals to prove a submit behind a buffered event does not wake it.
coact_add_test(test_coordinator
    test_coordinator.cpp)
target_link_libraries(test_coordinator PRIVATE Threads::Threads)
target_include_directories(test_coordinator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Static-lifetime Runtime test (review P1): global .bss Runtime runs end-to-end
# under the RT-Thread stub; Config->PAL stack pass-through; memory footprint.
coact_add_test(test_static_lifetime
    test_static_lifetime.cpp
    pal_rtthread.cpp)
target_link_libraries(test_static_lifetime PRIVATE Threads::Threads)
target_compile_definitions(test_static_lifetime PRIVATE COACT_RTT_STUB)
target_include_directories(test_static_lifetime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Hot-path benchmark with a built-in "poor man's" sampler (no perf/root needed).
# --sample writes a folded-stacks file for tools/flamegraph_svg.py.
add_executable(bench_hotpath bench_hotpath.cpp pal_posix.cpp)
target_link_libraries(bench_hotpath PRIVATE coact_core Threads::Threads)
# -O2 for representative timings; keep frame pointers so backtrace() works.
target_compile_options(bench_hotpath PRIVATE -O2 -g -fno-omit-frame-pointer)
target_link_options(bench_hotpath PRIVATE -rdynamic)
target_include_directories(bench_hotpath PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

