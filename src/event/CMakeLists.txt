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

add_library(test_pool_meta_lifecycle_neg EXCLUDE_FROM_ALL STATIC
            pool_meta_lifecycle_neg.cpp)
target_link_libraries(test_pool_meta_lifecycle_neg PRIVATE coact_core)
add_test(NAME test_pool_meta_lifecycle_neg
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test_pool_meta_lifecycle_neg)
set_tests_properties(test_pool_meta_lifecycle_neg PROPERTIES WILL_FAIL TRUE)

add_library(test_pool_oversized_payload_neg EXCLUDE_FROM_ALL STATIC
            pool_oversized_payload_neg.cpp)
target_link_libraries(test_pool_oversized_payload_neg PRIVATE coact_core)
add_test(NAME test_pool_oversized_payload_neg
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test_pool_oversized_payload_neg)
set_tests_properties(test_pool_oversized_payload_neg PROPERTIES WILL_FAIL TRUE)

add_library(test_pool_payload_throwing_ctor_neg EXCLUDE_FROM_ALL STATIC
            pool_default_ctor_neg.cpp)
target_link_libraries(test_pool_payload_throwing_ctor_neg PRIVATE coact_core)
add_test(NAME test_pool_payload_throwing_ctor_neg
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test_pool_payload_throwing_ctor_neg)
set_tests_properties(test_pool_payload_throwing_ctor_neg PROPERTIES WILL_FAIL TRUE)

add_library(test_pool_layout_throwing_ctor_neg EXCLUDE_FROM_ALL STATIC
            pool_default_ctor_neg.cpp)
target_compile_definitions(test_pool_layout_throwing_ctor_neg PRIVATE
                           COACT_TEST_LAYOUT_DEFAULT_CTOR=1)
target_link_libraries(test_pool_layout_throwing_ctor_neg PRIVATE coact_core)
add_test(NAME test_pool_layout_throwing_ctor_neg
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test_pool_layout_throwing_ctor_neg)
set_tests_properties(test_pool_layout_throwing_ctor_neg PROPERTIES WILL_FAIL TRUE)

add_library(test_pool_zero_block_align_neg EXCLUDE_FROM_ALL STATIC
            pool_zero_block_align_neg.cpp)
target_link_libraries(test_pool_zero_block_align_neg PRIVATE coact_core)
add_test(NAME test_pool_zero_block_align_neg
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test_pool_zero_block_align_neg)
set_tests_properties(test_pool_zero_block_align_neg PROPERTIES WILL_FAIL TRUE)

add_library(test_pool_small_block_align_neg EXCLUDE_FROM_ALL STATIC
            pool_small_block_align_neg.cpp)
target_link_libraries(test_pool_small_block_align_neg PRIVATE coact_core)
add_test(NAME test_pool_small_block_align_neg
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test_pool_small_block_align_neg)
set_tests_properties(test_pool_small_block_align_neg PROPERTIES WILL_FAIL TRUE)
