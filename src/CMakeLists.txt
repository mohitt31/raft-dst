add_library(raftdst_common STATIC
  common/check.cc
  common/crc32c.cc
  common/entry_log.cc
  common/header_check.cc)
target_include_directories(raftdst_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(raftdst_common
  PUBLIC raftdst_options
  PRIVATE raftdst_warnings)

add_library(raftdst_sim STATIC
  sim/sim_storage.cc
  sim/trace.cc
  sim/world.cc)
target_link_libraries(raftdst_sim
  PUBLIC raftdst_common
  PRIVATE raftdst_warnings)

add_library(raftdst_raft STATIC
  raft/codec.cc
  raft/core.cc)
target_link_libraries(raftdst_raft
  PUBLIC raftdst_common
  PRIVATE raftdst_warnings)

add_library(raftdst_node STATIC
  node/node_host.cc)
target_link_libraries(raftdst_node
  PUBLIC raftdst_raft
  PRIVATE raftdst_warnings)

add_library(raftdst_kv STATIC
  kv/command.cc
  kv/kv_store.cc)
target_link_libraries(raftdst_kv
  PUBLIC raftdst_common
  PRIVATE raftdst_warnings)

add_library(raftdst_harness STATIC
  harness/checker.cc
  harness/fuzz_config.cc
  harness/scenario.cc
  harness/sim_cluster.cc
  harness/workload.cc)
target_link_libraries(raftdst_harness
  PUBLIC raftdst_node raftdst_sim raftdst_kv
  PRIVATE raftdst_warnings)
