coact_add_test(test_staging test_staging.cpp)
find_package(Threads REQUIRED)
target_link_libraries(test_staging PRIVATE Threads::Threads)
