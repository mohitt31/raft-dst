cmake_minimum_required(VERSION 3.25)
project(raftdst LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RAFTDST_DCHECKS "Enable RAFT_DCHECK internal invariants" OFF)
option(RAFTDST_MUTANTS "Compile planted-bug switches (never in release)" OFF)
option(RAFTDST_WERROR "Treat warnings as errors" ON)
option(RAFTDST_BUILD_TESTS "Build tests" ON)

# Strict warnings for project code. Tests get a lighter set because gtest
# macros trip -Wconversion and friends.
add_library(raftdst_warnings INTERFACE)
target_compile_options(raftdst_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wold-style-cast -Wnon-virtual-dtor -Wcast-align -Wimplicit-fallthrough
  -Woverloaded-virtual -Wnull-dereference
  $<$<BOOL:${RAFTDST_WERROR}>:-Werror>)

add_library(raftdst_test_warnings INTERFACE)
target_compile_options(raftdst_test_warnings INTERFACE
  -Wall -Wextra $<$<BOOL:${RAFTDST_WERROR}>:-Werror>)

add_library(raftdst_options INTERFACE)
target_compile_definitions(raftdst_options INTERFACE
  $<$<BOOL:${RAFTDST_DCHECKS}>:RAFTDST_DCHECKS>
  $<$<BOOL:${RAFTDST_MUTANTS}>:RAFTDST_MUTANTS>)

add_subdirectory(src)
add_subdirectory(tools)
add_subdirectory(bench)

if(RAFTDST_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
