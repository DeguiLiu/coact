cmake_minimum_required(VERSION 3.16)
project(coact VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# ---------------------------------------------------------------------------
# coact_core: interface-only library. All core components are header-only
# templates compiled under strict C++17 with exceptions and RTTI disabled.
# ---------------------------------------------------------------------------
add_library(coact_core INTERFACE)
target_include_directories(coact_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(coact_core INTERFACE
    -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)

enable_testing()

add_subdirectory(test)
add_subdirectory(src/event)
add_subdirectory(src/hsm)
add_subdirectory(src/queue)
add_subdirectory(src/ao)
add_subdirectory(src/staging)
add_subdirectory(src/policy)
add_subdirectory(src/monitor)
add_subdirectory(src/core)
add_subdirectory(examples)
