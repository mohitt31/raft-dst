#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "node/node_host.h"

namespace raftdst::harness {

// Global safety checker. It sees every node's durable state changes and
// commits, and checks the Raft safety properties continuously, as they
// happen, not at the end of a run:
//
//   election safety        at most one leader per term
//   vote durability        a node never votes for two candidates in one term,
//                          even across crashes
//   term durability        a node's persisted term never goes backwards
//   state machine safety   every node commits the same entry at each index
//   leader completeness    a new leader's log holds every committed entry
//
// Violations are recorded, not asserted, so a fuzzer can report the seed
// and move on.
class Checker final : public node::IObserver {
 public:
  void on_recovered(Time now, NodeId node, const HardState& hs, const EntryLog& log) override;
  void on_hard_state_synced(Time now, NodeId node, const HardState& hs) override;
  void on_became_leader(Time now, NodeId node, Term term, const EntryLog& log) override;
  void on_commit(Time now, NodeId node, Index index, Term term,
                 std::span<const uint8_t> payload) override;

  // Record a violation found outside the observer hooks (e.g. liveness).
  void fail(Time now, std::string what);

  [[nodiscard]] bool ok() const { return violations_.empty(); }
  [[nodiscard]] const std::vector<std::string>& violations() const { return violations_; }
  [[nodiscard]] uint64_t committed_entries() const { return committed_.size(); }
  [[nodiscard]] uint64_t leaders_elected() const { return leader_of_.size(); }
  [[nodiscard]] uint64_t commit_reports() const { return commit_reports_; }

 private:
  struct Committed {
    Term term;
    uint64_t payload_hash;
  };

  void check_hard_state(Time now, NodeId node, const HardState& hs, const char* when);

  std::vector<std::string> violations_;
  std::array<Term, kMaxNodes> durable_term_{};
  std::array<NodeId, kMaxNodes> vote_in_durable_term_{};
  std::unordered_map<uint64_t, NodeId> leader_of_;  // looked up only, never iterated
  std::vector<Committed> committed_;                // index i at [i - 1]
  uint64_t commit_reports_ = 0;
};

}  // namespace raftdst::harness
