#include "harness/checker.h"

#include <string>

#include "common/hash.h"

namespace raftdst::harness {

namespace {

uint64_t hash_of(std::span<const uint8_t> bytes) {
  Fnv1a64 h;
  h.add_bytes(bytes);
  return h.value();
}

std::string at(Time now, NodeId node) {
  return "t=" + std::to_string(now.ns_since_epoch()) + " n" + std::to_string(node.get()) + ": ";
}

}  // namespace

void Checker::fail(Time now, std::string what) {
  violations_.push_back("t=" + std::to_string(now.ns_since_epoch()) + ": " + std::move(what));
}

void Checker::check_hard_state(Time now, NodeId node, const HardState& hs, const char* when) {
  const uint8_t i = node.get();
  if (hs.term < durable_term_[i]) {
    violations_.push_back(at(now, node) + "durable term went back from " +
                          std::to_string(durable_term_[i].get()) + " to " +
                          std::to_string(hs.term.get()) + " (" + when + ")");
    return;
  }
  if (hs.term > durable_term_[i]) {
    // A new term starts with whatever vote was persisted alongside it.
    durable_term_[i] = hs.term;
    vote_in_durable_term_[i] = hs.voted_for;
    return;
  }
  // Same term: a recorded vote may never change or disappear.
  const NodeId recorded = vote_in_durable_term_[i];
  if (recorded != NodeId::none() && hs.voted_for != recorded) {
    const std::string now_vote = hs.voted_for == NodeId::none()
                                     ? std::string("none")
                                     : "n" + std::to_string(hs.voted_for.get());
    violations_.push_back(at(now, node) + "vote in term " + std::to_string(hs.term.get()) +
                          " changed from n" + std::to_string(recorded.get()) + " to " + now_vote +
                          " (" + when + ")");
    return;
  }
  vote_in_durable_term_[i] = hs.voted_for;
}

void Checker::on_recovered(Time now, NodeId node, const HardState& hs, const EntryLog& log) {
  check_hard_state(now, node, hs, "on restart");
  if (!log.invariants_hold()) violations_.push_back(at(now, node) + "recovered log is malformed");
}

void Checker::on_hard_state_synced(Time now, NodeId node, const HardState& hs) {
  check_hard_state(now, node, hs, "after sync");
}

void Checker::on_became_leader(Time now, NodeId node, Term term, const EntryLog& log) {
  auto [it, inserted] = leader_of_.try_emplace(term.get(), node);
  if (!inserted && it->second != node) {
    violations_.push_back(at(now, node) + "second leader in term " + std::to_string(term.get()) +
                          " (first was n" + std::to_string(it->second.get()) + ")");
  }
  for (size_t i = 0; i < committed_.size(); ++i) {
    const Index idx{i + 1};
    if (!log.has(idx) || log.term_at(idx) != committed_[i].term ||
        hash_of(log.payload_at(idx)) != committed_[i].payload_hash) {
      violations_.push_back(at(now, node) + "leader of term " + std::to_string(term.get()) +
                            " is missing committed entry " + std::to_string(idx.get()));
      return;
    }
  }
}

void Checker::on_commit(Time now, NodeId node, Index index, Term term,
                        std::span<const uint8_t> payload) {
  ++commit_reports_;
  const uint64_t h = hash_of(payload);
  const uint64_t i = index.get();
  if (i == committed_.size() + 1) {
    committed_.push_back({term, h});
    return;
  }
  if (i > committed_.size() + 1) {
    // A node can learn a commit index before anyone reported the indices in
    // between only if it skipped them, which the host never does.
    violations_.push_back(at(now, node) + "commit of index " + std::to_string(i) +
                          " before index " + std::to_string(committed_.size() + 1));
    return;
  }
  const Committed& c = committed_[i - 1];
  if (c.term != term || c.payload_hash != h) {
    violations_.push_back(at(now, node) + "committed a different entry at index " +
                          std::to_string(i) + " (term " + std::to_string(term.get()) +
                          ", first committed with term " + std::to_string(c.term.get()) + ")");
  }
}

}  // namespace raftdst::harness
