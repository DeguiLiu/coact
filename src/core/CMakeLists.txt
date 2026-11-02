# coact core module: PAL, Dispatcher, Coordinator, Runtime + integration tests.
# SPDX-License-Identifier: MIT
find_package(Threads REQUIRED)

# ---- Host-portable core tests (build everywhere, including MSVC) ----------
# These use only headers plus Threads::Threads; no POSIX PAL, no pthread,
# no fork(). They are the tests that prove the Windows CI scope is not empty.
coact_add_test(test_expected test_expected.cpp)
target_include_directories(test_expected PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# C++17 non-owning view (replaces pointer+length pairs in public APIs).
coact_add_test(test_span test_span.cpp)

# Coordinator wakeup-on-empty optimization test (flame finding): mock PAL counts
# Dispatcher signals to prove a submit behind a buffered event does not wake it.
coact_add_test(test_coordinator
    test_coordinator.cpp)
target_link_libraries(test_coordinator PRIVATE Threads::Threads)
target_include_directories(test_coordinator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# P1 admission: per-target arrival interval, backlog cap and the fixed
# admission snapshot ring (mock clock, no POSIX PAL / threads). The rule set
# is a compile-time template parameter, so these stay host-portable.
coact_add_test(test_admission
    test_admission.cpp)
target_include_directories(test_admission PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Negative compile contracts for the admission rule set: an invalid table must
# be rejected where the board declares it, not ignored at boot. EXCLUDE_FROM_ALL
# keeps them out of the default build; each ctest builds its target on demand
# and passes only when the output carries the specific static_assert message.
# A plain WILL_FAIL test would also pass on an unrelated compile error, e.g. a
# missing include or a changed constructor.
set(neg_expect_TARGET_ZERO
    "admission rule names a target outside 1\\.\\.kMaxAo")
set(neg_expect_TARGET_OVER_MAX_AO
    "admission rule names a target outside 1\\.\\.kMaxAo")
set(neg_expect_DUPLICATE_TARGET
    "duplicate TargetId in the admission rule set")
set(neg_expect_INTERVAL_UNBOUNDED
    "min_arrival above kMaxMinArrivalNs")
set(neg_expect_TOO_MANY_RULES
    "more admission rules than Config::kMaxAo")
foreach(neg_case
        TARGET_ZERO TARGET_OVER_MAX_AO DUPLICATE_TARGET
        INTERVAL_UNBOUNDED TOO_MANY_RULES)
    string(TOLOWER "${neg_case}" neg_name)
    add_library(test_admission_${neg_name}_neg EXCLUDE_FROM_ALL STATIC
                admission_invalid_rules_neg.cpp)
    target_compile_definitions(test_admission_${neg_name}_neg PRIVATE
                               COACT_TEST_ADM_${neg_case}=1)
    target_link_libraries(test_admission_${neg_name}_neg PRIVATE coact_core)
    add_test(NAME test_admission_${neg_name}_neg
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                     --target test_admission_${neg_name}_neg)
    set_tests_properties(test_admission_${neg_name}_neg PROPERTIES
        PASS_REGULAR_EXPRESSION "${neg_expect_${neg_case}}")
endforeach()

# ---- POSIX / RT-Thread host suites ----------------------------------------
# Everything below links pal_posix.cpp / pal_rtthread.cpp (pthread, signalfd,
# ucontext) or the COACT_RTT_STUB pthread shim, or forks a child to observe
# COACT_ASSERT. None of it compiles under MSVC; the Windows CI scope skips the
# whole block by name instead of pretending these suites passed.
if(COACT_PORTABLE_ONLY)
foreach(skipped
            test_core test_integration test_pal_sync test_rtt_pal test_stress
            test_config test_timer test_bitfield test_static_lifetime
            test_static_pal test_freertos_pal test_softirq test_rtt_smp_gate
            test_rtt_multicore_gate bench_hotpath
            test_admission_adv
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
# zero-heap hot path.
coact_add_test(test_stress
    test_stress.cpp
    pal_posix.cpp)
target_link_libraries(test_stress PRIVATE Threads::Threads)
target_include_directories(test_stress PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# P1 admission adversarial suite: the concurrency acceptance bound, ISR/task
# interleave, rollback-on-every-failure-path, hostile rule sets, the fixed
# snapshot ring, zero-allocation submit path and the measured footprint of the
# disabled policy. Host-only because it drives real std::thread producers and
# counts global operator new; the rule sets themselves stay compile-time.
coact_add_test(test_admission_adv
    test_admission_adv.cpp
    pal_posix.cpp)
target_link_libraries(test_admission_adv PRIVATE Threads::Threads)
target_include_directories(test_admission_adv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

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
# the field-level write_field / read_field entry.
coact_add_test(test_bitfield
    test_bitfield.cpp
    pal_posix.cpp
    ${CMAKE_SOURCE_DIR}/src/diag/log_rtthread.cpp)
target_link_libraries(test_bitfield PRIVATE Threads::Threads)
target_compile_definitions(test_bitfield PRIVATE COACT_RTT_STUB)
target_include_directories(test_bitfield PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../..)

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

# Static FreeRTOS PAL test (docs/design_freertos_pal_zh.md): FreeRtosResources,
# explicit initialize/InitError, ContextSlot freeze, ISR/task critical-section
# split, ClockOps extension, dispatcher start/join, Sem/Mutex/Cond, resident
# workers, SoftIrq and one Runtime assembly. The FreeRTOS API is backed by
# test/freertos_stub.h (pthreads); no kernel source is required for host CI.
coact_add_test(test_freertos_pal
    test_freertos_pal.cpp
    pal_freertos.cpp)
target_link_libraries(test_freertos_pal PRIVATE Threads::Threads)
target_compile_definitions(test_freertos_pal PRIVATE COACT_FREERTOS_STUB)
target_include_directories(test_freertos_pal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

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

# Windows PAL: native Win32 primitives (CreateSemaphoreW / CRITICAL_SECTION /
# CONDITION_VARIABLE / _beginthreadex / event + SPSC ring). Windows-only — the
# Linux host build must not compile pal_windows.cpp (it includes <windows.h>).
#
# Deliberately OUTSIDE the COACT_PORTABLE_ONLY gate above, and the merge that
# introduced this file arrangement is why the comment is here: the Windows CI
# job configures with -DCOACT_PORTABLE_ONLY=ON, so anything inside that gate's
# else() branch is NOT built there. This suite is the one Windows-specific test
# that must still run in the portable Windows scope — it is what exercises the
# real Win32 PAL. Placing it inside the gate silently skips it and leaves the
# shipping platform's PAL untested.
if(WIN32)
    coact_add_test(test_pal_windows
        test_pal_windows.cpp
        pal_windows.cpp)
    target_include_directories(test_pal_windows PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../..)
endif()
