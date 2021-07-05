cmake_minimum_required (VERSION 2.8.11)
project (procstat)

enable_testing()

add_subdirectory (src)
if(NOT DEFINED Procstat_BUILD_TESTS)
    set(Procstat_BUILD_TESTS ON)
endif()
if(${Procstat_BUILD_TESTS})
    add_subdirectory (test)
endif()