enable_testing()
find_package(GTest REQUIRED)
find_package(Boost COMPONENTS system filesystem REQUIRED)


include_directories(${GTEST_INCLUDE_DIR})

add_executable(procstat_test test.cpp test_c.cpp)
target_link_libraries(procstat_test GTest::GTest GTest::Main procstat_static fuse pthread m Boost::filesystem)
add_test(NAME procstat_test
        COMMAND procstat_test)
