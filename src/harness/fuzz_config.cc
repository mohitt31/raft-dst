#include "harness/fuzz_config.h"

#include <cstdlib>
#include <sstream>
#include <type_traits>
#include <unordered_map>

#include "common/rng.h"

namespace raftdst::harness {

namespace {
// A distinct stream from anything the simulator uses, so drawing the
// configuration never shifts the run's own randomness.
constexpr uint64_t kConfigStream = 0xC0FFEE;
}  // namespace

FuzzCase FuzzCase::from_seed(uint64_t seed) {
  Rng r = Rng::derive(seed, kConfigStream);
  FuzzCase c;
  c.seed = seed;
  c.nodes = static_cast<uint8_t>(r.below(2) ? 3 : 5);
  // Small batches split leader no-ops from earlier entries, which is where
  // the paper's Figure 8 case lives.
  c.max_entries_per_append = r.below(2) ? 1 + r.below(8) : 1 + r.below(256);
  c.election_timeout_min_ms = static_cast<uint32_t>(60 + r.below(200));
  c.election_timeout_max_ms = c.election_timeout_min_ms + static_cast<uint32_t>(40 + r.below(300));
  c.heartbeat_ms = static_cast<uint32_t>(10 + r.below(c.election_timeout_min_ms / 2));
  c.delay_min_us = static_cast<uint32_t>(100 + r.below(3000));
  c.delay_max_us = c.delay_min_us + static_cast<uint32_t>(r.below(20000));
  c.drop_ppm = static_cast<uint32_t>(r.below(250'000));
  c.duplicate_ppm = static_cast<uint32_t>(r.below(150'000));
  c.long_delay_ppm = static_cast<uint32_t>(r.below(150'000));
  c.crashes = r.below(8) != 0;
  c.partitions = r.below(8) != 0;
  c.crash_in_sync_ppm = static_cast<uint32_t>(r.below(600'000));
  c.crash_gap_min_ms = static_cast<uint32_t>(100 + r.below(1500));
  c.crash_gap_max_ms = c.crash_gap_min_ms + static_cast<uint32_t>(100 + r.below(4000));
  c.partition_gap_min_ms = static_cast<uint32_t>(100 + r.below(2000));
  c.partition_gap_max_ms = c.partition_gap_min_ms + static_cast<uint32_t>(100 + r.below(5000));
  c.workload_interval_min_us = static_cast<uint32_t>(200 + r.below(8000));
  c.workload_interval_max_us = c.workload_interval_min_us + static_cast<uint32_t>(r.below(10000));
  c.keys = static_cast<uint32_t>(1 + r.below(64));
  c.fault_seconds = static_cast<uint32_t>(10 + r.below(40));
  c.run_seconds = c.fault_seconds + 18;
  return c;
}

ScenarioConfig FuzzCase::to_scenario() const {
  ScenarioConfig s;
  s.sim.seed = seed;
  s.sim.nodes = nodes;
  s.sim.net.delay_min = Duration::us(delay_min_us);
  s.sim.net.delay_max = Duration::us(delay_max_us);
  s.sim.net.drop_ppm = drop_ppm;
  s.sim.net.duplicate_ppm = duplicate_ppm;
  s.sim.net.long_delay_ppm = long_delay_ppm;
  s.sim.crash.enabled = crashes;
  s.sim.crash.gap_min = Duration::ms(crash_gap_min_ms);
  s.sim.crash.gap_max = Duration::ms(crash_gap_max_ms);
  s.sim.crash.in_sync_ppm = crash_in_sync_ppm;
  s.sim.partition.enabled = partitions;
  s.sim.partition.gap_min = Duration::ms(partition_gap_min_ms);
  s.sim.partition.gap_max = Duration::ms(partition_gap_max_ms);
  s.sim.faults_until = Time::zero() + Duration::sec(fault_seconds);
  s.sim.stop_at = Time::zero() + Duration::sec(run_seconds);
  s.raft.election_timeout_min = Duration::ms(election_timeout_min_ms);
  s.raft.election_timeout_max = Duration::ms(election_timeout_max_ms);
  s.raft.heartbeat_interval = Duration::ms(heartbeat_ms);
  s.raft.max_entries_per_append = max_entries_per_append;
  s.workload.interval_min = Duration::us(workload_interval_min_us);
  s.workload.interval_max = Duration::us(workload_interval_max_us);
  s.workload.keys = keys;
  s.workload_enabled = workload;
  return s;
}

std::string FuzzCase::to_line() const {
  std::ostringstream o;
  o << "seed=" << seed << " nodes=" << static_cast<int>(nodes)
    << " max_entries=" << max_entries_per_append << " elect_min_ms=" << election_timeout_min_ms
    << " elect_max_ms=" << election_timeout_max_ms << " heartbeat_ms=" << heartbeat_ms
    << " delay_min_us=" << delay_min_us << " delay_max_us=" << delay_max_us
    << " drop_ppm=" << drop_ppm << " dup_ppm=" << duplicate_ppm
    << " long_delay_ppm=" << long_delay_ppm << " crashes=" << (crashes ? 1 : 0)
    << " crash_in_sync_ppm=" << crash_in_sync_ppm << " crash_gap_min_ms=" << crash_gap_min_ms
    << " crash_gap_max_ms=" << crash_gap_max_ms << " partitions=" << (partitions ? 1 : 0)
    << " part_gap_min_ms=" << partition_gap_min_ms << " part_gap_max_ms=" << partition_gap_max_ms
    << " wl_min_us=" << workload_interval_min_us << " wl_max_us=" << workload_interval_max_us
    << " keys=" << keys << " workload=" << (workload ? 1 : 0) << " fault_s=" << fault_seconds
    << " run_s=" << run_seconds;
  return o.str();
}

bool FuzzCase::parse(const std::string& line, FuzzCase& out) {
  std::unordered_map<std::string, uint64_t> kv;
  std::istringstream in{line};
  std::string token;
  while (in >> token) {
    if (token.empty() || token[0] == '#') break;
    const size_t eq = token.find('=');
    if (eq == std::string::npos) return false;
    kv[token.substr(0, eq)] = std::strtoull(token.c_str() + eq + 1, nullptr, 10);
  }
  if (kv.find("seed") == kv.end()) return false;
  FuzzCase c;
  auto get = [&](const char* name, auto& field) {
    auto it = kv.find(name);
    if (it != kv.end()) field = static_cast<std::decay_t<decltype(field)>>(it->second);
  };
  auto get_bool = [&](const char* name, bool& field) {
    auto it = kv.find(name);
    if (it != kv.end()) field = it->second != 0;
  };
  get("seed", c.seed);
  get("nodes", c.nodes);
  get("max_entries", c.max_entries_per_append);
  get("elect_min_ms", c.election_timeout_min_ms);
  get("elect_max_ms", c.election_timeout_max_ms);
  get("heartbeat_ms", c.heartbeat_ms);
  get("delay_min_us", c.delay_min_us);
  get("delay_max_us", c.delay_max_us);
  get("drop_ppm", c.drop_ppm);
  get("dup_ppm", c.duplicate_ppm);
  get("long_delay_ppm", c.long_delay_ppm);
  get_bool("crashes", c.crashes);
  get("crash_in_sync_ppm", c.crash_in_sync_ppm);
  get("crash_gap_min_ms", c.crash_gap_min_ms);
  get("crash_gap_max_ms", c.crash_gap_max_ms);
  get_bool("partitions", c.partitions);
  get("part_gap_min_ms", c.partition_gap_min_ms);
  get("part_gap_max_ms", c.partition_gap_max_ms);
  get("wl_min_us", c.workload_interval_min_us);
  get("wl_max_us", c.workload_interval_max_us);
  get("keys", c.keys);
  get_bool("workload", c.workload);
  get("fault_s", c.fault_seconds);
  get("run_s", c.run_seconds);
  out = c;
  return true;
}

}  // namespace raftdst::harness
