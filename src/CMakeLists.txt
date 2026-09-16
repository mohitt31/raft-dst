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
  raft/codec.cc)
target_link_libraries(raftdst_raft
  PUBLIC raftdst_common
  PRIVATE raftdst_warnings)
