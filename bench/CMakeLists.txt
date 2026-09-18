find_package(Threads REQUIRED)

add_executable(sim_bench sim_bench.cc)
target_link_libraries(sim_bench PRIVATE raftdst_harness raftdst_test_warnings)

add_subdirectory(cluster)
