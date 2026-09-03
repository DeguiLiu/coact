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

# isp_pipeline_demo: complex example. Models the RS500 "Preview Start" path:
# IRSC pthread producer -> low/high gain chains (parallel) -> HL fuse -> enhance
# + TPD chains -> PIC/TEMP video packaging with DMA triple-buffer -> USB/MIPI
# sinks. Runtime progress is emitted through the coact::diag log channel
# (COACT_RTT_STUB host adapter); the orchestrator drives Phase1/3/4, IRSC
# 4-step, ISP init forward, video FSM IDLE->READY->RUNNING, KBC then IRSC output
# enable order, and STOP reverse.
add_executable(isp_pipeline_demo
    isp_pipeline/main.cpp
    isp_pipeline/common.hpp
    isp_pipeline/sensor_irsc.hpp
    isp_pipeline/sensor_irsc.cpp
    isp_pipeline/isp_chain.hpp
    isp_pipeline/isp_chain.cpp
    isp_pipeline/video_stream.hpp
    isp_pipeline/video_stream.cpp
    isp_pipeline/output_itf.hpp
    isp_pipeline/output_itf.cpp
    isp_pipeline/recfg_session.hpp
    isp_pipeline/recfg_session.cpp
    ${CMAKE_SOURCE_DIR}/src/core/pal_posix.cpp
    ${CMAKE_SOURCE_DIR}/src/diag/log_rtthread.cpp)
target_compile_definitions(isp_pipeline_demo PRIVATE COACT_RTT_STUB)
target_include_directories(isp_pipeline_demo PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(isp_pipeline_demo PRIVATE coact_core Threads::Threads)
# Self-verifying: asserts its invariants and exits non-zero on failure.
add_test(NAME isp_pipeline_demo COMMAND isp_pipeline_demo)

# isp_pipeline RT-Thread compile gate: the SAME demo sources compiled against
# the RT-Thread PAL through the host stub (COACT_RTT_STUB, the test_rtt_pal
# pattern). Proves the demo's PAL-ization is real — no pthread/unistd leaks
# would compile under ISP_DEMO_USE_RTT. Milestone is a zero-warning compile +
# link of the RT-Thread path (the host stub's ms-granularity mdelay shifts
# the timing windows enough that the T37 frame-gap assertion is not asserted
# here; real-board pacing is a separate milestone).
add_executable(isp_pipeline_demo_rtt EXCLUDE_FROM_ALL
    isp_pipeline/main.cpp
    isp_pipeline/sensor_irsc.cpp
    isp_pipeline/isp_chain.cpp
    isp_pipeline/video_stream.cpp
    isp_pipeline/output_itf.cpp
    isp_pipeline/recfg_session.cpp
    ${CMAKE_SOURCE_DIR}/src/core/pal_rtthread.cpp
    ${CMAKE_SOURCE_DIR}/src/diag/log_rtthread.cpp)
target_compile_definitions(isp_pipeline_demo_rtt PRIVATE
    COACT_RTT_STUB ISP_DEMO_USE_RTT)
target_include_directories(isp_pipeline_demo_rtt PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(isp_pipeline_demo_rtt PRIVATE coact_core Threads::Threads)
add_test(NAME isp_pipeline_demo_rtt
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                 --target isp_pipeline_demo_rtt)

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

# serial_ota_demo: multi-file industrial example (Serial OTA upgrade) ported
# from ~/newosp/examples/serial_ota. It depends on the header-only newosp
# library (osp::StateMachine/BehaviorTree/SpscRingbuffer/TimerScheduler/...) for
# the device OTA state machine, host BehaviorTree and UART FIFOs, while coact
# carries the frame-parser -> OtaHost event link on its Dispatcher thread.
# newosp is a separate out-of-tree repo, so this example is opt-in.
option(COACT_BUILD_SERIAL_OTA "Build the serial_ota_demo (requires newosp headers)" OFF)
if(COACT_BUILD_SERIAL_OTA)
    if(EXISTS "$ENV{HOME}/newosp/include/osp/hsm.hpp")
        set(NEWOSP_ROOT "$ENV{HOME}/newosp")
    else()
        set(NEWOSP_ROOT "/home/dgliu/newosp")
    endif()
    if(NOT EXISTS "${NEWOSP_ROOT}/include/osp/hsm.hpp")
        message(WARNING "COACT_BUILD_SERIAL_OTA=ON but newosp headers not found at ${NEWOSP_ROOT}; serial_ota_demo skipped")
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

