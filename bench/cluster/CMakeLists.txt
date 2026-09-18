add_executable(load_gen load_gen.cc)
target_link_libraries(load_gen PRIVATE raftdst_real raftdst_test_warnings Threads::Threads)
