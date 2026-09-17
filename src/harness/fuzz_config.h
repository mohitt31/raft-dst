#pragma once

#include <cstdint>
#include <string>

#include "harness/scenario.h"

namespace raftdst::harness {

// One fuzz case: a seed plus every knob the fuzzer varies. Fixing the
// configuration across seeds hides bugs that only appear at, say, a batch
// size of one, so each seed draws its own (swarm testing). Every field is
// printable and parseable, so a failing case is one line that replays exactly.
struct FuzzCase {
  uint64_t seed = 1;
  uint8_t nodes = 3;
  uint64_t max_entries_per_append = 64;
  uint32_t election_timeout_min_ms = 150;
  uint32_t election_timeout_max_ms = 300;
  uint32_t heartbeat_ms = 50;
  uint32_t delay_min_us = 1000;
  uint32_t delay_max_us = 5000;
  uint32_t drop_ppm = 50'000;
  uint32_t duplicate_ppm = 20'000;
  uint32_t long_delay_ppm = 30'000;
  uint32_t crash_in_sync_ppm = 300'000;
  uint32_t crash_gap_min_ms = 300;
  uint32_t crash_gap_max_ms = 3000;
  uint32_t partition_gap_min_ms = 500;
  uint32_t partition_gap_max_ms = 4000;
  uint32_t workload_interval_min_us = 5000;
  uint32_t workload_interval_max_us = 15000;
  uint32_t keys = 32;
  bool crashes = true;
  bool partitions = true;
  bool workload = true;
  uint32_t fault_seconds = 30;
  uint32_t run_seconds = 48;

  // Draws every knob from `seed`, so one number describes a whole case.
  static FuzzCase from_seed(uint64_t seed);

  [[nodiscard]] ScenarioConfig to_scenario() const;

  // "seed=7 nodes=5 max_entries=3 ..." - one line, parseable by parse().
  [[nodiscard]] std::string to_line() const;
  static bool parse(const std::string& line, FuzzCase& out);
};

}  // namespace raftdst::harness
