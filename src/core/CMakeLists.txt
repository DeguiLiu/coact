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

