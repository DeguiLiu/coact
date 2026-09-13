# coact host examples (POSIX PAL). Each is a standalone executable exercising
# the framework end-to-end: pool -> coordinator submit -> staging -> Dispatcher
# -> Ao dispatch -> HSM. SPDX-License-Identifier: MIT
find_package(Threads REQUIRED)

add_executable(hsm_protocol_demo
    hsm_protocol_demo.cpp
    ${CMAKE_SOURCE_DIR}/src/core/pal_posix.cpp)
target_link_libraries(hsm_protocol_demo PRIVATE coact_core Threads::Threads)

add_executable(node_manager_demo
    node_manager_demo.cpp
    ${CMAKE_SOURCE_DIR}/src/core/pal_posix.cpp)
target_link_libraries(node_manager_demo PRIVATE coact_core Threads::Threads)

# flash_proxy_demo: complex example. Owner AO serializes a NAND device behind
# a non-AO DMA worker; timed interrupt-callback completion, ref-count fan-out
# and request/response query with resource contention on the shared DMA buffer.
add_executable(flash_proxy_demo
    flash_proxy_demo.cpp
    ${CMAKE_SOURCE_DIR}/src/core/pal_posix.cpp)
target_link_libraries(flash_proxy_demo PRIVATE coact_core Threads::Threads)
# Self-verifying: asserts its invariants and exits non-zero on failure.
add_test(NAME flash_proxy_demo COMMAND flash_proxy_demo)

# msh_monitor_demo: MSH-style three-layer monitoring queries over a running
# AO system (design_architecture_zh.md 5.9). A compact self-built runtime:
# 7 AOs (incl. a reconfig-transaction HSM) + 2 non-AO workers + a mini
# hardware blackboard; the scenario (boot -> IRSC 4-step -> streaming ->
# live X1->X2 reconfig -> stop) interleaves `query status/data/health` at
# milestones AND fires queries from a parallel monitor pthread mid-run.
# Queries read lock-free snapshots only (no event plane) — the "query never
# interrupts" proof. Command dispatch is a const static function table.
add_executable(msh_monitor_demo
    msh_monitor_demo.cpp
    ${CMAKE_SOURCE_DIR}/src/core/pal_posix.cpp)
target_link_libraries(msh_monitor_demo PRIVATE coact_core Threads::Threads)
# Self-verifying: asserts its invariants and exits non-zero on failure.
add_test(NAME msh_monitor_demo COMMAND msh_monitor_demo)

include(${CMAKE_CURRENT_SOURCE_DIR}/coact_coro_demos.cmake)

# log_rtthread_demo: single-core RT-Thread static log adapter, compiled on host
# against test/rtthread_stub.h (RT-Thread API backed by pthreads). Exercises the
# writer end-to-end: task + ISR producers, admission drop, stop/drain.
add_executable(log_rtthread_demo
    log_rtthread_demo.cpp
    ${CMAKE_SOURCE_DIR}/src/diag/log_rtthread.cpp)
target_link_libraries(log_rtthread_demo PRIVATE coact_core Threads::Threads)
target_compile_definitions(log_rtthread_demo PRIVATE COACT_RTT_STUB)
target_include_directories(log_rtthread_demo PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME log_rtthread_demo COMMAND log_rtthread_demo)

# serial_ota_demo: multi-file industrial example (Serial OTA upgrade). It
# depends on an external, out-of-tree, header-only component library
# (osp::StateMachine/BehaviorTree/SpscRingbuffer/TimerScheduler/...) for
# the device OTA state machine, host BehaviorTree and UART FIFOs, while coact
# carries the frame-parser -> OtaHost event link on its Dispatcher thread.
# That library is a separate out-of-tree repo, so this example is opt-in.
option(COACT_BUILD_SERIAL_OTA "Build the serial_ota_demo (requires the external OTA headers)" OFF)
if(COACT_BUILD_SERIAL_OTA)
    if(EXISTS "$ENV{HOME}/newosp/include/osp/hsm.hpp")
        set(NEWOSP_ROOT "$ENV{HOME}/newosp")
    else()
        set(NEWOSP_ROOT "/home/dgliu/newosp")
    endif()
    if(NOT EXISTS "${NEWOSP_ROOT}/include/osp/hsm.hpp")
        message(WARNING "COACT_BUILD_SERIAL_OTA=ON but external OTA headers not found at ${NEWOSP_ROOT}; serial_ota_demo skipped")
    else()
        add_executable(serial_ota_demo
            serial_ota/main.cpp
            ${CMAKE_SOURCE_DIR}/src/core/pal_posix.cpp)
        target_include_directories(serial_ota_demo PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/serial_ota
            ${NEWOSP_ROOT}/include)
        target_link_libraries(serial_ota_demo PRIVATE coact_core Threads::Threads)
        add_test(NAME serial_ota_demo COMMAND serial_ota_demo)
    endif()
endif()

