#pragma once

#include <memory>
#include <optional>

#include "harness/checker.h"
#include "node/node_host.h"
#include "raft/core.h"
#include "sim/world.h"

namespace raftdst::harness {

// A simulated Raft cluster: World + one NodeHost per node + the checker.
class SimCluster {
 public:
  SimCluster(const sim::SimConfig& sim, const raft::Config& raft);

  [[nodiscard]] sim::World& world() { return *world_; }
  [[nodiscard]] const sim::World& world() const { return *world_; }
  [[nodiscard]] Checker& checker() { return checker_; }
  [[nodiscard]] const Checker& checker() const { return checker_; }
  [[nodiscard]] uint8_t size() const { return world_->config().nodes; }

  // nullptr while the node is down.
  [[nodiscard]] node::NodeHost* host(NodeId id) const;

  struct Leadership {
    NodeId leader;
    Term term;
  };
  // Set when every node is up, all are in the same term, all name the same
  // leader, and that node is leader.
  [[nodiscard]] std::optional<Leadership> agreed_leader() const;

  // Adds a violation for every input a Core refused (see raft::CoreStats) or
  // frame a host could not decode. Call at the end of a run.
  void check_node_stats();

 private:
  raft::Config raft_;
  Checker checker_;
  std::unique_ptr<sim::World> world_;
};

}  // namespace raftdst::harness
