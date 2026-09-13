cmake_minimum_required(VERSION 3.16)
project(coact VERSION 0.5.0 LANGUAGES CXX)

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

# Strict-warning + no-exceptions / no-RTTI policy. Guarded per compiler: the
# former unconditional GCC/Clang flags made every MSVC configure fail before
# compiling a single TU, which is why the Windows product's branch had no CI.
if(MSVC)
    # Usage requirements for consumers only: a header-only INTERFACE library has
    # no TUs, so it cannot set the warning level of code it does not compile -
    # these flags reach whoever links coact_core, and the xcom_lua product (which
    # compiles these headers without linking it) sets its own /W4.
    target_compile_options(coact_core INTERFACE /W4 /GR- /EHs-c-)
else()
    target_compile_options(coact_core INTERFACE
        -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
endif()

# Host-portability gate for CI. OFF by default, so every POSIX / RT-Thread /
# coro / timer suite builds on the Linux host exactly as before. The MSVC job
# sets it ON: only targets that compile under MSVC are registered, and every
# skipped suite is named at configure time (and echoed by the job) so a skip
# can never be mistaken for a pass. This is not a platform macro - it is an
# explicit CI scope, and it changes nothing at runtime.
option(COACT_PORTABLE_ONLY
       "Register only targets that build on every host (skip POSIX/RT-Thread/coro/timer suites)"
       OFF)

# Visible-skip helper. Prints only under the Windows CI scope; a no-op on a
# normal host build so the configure log stays clean.
function(coact_skip target reason)
    if(COACT_PORTABLE_ONLY)
        message(STATUS "coact CI: SKIP target '${target}' - ${reason}")
    endif()
endfunction()

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

# Examples are all POSIX-PAL executables (or RT-Thread host-stub builds); none
# is host-portable under MSVC. Skip them as a unit and say so out loud.
if(NOT COACT_PORTABLE_ONLY)
    add_subdirectory(examples)
else()
    coact_skip(examples "POSIX PAL / pthread workers / RT-Thread host stub")
endif()

# Guard against a configure that registers zero core tests: `ctest` with an
# empty test set exits 0 and would look green while proving nothing. The
# portable core test is registered on every host, so its absence is fatal on
# Linux and under the Windows CI scope alike.
if(NOT TARGET test_coordinator)
    message(FATAL_ERROR
        "coact: test_coordinator target missing - CI scope is broken")
endif()
