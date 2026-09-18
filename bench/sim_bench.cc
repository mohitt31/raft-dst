// Measures how much simulated cluster time the deterministic simulator runs
// per wall-clock minute, and how many faults it injects per wall second.
//
// Honest-measurement rules this follows:
//   - release build (-O2), warmup pass first, several repetitions, median
//   - identical work in every repetition (same seeds, same profile), so the
//     spread is the machine's, not the workload's
//   - results are consumed and printed, so nothing can be optimised away
//   - the safety checker stays ON, because that is how the fuzzer runs
//
// This is the only place in the project that reads a real clock.
#include <algorithm>
#include <chrono>
#include <cstdint>
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

struct Totals {
  double seconds = 0;
  uint64_t simulated_node_seconds = 0;
  uint64_t events = 0;
  uint64_t faults = 0;
  uint64_t committed = 0;
  uint64_t acknowledged = 0;
  uint64_t fingerprint = 0;  // accumulated so the work cannot be elided
  uint64_t violations = 0;
};

Totals run_pass(uint64_t from, uint64_t to, uint8_t nodes) {
  Totals t;
  const auto start = std::chrono::steady_clock::now();
  for (uint64_t seed = from; seed < to; ++seed) {
    FuzzCase c = FuzzCase::from_seed(seed);
    c.nodes = nodes;  // fixed, so cluster-hours are comparable across passes
    const ScenarioResult r = raftdst::harness::run_scenario(c.to_scenario());
    t.simulated_node_seconds +=
        static_cast<uint64_t>(r.ended_at.ns_since_epoch() / 1'000'000'000) * nodes;
    t.events += r.sim.events;
    t.faults += r.sim.injected_faults();
    t.committed += r.committed_entries;
    t.acknowledged += r.workload.acknowledged;
    t.fingerprint ^= r.fingerprint;
    t.violations += r.violations.size();
  }
  const auto end = std::chrono::steady_clock::now();
  t.seconds = std::chrono::duration<double>(end - start).count();
  return t;
}

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t seeds = 100;
  int repeats = 5;
  uint8_t nodes = 5;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--seeds") == 0 && i + 1 < argc) {
      seeds = std::strtoull(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--repeats") == 0 && i + 1 < argc) {
      repeats = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--nodes") == 0 && i + 1 < argc) {
      nodes = static_cast<uint8_t>(std::atoi(argv[++i]));
    } else {
      std::fprintf(stderr, "usage: %s [--seeds N] [--repeats N] [--nodes N]\n", argv[0]);
      return 2;
    }
  }

  run_pass(1, 1 + seeds / 4, nodes);  // warmup: fills caches, touches every path

  std::vector<double> hours_per_minute, faults_per_second, events_per_second;
  Totals last;
  for (int i = 0; i < repeats; ++i) {
    const Totals t = run_pass(1, 1 + seeds, nodes);
    const double cluster_hours = static_cast<double>(t.simulated_node_seconds) / 3600.0;
    hours_per_minute.push_back(cluster_hours / (t.seconds / 60.0));
    faults_per_second.push_back(static_cast<double>(t.faults) / t.seconds);
    events_per_second.push_back(static_cast<double>(t.events) / t.seconds);
    last = t;
  }

  std::printf("seeds=%llu nodes=%u repeats=%d\n", static_cast<unsigned long long>(seeds), nodes,
              repeats);
  std::printf("simulated_cluster_hours_per_wall_minute %.1f\n", median(hours_per_minute));
  std::printf("injected_faults_per_wall_second %.0f\n", median(faults_per_second));
  std::printf("simulator_events_per_wall_second %.0f\n", median(events_per_second));
  std::printf(
      "per_pass: wall_seconds=%.2f events=%llu faults=%llu committed=%llu acked=%llu "
      "violations=%llu fingerprint=%016llx\n",
      last.seconds, static_cast<unsigned long long>(last.events),
      static_cast<unsigned long long>(last.faults), static_cast<unsigned long long>(last.committed),
      static_cast<unsigned long long>(last.acknowledged),
      static_cast<unsigned long long>(last.violations),
      static_cast<unsigned long long>(last.fingerprint));
  return last.violations == 0 ? 0 : 1;
}
