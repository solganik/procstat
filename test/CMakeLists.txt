add_executable (mytest test.c)
target_include_directories (mytest PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries (mytest PUBLIC
					   procstat_static
					   fuse pthread m)
