#include "harness/workload.h"

#include <algorithm>
#include <string>

#include "common/hash.h"
#include "kv/command.h"

namespace raftdst::harness {

Workload::Workload(const WorkloadConfig& config, uint64_t seed) : config_(config), rng_(seed) {
  RAFT_ASSERT(config_.keys >= 1, "workload needs at least one key");
  next_at_ = Time::zero() + Duration::ns(rng_.between(config_.interval_min.count_ns(),
                                                      config_.interval_max.count_ns()));
}

void Workload::step(SimCluster& cluster) {
  const Time now = cluster.world().now();
  next_at_ = now + Duration::ns(rng_.between(config_.interval_min.count_ns(),
                                             config_.interval_max.count_ns()));
  if (pending_.size() >= config_.max_pending) return;

  // Build the command. Every draw happens before the target is chosen, so
  // the command stream does not depend on cluster state.
  const std::string key = "k" + std::to_string(rng_.below(config_.keys));
  std::string value;
  const bool del = rng_.chance_ppm(config_.del_ppm);
  if (!del) {
    value.resize(rng_.below(config_.value_bytes_max + 1u));
    for (char& ch : value) ch = static_cast<char>('a' + rng_.below(26));
  }
  command_.clear();
  kv::encode_command({del ? kv::Op::kDel : kv::Op::kPut, key, value}, command_);
  const uint64_t pick = rng_.below(cluster.size());

  NodeId target = leader_hint_;
  if (!target.valid() || target.get() >= cluster.size() || !cluster.world().is_up(target)) {
    target = NodeId{static_cast<uint8_t>(pick)};
  }

  node::ProposeOutcome outcome;
  bool returned = false;
  const bool up =
      cluster.world().call_node(target, stats_.proposed + stats_.not_leader, [&](INode& n) {
        outcome = static_cast<node::NodeHost&>(n).propose(command_);
        returned = true;
      });
  if (!up) {
    ++stats_.target_down;
    leader_hint_ = NodeId::none();
    return;
  }
  if (!returned) {
    ++stats_.died_in_propose;  // may or may not have been written; not tracked
    leader_hint_ = NodeId::none();
    return;
  }
  switch (outcome.status) {
    case node::ProposeOutcome::Status::kAccepted: {
      ++stats_.proposed;
      leader_hint_ = target;
      Fnv1a64 h;
      h.add_bytes(command_);
      pending_.push_back({outcome.index, outcome.term, h.value(), now});
      break;
    }
    case node::ProposeOutcome::Status::kNotLeader:
      ++stats_.not_leader;
      leader_hint_ = outcome.leader_hint;
      break;
    case node::ProposeOutcome::Status::kInvalidCommand:
      RAFT_ASSERT(false, "workload built an invalid command");
  }
}

void Workload::resolve(const SimCluster& cluster) {
  const Checker& checker = cluster.checker();
  auto keep = std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) {
    const auto c = checker.committed(p.index);
    if (!c) return false;
    if (c->term == p.term) {
      // Same index and term means the same entry (log matching), so the
      // payload must match too; the checker would flag it otherwise.
      RAFT_ASSERT(c->payload_hash == p.payload_hash, "acknowledged entry has a different payload");
      ++stats_.acknowledged;
      const Duration latency = c->first_committed_at - p.proposed_at;
      stats_.ack_latency_total = stats_.ack_latency_total + latency;
      stats_.ack_latency_max = std::max(stats_.ack_latency_max, latency);
      acked_proposed_at_.push_back(p.proposed_at);
    } else {
      ++stats_.lost;
    }
    return true;
  });
  pending_.erase(keep, pending_.end());
}

uint64_t Workload::acknowledged_proposed_since(Time t) const {
  return static_cast<uint64_t>(std::count_if(acked_proposed_at_.begin(), acked_proposed_at_.end(),
                                             [&](Time at) { return at >= t; }));
}

}  // namespace raftdst::harness
