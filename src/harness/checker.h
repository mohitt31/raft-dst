#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "node/node_host.h"

namespace raftdst::harness {

// Global safety checker. It sees every node's durable state changes, commits
// and applies, and checks the Raft safety properties continuously, as they
// happen, not at the end of a run:
//
//   election safety        at most one leader per term
//   vote durability        a node never votes for two candidates in one term,
//                          even across crashes
//   term durability        a node's persisted term never goes backwards
//   state machine safety   every node commits the same entry at each index
//   leader completeness    a new leader's log holds every committed entry
//   applied state          after applying index i, every node's KV digest
//                          equals the digest of a reference model that has
//                          applied the committed entries 1..i
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
  void on_applied(Time now, NodeId node, Index index, uint64_t digest) override;

  // Record a violation found outside the observer hooks (e.g. liveness).
  void fail(Time now, std::string what);

  struct Committed {
    Term term;
    uint64_t payload_hash;
    Time first_committed_at;
  };
  // The entry committed at `index`, if any node has committed it yet.
  [[nodiscard]] std::optional<Committed> committed(Index index) const;

  [[nodiscard]] bool ok() const { return violations_.empty(); }
  [[nodiscard]] const std::vector<std::string>& violations() const { return violations_; }
  [[nodiscard]] Time first_violation_at() const { return first_violation_at_; }
  [[nodiscard]] uint64_t committed_entries() const { return committed_.size(); }
  [[nodiscard]] uint64_t leaders_elected() const { return leader_of_.size(); }
  [[nodiscard]] uint64_t commit_reports() const { return commit_reports_; }
  [[nodiscard]] uint64_t apply_reports() const { return apply_reports_; }

 private:
  void check_hard_state(Time now, NodeId node, const HardState& hs, const char* when);
  void violation(Time now, std::string what);
  void apply_to_model(std::span<const uint8_t> payload);

  std::vector<std::string> violations_;
  Time first_violation_at_ = Time::max();
  std::array<Term, kMaxNodes> durable_term_{};
  std::array<NodeId, kMaxNodes> vote_in_durable_term_{};
  std::unordered_map<uint64_t, NodeId> leader_of_;  // looked up only, never iterated
  std::vector<Committed> committed_;                // index i at [i - 1]
  uint64_t commit_reports_ = 0;
  uint64_t apply_reports_ = 0;

  // Reference key-value model: an ordered map, digest recomputed from
  // scratch after every committed entry. A differential check against each
  // node's incrementally maintained KvStore digest.
  std::map<std::string, std::string, std::less<>> model_;
  std::vector<uint64_t> model_digest_;  // digest after applying index i at [i - 1]
};

}  // namespace raftdst::harness
