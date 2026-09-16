include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
  URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
  DOWNLOAD_EXTRACT_TIMESTAMP ON)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# raftdst_test(<name> SOURCES ... [LIBS ...])
function(raftdst_test name)
  cmake_parse_arguments(T "" "" "SOURCES;LIBS" ${ARGN})
  add_executable(${name} ${T_SOURCES})
  target_link_libraries(${name} PRIVATE ${T_LIBS} GTest::gtest_main raftdst_test_warnings)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  # PRE_TEST: discover when ctest runs, not at build time, so a sanitizer
  # abort shows up as a failing test instead of a broken build.
  gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
endfunction()

raftdst_test(unit_types SOURCES unit/types_test.cc LIBS raftdst_common)
raftdst_test(unit_check SOURCES unit/check_test.cc LIBS raftdst_common)
raftdst_test(unit_rng SOURCES unit/rng_test.cc LIBS raftdst_common)
raftdst_test(unit_hash_crc SOURCES unit/hash_crc_test.cc LIBS raftdst_common)
raftdst_test(unit_bytes SOURCES unit/bytes_test.cc LIBS raftdst_common)
raftdst_test(unit_entry_log SOURCES unit/entry_log_test.cc LIBS raftdst_common)
raftdst_test(unit_event_queue SOURCES unit/event_queue_test.cc LIBS raftdst_sim)
raftdst_test(unit_sim_storage SOURCES unit/sim_storage_test.cc LIBS raftdst_sim)
raftdst_test(sim_world SOURCES sim/world_test.cc LIBS raftdst_sim)
raftdst_test(sim_determinism SOURCES sim/determinism_test.cc LIBS raftdst_sim)

# Determinism lint: banned APIs in code that runs inside the simulator.
set(LINT_DIRS common env raft kv node sim)
list(TRANSFORM LINT_DIRS PREPEND ${PROJECT_SOURCE_DIR}/src/)
add_test(NAME lint.banned_apis
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/lint/banned_apis.sh ${LINT_DIRS})
# The lint must actually catch things: every code line in the fixture has to hit.
add_test(NAME lint.selftest
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/lint/banned_apis.sh --selftest
          ${CMAKE_CURRENT_SOURCE_DIR}/lint/fixtures)

# Same seed, two separate processes, identical fingerprint.
add_executable(ping_fingerprint sim/ping_fingerprint.cc)
target_include_directories(ping_fingerprint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ping_fingerprint PRIVATE raftdst_sim raftdst_test_warnings)
add_test(NAME sim.cross_process_determinism
  COMMAND bash -c "a=$($<TARGET_FILE:ping_fingerprint> 42) && b=$($<TARGET_FILE:ping_fingerprint> 42) && echo \"$a / $b\" && test \"$a\" = \"$b\"")
