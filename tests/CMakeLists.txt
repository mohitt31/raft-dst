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
  # PRE_TEST: discover when ctest runs, not at build time, so a sanitizer
  # abort shows up as a failing test instead of a broken build.
  gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
endfunction()

raftdst_test(unit_types SOURCES unit/types_test.cc LIBS raftdst_common)
raftdst_test(unit_check SOURCES unit/check_test.cc LIBS raftdst_common)

# Determinism lint: banned APIs in code that runs inside the simulator.
set(LINT_DIRS common env raft kv node sim)
list(TRANSFORM LINT_DIRS PREPEND ${PROJECT_SOURCE_DIR}/src/)
add_test(NAME lint.banned_apis
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/lint/banned_apis.sh ${LINT_DIRS})
# The lint must actually catch things: every code line in the fixture has to hit.
add_test(NAME lint.selftest
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/lint/banned_apis.sh --selftest
          ${CMAKE_CURRENT_SOURCE_DIR}/lint/fixtures)
