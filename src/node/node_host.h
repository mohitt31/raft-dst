#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "common/entry_log.h"
#include "common/types.h"
#include "env/node.h"
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
  // Entry `index` is committed on `node` and is being applied.
  virtual void on_commit(Time now, NodeId node, Index index, Term term,
                         std::span<const uint8_t> payload) = 0;
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
  NodeHost(NodeContext& ctx, const raft::Config& config, IObserver* observer);

  void on_frame(NodeId from, std::span<const uint8_t> frame) override;
  void on_wake() override;

  raft::ProposeResult propose(std::span<const uint8_t> command);

  [[nodiscard]] const raft::Core& core() const { return core_; }
  [[nodiscard]] const HostStats& stats() const { return stats_; }

 private:
  static Recovered load(NodeContext& ctx, IObserver* observer);
  void drain();

  NodeContext& ctx_;
  IObserver* observer_;
  raft::Core core_;
  HostStats stats_;
  std::vector<uint8_t> tx_;
  Index reported_commit_;
  Term leader_reported_for_;
};

}  // namespace raftdst::node
