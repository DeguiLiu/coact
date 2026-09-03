# coact::coro demos (plan Task 7). Standalone include so parallel example
# edits (msh_monitor / isp_pipeline) only conflict on one include line.
# SPDX-License-Identifier: MIT

add_executable(coact_coro_demo
    coact_coro_demo.cpp
    ${CMAKE_SOURCE_DIR}/src/core/pal_posix.cpp)
target_link_libraries(coact_coro_demo PRIVATE coact_core Threads::Threads)
add_test(NAME coact_coro_demo COMMAND coact_coro_demo)

add_executable(coact_coro_posix
    coact_coro_posix.cpp
    ${CMAKE_SOURCE_DIR}/src/core/pal_posix.cpp)
target_link_libraries(coact_coro_posix PRIVATE coact_core Threads::Threads)
add_test(NAME coact_coro_posix COMMAND coact_coro_posix)
