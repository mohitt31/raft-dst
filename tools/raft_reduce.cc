// Shrinks a failing fuzz case to a small one that still fails the same way,
// so it can be committed as a regression test.
//
//   raft_reduce "seed=7 nodes=5 ..."
//
// Changing a knob changes the whole run, so a candidate is kept only if it
// still fails with the same kind of violation. The signature is the
// violation text with all digits removed, which drops times, node numbers
// and indices but keeps the wording.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "harness/fuzz_config.h"
#include "harness/scenario.h"

namespace {

using raftdst::harness::FuzzCase;
using raftdst::harness::ScenarioResult;

uint8_t g_mutation = 0;

ScenarioResult run(const FuzzCase& c) {
  auto scenario = c.to_scenario();
  scenario.raft.mutation = static_cast<raftdst::raft::Mutation>(g_mutation);
  return raftdst::harness::run_scenario(scenario);
}

std::string signature(const ScenarioResult& r) {
  if (r.violations.empty()) return {};
  std::string s;
  for (char ch : r.violations.front()) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) s += ch;
  }
  return s;
}

bool still_fails(const FuzzCase& c, const std::string& want) {
  const ScenarioResult r = run(c);
  return !r.ok() && signature(r) == want;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s \"seed=... nodes=...\" [--mutation N]\n", argv[0]);
    return 2;
  }
  for (int i = 2; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--mutation") == 0) {
      g_mutation = static_cast<uint8_t>(std::strtoul(argv[i + 1], nullptr, 10));
    }
  }
  FuzzCase c;
  if (!FuzzCase::parse(argv[1], c)) {
    std::fprintf(stderr, "could not parse case\n");
    return 2;
  }
  const ScenarioResult first = run(c);
  if (first.ok()) {
    std::fprintf(stderr, "this case does not fail\n");
    return 2;
  }
  const std::string want = signature(first);
  std::printf("reducing: %s\n", first.violations.front().c_str());

  uint64_t tried = 0, kept = 0;
  auto try_case = [&](FuzzCase candidate) {
    ++tried;
    if (!still_fails(candidate, want)) return false;
    c = candidate;
    ++kept;
    return true;
  };

  // 1. Shortest run that still fails, then the fault window inside it.
  for (uint32_t run = 5; run < c.run_seconds; run += 5) {
    FuzzCase t = c;
    t.run_seconds = run;
    t.fault_seconds = std::min(t.fault_seconds, run);
    if (try_case(t)) break;
  }
  // 2. Fewer nodes.
  if (c.nodes == 5) {
    FuzzCase t = c;
    t.nodes = 3;
    try_case(t);
  }
  // 3. Drop whole fault classes.
  {
    FuzzCase t = c;
    t.crashes = false;
    try_case(t);
  }
  {
    FuzzCase t = c;
    t.partitions = false;
    try_case(t);
  }
  for (uint32_t* rate : {&c.drop_ppm, &c.duplicate_ppm, &c.long_delay_ppm, &c.crash_in_sync_ppm}) {
    FuzzCase t = c;
    uint32_t* target =
        reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(&t) +
                                    (reinterpret_cast<char*>(rate) - reinterpret_cast<char*>(&c)));
    while (*target > 0) {
      const uint32_t saved = *target;
      *target = 0;
      if (try_case(t)) break;
      *target = saved / 2;
      if (*target == saved || !try_case(t)) break;
    }
  }
  // 4. Fewer keys and a slower client.
  for (uint32_t keys : {1u, 2u, 4u, 8u}) {
    if (keys >= c.keys) break;
    FuzzCase t = c;
    t.keys = keys;
    if (try_case(t)) break;
  }

  std::printf("tried %llu variants, kept %llu\n", static_cast<unsigned long long>(tried),
              static_cast<unsigned long long>(kept));
  std::printf("%s\n", c.to_line().c_str());
  return 0;
}
