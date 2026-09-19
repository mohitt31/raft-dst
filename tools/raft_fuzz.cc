// Sweeps seeds through the simulator and reports any safety or liveness
// violation with a line that reproduces it.
//
//   raft_fuzz --seeds 1:1000            run seeds [1, 1000)
//   raft_fuzz --seeds 1:1000 --keep-going
//   raft_fuzz --case "seed=7 nodes=5 ..."   replay one case
//   raft_fuzz --mutation 3 --seeds 1:200    mutants build: how many seeds it
//                                           takes to catch planted bug 3
//   raft_fuzz --seeds 1:50 --fingerprints   one trace fingerprint per seed, to
//                                           compare runs across platforms
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "harness/fuzz_config.h"
#include "harness/scenario.h"

namespace {

using raftdst::harness::FuzzCase;
using raftdst::harness::ScenarioResult;

void report(const FuzzCase& c, const ScenarioResult& r) {
  std::printf("FAIL %s\n", c.to_line().c_str());
  for (const std::string& v : r.violations) std::printf("  %s\n", v.c_str());
  std::printf("  first violation at t=%lld ns, %llu entries committed, %llu leaders\n",
              static_cast<long long>(r.first_violation_at.ns_since_epoch()),
              static_cast<unsigned long long>(r.committed_entries),
              static_cast<unsigned long long>(r.leaders_elected));
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t from = 1, to = 1000;
  bool keep_going = false;
  const char* single = nullptr;
  uint8_t mutation = 0;
  bool fingerprints = false;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strcmp(a, "--seeds") == 0 && i + 1 < argc) {
      const char* range = argv[++i];
      from = std::strtoull(range, nullptr, 10);
      const char* colon = std::strchr(range, ':');
      to = colon ? std::strtoull(colon + 1, nullptr, 10) : from + 1;
    } else if (std::strcmp(a, "--keep-going") == 0) {
      keep_going = true;
    } else if (std::strcmp(a, "--case") == 0 && i + 1 < argc) {
      single = argv[++i];
    } else if (std::strcmp(a, "--fingerprints") == 0) {
      fingerprints = true;
    } else if (std::strcmp(a, "--mutation") == 0 && i + 1 < argc) {
      mutation = static_cast<uint8_t>(std::strtoul(argv[++i], nullptr, 10));
    } else {
      std::fprintf(stderr, "usage: %s [--seeds from:to] [--keep-going] [--case \"...\"]\n",
                   argv[0]);
      return 2;
    }
  }

  if (single) {
    FuzzCase c;
    if (!FuzzCase::parse(single, c)) {
      std::fprintf(stderr, "could not parse case\n");
      return 2;
    }
    auto scenario = c.to_scenario();
    scenario.raft.mutation = static_cast<raftdst::raft::Mutation>(mutation);
    const ScenarioResult r = raftdst::harness::run_scenario(scenario);
    if (r.ok()) {
      std::printf("ok %s\n", c.to_line().c_str());
      return 0;
    }
    report(c, r);
    return 1;
  }

  uint64_t failures = 0, committed = 0, acknowledged = 0, faults = 0;
  for (uint64_t seed = from; seed < to; ++seed) {
    const FuzzCase c = FuzzCase::from_seed(seed);
    auto scenario = c.to_scenario();
    scenario.raft.mutation = static_cast<raftdst::raft::Mutation>(mutation);
    const ScenarioResult r = raftdst::harness::run_scenario(scenario);
    if (fingerprints) {
      std::printf("%llu %016llx\n", static_cast<unsigned long long>(seed),
                  static_cast<unsigned long long>(r.fingerprint));
      continue;
    }
    committed += r.committed_entries;
    acknowledged += r.workload.acknowledged;
    faults += r.sim.injected_faults();
    if (!r.ok()) {
      ++failures;
      if (mutation != 0) {
        std::printf("mutation %u detected after %llu seeds (seed %llu): %s\n", mutation,
                    static_cast<unsigned long long>(seed - from + 1),
                    static_cast<unsigned long long>(seed), r.violations.front().c_str());
        return 1;
      }
      report(c, r);
      if (!keep_going) return 1;
    }
  }
  if (mutation != 0) {
    std::printf("mutation %u NOT detected in %llu seeds\n", mutation,
                static_cast<unsigned long long>(to - from));
    return 3;
  }
  if (fingerprints) return 0;
  std::printf(
      "%llu seeds, %llu failures, %llu entries committed, %llu writes acknowledged, "
      "%llu faults injected\n",
      static_cast<unsigned long long>(to - from), static_cast<unsigned long long>(failures),
      static_cast<unsigned long long>(committed), static_cast<unsigned long long>(acknowledged),
      static_cast<unsigned long long>(faults));
  return failures == 0 ? 0 : 1;
}
