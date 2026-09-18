#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "common/entry_log.h"
#include "common/types.h"
#include "env/node.h"
#include "env/state_machine.h"
#include "raft/core.h"

namespace raftdst::node {

// Hooks for an outside observer, such as the simulator's safety checker.
// Every call reports something that is already durable (or, for commits,
// already persisted and about to be applied), never a pending intention.
class IObserver {
 public:
  virtual ~IObserver() = default;
  // State loaded from storage when the node starts.
  virtual void on_recovered(Time now, NodeId node, const HardState& hs, const EntryLog& log) = 0;
  // Hard state after it was synced.
  virtual void on_hard_state_synced(Time now, NodeId node, const HardState& hs) = 0;
  // The node became leader of `term` (reported after its vote was synced).
  virtual void on_became_leader(Time now, NodeId node, Term term, const EntryLog& log) = 0;
  // Entry `index` is committed on `node` and is about to be applied.
  // `entry_term` is the term the entry was created in; `node_term` is the
  // reporting node's current term, which is an upper bound on the term of
  // the leader under which the entry became committed.
  virtual void on_commit(Time now, NodeId node, Index index, Term entry_term, Term node_term,
                         std::span<const uint8_t> payload) = 0;
  // Entry `index` was applied; `digest` is the state machine digest after it.
  virtual void on_applied(Time now, NodeId node, Index index, uint64_t digest) = 0;
};

struct ProposeOutcome {
  enum class Status : uint8_t { kAccepted, kNotLeader, kInvalidCommand };
  Status status = Status::kNotLeader;
  Index index;         // accepted: where the command was appended
  Term term;           // accepted: the term it was appended in
  NodeId leader_hint;  // not leader: who this node believes leads (may be none)
};

struct HostStats {
  uint64_t frames_rejected = 0;   // failed to decode
  uint64_t frames_misrouted = 0;  // header from/to disagree with the transport
  uint64_t syncs = 0;
  uint64_t messages_sent = 0;
};

// Runs a raft::Core against the environment interfaces. The same class runs
// in the simulator and in raftd. Each input is one step followed by one
// drain: persist, sync, send, apply, advance, re-arm the timer.
class NodeHost final : public INode {
 public:
  NodeHost(NodeContext& ctx, const raft::Config& config, std::unique_ptr<IStateMachine> sm,
           IObserver* observer);

  void on_frame(NodeId from, std::span<const uint8_t> frame) override;
  void on_wake() override;

  // The command counts as written once an entry with this index AND term is
  // committed. A different term at that index means it was lost.
  ProposeOutcome propose(std::span<const uint8_t> command);

  [[nodiscard]] const raft::Core& core() const { return core_; }
  [[nodiscard]] const IStateMachine& state_machine() const { return *sm_; }
  [[nodiscard]] Index applied() const { return applied_; }
  [[nodiscard]] const HostStats& stats() const { return stats_; }

 private:
  static Recovered load(NodeContext& ctx, IObserver* observer);
  void drain();

  NodeContext& ctx_;
  IObserver* observer_;
  raft::Core core_;
  std::unique_ptr<IStateMachine> sm_;
  HostStats stats_;
  std::vector<uint8_t> tx_;
  Index applied_;
  Term leader_reported_for_;
};

}  // namespace raftdst::node
