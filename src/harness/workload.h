#pragma once

#include <cstdint>
#include <vector>

#include "common/rng.h"
#include "common/types.h"
#include "harness/sim_cluster.h"

namespace raftdst::harness {

struct WorkloadConfig {
  Duration interval_min = Duration::ms(5);  // between client operations
  Duration interval_max = Duration::ms(15);
  uint32_t keys = 32;  // small key space, so writes overwrite and delete
  uint32_t value_bytes_max = 32;
  uint32_t del_ppm = 200'000;
  uint32_t max_pending = 4096;  // stop proposing if this many are unresolved
};

struct WorkloadStats {
  uint64_t proposed = 0;         // accepted by a leader
  uint64_t not_leader = 0;       // sent to a node that was not leader
  uint64_t target_down = 0;      // chosen node was down
  uint64_t died_in_propose = 0;  // node crashed while handling the proposal
  uint64_t acknowledged = 0;     // committed at the returned (index, term)
  uint64_t lost = 0;             // a different term was committed at that index
  Duration ack_latency_total;    // virtual time, proposal to first commit
  Duration ack_latency_max;

  friend bool operator==(const WorkloadStats&, const WorkloadStats&) = default;
};

// Open-loop client: proposes a random put or delete at randomized intervals,
// whether or not earlier ones have finished. It aims at the node it last
// learned leads, otherwise at a random node, and learns from NotLeader hints.
// No sessions and no retries (both out of scope): a lost write is counted,
// not repeated.
class Workload {
 public:
  Workload(const WorkloadConfig& config, uint64_t seed);

  [[nodiscard]] Time next_at() const { return next_at_; }

  // Performs the operation due at next_at() and schedules the next one.
  void step(SimCluster& cluster);

  // Resolves pending proposals against what the checker has seen committed.
  void resolve(const SimCluster& cluster);

  [[nodiscard]] const WorkloadStats& stats() const { return stats_; }
  // Acknowledged proposals that were made at or after `t`.
  [[nodiscard]] uint64_t acknowledged_proposed_since(Time t) const;
  [[nodiscard]] uint64_t pending() const { return pending_.size(); }

 private:
  struct Pending {
    Index index;
    Term term;
    uint64_t payload_hash;
    Time proposed_at;
  };

  WorkloadConfig config_;
  Rng rng_;
  Time next_at_;
  NodeId leader_hint_ = NodeId::none();
  std::vector<Pending> pending_;
  std::vector<Time> acked_proposed_at_;
  std::vector<uint8_t> command_;
  WorkloadStats stats_;
};

}  // namespace raftdst::harness
