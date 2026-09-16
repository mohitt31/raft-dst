#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>

namespace raftdst::testing {

inline uint64_t env_u64(const char* name, uint64_t fallback) {
  const char* v = std::getenv(name);
  return v ? std::strtoull(v, nullptr, 0) : fallback;
}

// Runs `body(seed)` for seeds base, base+1, ... . The base seed is fixed by
// default so CI is deterministic. On failure the message says how to replay
// just the failing case:
//   RAFTDST_SEED=<seed> RAFTDST_ITERS=1 ./tests/<binary> --gtest_filter=<test>
template <class F>
void for_each_seed(uint64_t default_iters, F&& body) {
  const uint64_t base = env_u64("RAFTDST_SEED", 1);
  const uint64_t iters = env_u64("RAFTDST_ITERS", default_iters);
  for (uint64_t i = 0; i < iters; ++i) {
    const uint64_t seed = base + i;
    SCOPED_TRACE("replay with RAFTDST_SEED=" + std::to_string(seed) + " RAFTDST_ITERS=1");
    body(seed);
    if (::testing::Test::HasFailure()) return;
  }
}

}  // namespace raftdst::testing
