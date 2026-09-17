#include "harness/sim_cluster.h"

#include <string>

namespace raftdst::harness {

SimCluster::SimCluster(const sim::SimConfig& sim, const raft::Config& raft) : raft_(raft) {
  world_ = std::make_unique<sim::World>(sim, [this](NodeContext& ctx) -> std::unique_ptr<INode> {
    return std::make_unique<node::NodeHost>(ctx, raft_, &checker_);
  });
}

node::NodeHost* SimCluster::host(NodeId id) const {
  return static_cast<node::NodeHost*>(world_->node(id));
}

std::optional<SimCluster::Leadership> SimCluster::agreed_leader() const {
  node::NodeHost* first = host(NodeId{0});
  if (!first) return std::nullopt;
  const Term term = first->core().term();
  const NodeId leader = first->core().leader();
  if (!leader.valid() || leader.get() >= size()) return std::nullopt;
  for (uint8_t i = 0; i < size(); ++i) {
    node::NodeHost* h = host(NodeId{i});
    if (!h || h->core().term() != term || h->core().leader() != leader) return std::nullopt;
  }
  if (host(leader)->core().role() != raft::Role::kLeader) return std::nullopt;
  return Leadership{leader, term};
}

void SimCluster::check_node_stats() {
  for (uint8_t i = 0; i < size(); ++i) {
    node::NodeHost* h = host(NodeId{i});
    if (!h) continue;
    const raft::CoreStats& c = h->core().stats();
    const std::string who = "n" + std::to_string(i) + ": ";
    const Time now = world_->now();
    if (c.append_from_same_term_leader)
      checker_.fail(now, who + "got appends from another leader of its own term");
    if (c.refused_committed_truncation)
      checker_.fail(now, who + "was asked to truncate committed entries");
    if (c.response_beyond_log) checker_.fail(now, who + "got an append response past its log");
    if (h->stats().frames_rejected)
      checker_.fail(now, who + "received frames that failed to decode");
    if (h->stats().frames_misrouted) checker_.fail(now, who + "received misrouted frames");
  }
}

}  // namespace raftdst::harness
