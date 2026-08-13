coact_add_test(test_event test_event.cpp)
coact_add_test(test_pool_lifecycle test_pool_lifecycle.cpp)
coact_add_test(test_pool_single_core test_pool_single_core.cpp)
coact_add_test(test_pool_smp test_pool_smp.cpp)
coact_add_test(test_reclaimer test_reclaimer.cpp)

find_package(Threads REQUIRED)
coact_add_test(test_dispatcher_reclaim test_dispatcher_reclaim.cpp ../core/pal_posix.cpp)
target_link_libraries(test_dispatcher_reclaim PRIVATE Threads::Threads)
target_include_directories(test_dispatcher_reclaim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Negative compile contract: this TU MUST NOT compile (typed alloc with a
# non-trivial payload). EXCLUDE_FROM_ALL keeps it out of the default build; the
# WILL_FAIL ctest builds the target on demand and inverts the expected build
# failure into a pass.
add_library(test_pool_lifecycle_neg EXCLUDE_FROM_ALL STATIC pool_lifecycle_neg.cpp)
target_link_libraries(test_pool_lifecycle_neg PRIVATE coact_core)
add_test(NAME test_pool_lifecycle_neg
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test_pool_lifecycle_neg)
set_tests_properties(test_pool_lifecycle_neg PROPERTIES WILL_FAIL TRUE)
