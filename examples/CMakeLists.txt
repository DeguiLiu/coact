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

# serial_ota_demo: multi-file industrial example (Serial OTA upgrade) ported
# from ~/newosp/examples/serial_ota. It depends on the header-only newosp
# library (osp::StateMachine/BehaviorTree/SpscRingbuffer/TimerScheduler/...) for
# the device OTA state machine, host BehaviorTree and UART FIFOs, while coact
# carries the frame-parser -> OtaHost event link on its Dispatcher thread.
# newosp is header-only (add_library(osp INTERFACE)); point include at its tree.
set(NEWOSP_ROOT "$ENV{HOME}/newosp")
if(NOT EXISTS "${NEWOSP_ROOT}/include/osp/hsm.hpp")
    set(NEWOSP_ROOT "/home/dgliu/newosp")
endif()

add_executable(serial_ota_demo
    serial_ota/main.cpp
    ${CMAKE_SOURCE_DIR}/src/core/pal_posix.cpp)
target_include_directories(serial_ota_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/serial_ota
    ${NEWOSP_ROOT}/include)
target_link_libraries(serial_ota_demo PRIVATE coact_core Threads::Threads)

add_test(NAME serial_ota_demo COMMAND serial_ota_demo)

