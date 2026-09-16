add_library(raftdst_common STATIC
  common/check.cc)
target_include_directories(raftdst_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(raftdst_common
  PUBLIC raftdst_options
  PRIVATE raftdst_warnings)
