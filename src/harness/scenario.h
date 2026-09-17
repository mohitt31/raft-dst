#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "harness/workload.h"
#include "raft/core.h"
#include "sim/faults.h"

namespace raftdst::harness {

struct ScenarioConfig {
  sim::SimConfig sim;
  raft::Config raft;
  WorkloadConfig workload;
  bool workload_enabled = true;
  // After faults stop and the last crashed node is back, the cluster gets
  // this long before commit liveness is measured.
  Duration settle = Duration::sec(5);
  // Proposals made after the settle point that must be acknowledged by the
  // end of the run.
  uint64_t min_acks_after_settle = 20;
  bool stop_at_first_violation = true;
};

struct ScenarioResult {
  std::vector<std::string> violations;
  Time first_violation_at = Time::max();
  Time ended_at;
  sim::SimStats sim;
  WorkloadStats workload;
  uint64_t fingerprint = 0;
  uint64_t committed_entries = 0;
  uint64_t leaders_elected = 0;

  [[nodiscard]] bool ok() const { return violations.empty(); }
};

// Runs one simulated cluster with the workload and returns what happened.
// Safety is checked continuously; at the end it checks liveness (only when
// faults stop before stop_at): every node is up and agrees on a leader, and
// proposals made after the settle point were acknowledged.
ScenarioResult run_scenario(const ScenarioConfig& config);

// The standard chaos profile used by tests, the fuzzer and benchmarks:
// every fault class on for `fault_window`, then a quiet period.
ScenarioConfig chaos_scenario(uint64_t seed, uint8_t nodes,
                              Duration fault_window = Duration::sec(30));

}  // namespace raftdst::harness
