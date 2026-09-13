# coact core module: PAL, Dispatcher, Coordinator, Runtime + integration tests.
# SPDX-License-Identifier: MIT
find_package(Threads REQUIRED)

# ---- Host-portable core tests (build everywhere, including MSVC) ----------
# These use only headers plus Threads::Threads; no POSIX PAL, no pthread,
# no fork(). They are the tests that prove the Windows CI scope is not empty.
coact_add_test(test_expected test_expected.cpp)
target_include_directories(test_expected PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Coordinator wakeup-on-empty optimization test (flame finding): mock PAL counts
# Dispatcher signals to prove a submit behind a buffered event does not wake it.
coact_add_test(test_coordinator
    test_coordinator.cpp)
target_link_libraries(test_coordinator PRIVATE Threads::Threads)
target_include_directories(test_coordinator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# ---- POSIX / RT-Thread host suites ----------------------------------------
# Everything below links pal_posix.cpp / pal_rtthread.cpp (pthread, signalfd,
# ucontext) or the COACT_RTT_STUB pthread shim, or forks a child to observe
# COACT_ASSERT. None of it compiles under MSVC; the Windows CI scope skips the
# whole block by name instead of pretending these suites passed.
if(COACT_PORTABLE_ONLY)
    foreach(skipped
            test_core test_integration test_pal_sync test_rtt_pal test_stress
            test_config test_timer test_bitfield test_static_lifetime
            test_static_pal test_softirq test_rtt_smp_gate
            test_rtt_multicore_gate bench_hotpath
            test_coro_registry test_coro_combinators test_coro_posix
            test_coro_awaitable test_coro_scheduler test_coro_integration
            test_coro_rg_gate)
        coact_skip("${skipped}"
            "POSIX PAL / pthread / RT-Thread host stub / ucontext (MSVC cannot build it)")
    endforeach()
else()

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
coact_add_test(test_pal_sync
    test_pal_sync.cpp
    pal_posix.cpp
    pal_rtthread.cpp)
target_link_libraries(test_pal_sync PRIVATE Threads::Threads)
target_compile_definitions(test_pal_sync PRIVATE COACT_RTT_STUB)
target_include_directories(test_pal_sync PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

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

# TimerScheduler (event-source timer adapted from newosp osp::timer.hpp):
# ManualTickSource poll() mode end-to-end through the full coact pipeline.
coact_add_test(test_timer
    test_timer.cpp
    pal_posix.cpp)
target_link_libraries(test_timer PRIVATE Threads::Threads)
target_include_directories(test_timer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# BitFieldView: compile-time mask/shift boundaries, RMW isolation, and
# end-to-end integration with PeriphRegCache's three-flag protocol through
# the field-level write_field / read_field entry (the demo's regmap layer).
coact_add_test(test_bitfield
    test_bitfield.cpp
    pal_posix.cpp
    ${CMAKE_SOURCE_DIR}/src/diag/log_rtthread.cpp)
target_link_libraries(test_bitfield PRIVATE Threads::Threads)
target_compile_definitions(test_bitfield PRIVATE COACT_RTT_STUB)
target_include_directories(test_bitfield PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_SOURCE_DIR}/examples)

# Static-lifetime Runtime test (review P1): global .bss Runtime runs end-to-end
# under the RT-Thread stub; Config->PAL stack pass-through; memory footprint.
coact_add_test(test_static_lifetime
    test_static_lifetime.cpp
    pal_rtthread.cpp)
target_link_libraries(test_static_lifetime PRIVATE Threads::Threads)
target_compile_definitions(test_static_lifetime PRIVATE COACT_RTT_STUB)
target_include_directories(test_static_lifetime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Static RT-Thread PAL test (design §7.5): RtThreadResources / explicit
# initialize + error-returning start / fixed ContextSlot table / ClockOps /
# single-core Dispatcher profile assembly. TDD: RED first against the
# historical PAL, GREEN after the staticization below.
coact_add_test(test_static_pal
    test_static_pal.cpp
    pal_rtthread.cpp)
target_link_libraries(test_static_pal PRIVATE Threads::Threads)
target_compile_definitions(test_static_pal PRIVATE COACT_RTT_STUB)
target_include_directories(test_static_pal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# SoftIrqOps (signalfd + RT-Thread SPSC ring) test: real POSIX signal path +
# stub-backed ring path. The RT-Thread side must compile under COACT_RTT_STUB
# (board-level rt_signal_wait verification is pending — the stub makes
# rt_signal_install / rt_thread_kill no-ops and the ring carries the payload).
coact_add_test(test_softirq
    test_softirq.cpp
    pal_posix.cpp
    pal_rtthread.cpp)
target_link_libraries(test_softirq PRIVATE Threads::Threads)
target_compile_definitions(test_softirq PRIVATE COACT_RTT_STUB)
target_include_directories(test_softirq PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Negative compile contracts: RtThread::Profile is only valid on a real
# single-core RT-Thread build. Each target compiles with a minimal synthetic
# RT-Thread configuration and CTest inverts the expected compiler failure.
add_library(test_rtt_smp_gate EXCLUDE_FROM_ALL STATIC rtt_compile_gate.cpp)
target_link_libraries(test_rtt_smp_gate PRIVATE coact_core)
target_include_directories(test_rtt_smp_gate BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../test/rtthread_gate_smp)
add_test(NAME test_rtt_smp_gate
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test_rtt_smp_gate)
set_tests_properties(test_rtt_smp_gate PROPERTIES WILL_FAIL TRUE)

add_library(test_rtt_multicore_gate EXCLUDE_FROM_ALL STATIC rtt_compile_gate.cpp)
target_link_libraries(test_rtt_multicore_gate PRIVATE coact_core)
target_include_directories(test_rtt_multicore_gate BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../test/rtthread_gate_multicore)
add_test(NAME test_rtt_multicore_gate
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test_rtt_multicore_gate)
set_tests_properties(test_rtt_multicore_gate PROPERTIES WILL_FAIL TRUE)

# Hot-path benchmark with a built-in "poor man's" sampler (no perf/root needed).
# --sample writes a folded-stacks file for tools/flamegraph_svg.py.
add_executable(bench_hotpath bench_hotpath.cpp pal_posix.cpp)
target_link_libraries(bench_hotpath PRIVATE coact_core Threads::Threads)
# -O2 for representative timings; keep frame pointers so backtrace() works.
if(MSVC)
    target_compile_options(bench_hotpath PRIVATE /O2)
else()
    target_compile_options(bench_hotpath PRIVATE -O2 -g -fno-omit-frame-pointer)
    target_link_options(bench_hotpath PRIVATE -rdynamic)
endif()
target_include_directories(bench_hotpath PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# coact::coro: task registry / combinators / stackful executor (Linux PAL
# cooperative layer). Registration lives in the standalone fragment so the
# component can land independently of other in-flight src/core changes.
include(${CMAKE_CURRENT_SOURCE_DIR}/test_coro.cmake)

endif()  # COACT_PORTABLE_ONLY
