add_executable(raft_fuzz raft_fuzz.cc)
target_link_libraries(raft_fuzz PRIVATE raftdst_harness raftdst_warnings)

add_executable(raft_reduce raft_reduce.cc)
target_link_libraries(raft_reduce PRIVATE raftdst_harness raftdst_warnings)

add_executable(raftd raftd.cc)
target_link_libraries(raftd PRIVATE raftdst_real raftdst_warnings)

add_executable(raft_client raft_client.cc)
target_link_libraries(raft_client PRIVATE raftdst_real raftdst_warnings)
