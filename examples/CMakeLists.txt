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

