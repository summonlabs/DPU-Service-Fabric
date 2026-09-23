add_executable(dpuf_benchmarks dpuf_benchmarks.cpp)
target_link_libraries(dpuf_benchmarks PRIVATE dpu::fabric)
dpuf_apply_warnings(dpuf_benchmarks)
