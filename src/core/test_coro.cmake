# coact::coro test registration. Kept in a standalone file so the async
# component lands without touching the shared src/core/CMakeLists.txt
# (parallel PAL work); include from src/core/CMakeLists.txt:
#   include(${CMAKE_CURRENT_SOURCE_DIR}/test_coro.cmake)
# SPDX-License-Identifier: MIT

coact_add_test(test_coro_registry
    test_coro_registry.cpp
    pal_posix.cpp)
target_link_libraries(test_coro_registry PRIVATE Threads::Threads)
target_include_directories(test_coro_registry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

coact_add_test(test_coro_combinators
    test_coro_combinators.cpp
    pal_posix.cpp)
target_link_libraries(test_coro_combinators PRIVATE Threads::Threads)
target_include_directories(test_coro_combinators PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

coact_add_test(test_coro_posix test_coro_posix.cpp)
target_link_libraries(test_coro_posix PRIVATE Threads::Threads)
target_include_directories(test_coro_posix PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
