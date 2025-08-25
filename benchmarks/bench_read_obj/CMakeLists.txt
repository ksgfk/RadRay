add_executable(bench_read_obj bench_read_obj.cpp)
target_link_libraries(bench_read_obj PRIVATE radraycore benchmark::benchmark)
