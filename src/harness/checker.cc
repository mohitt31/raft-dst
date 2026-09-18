#include "harness/checker.h"

#include <string>

#include "common/hash.h"
#include "kv/command.h"
#include "kv/kv_store.h"

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
  violation(now, "t=" + std::to_string(now.ns_since_epoch()) + ": " + std::move(what));
}

void Checker::violation(Time now, std::string what) {
  if (violations_.empty()) first_violation_at_ = now;
  violations_.push_back(std::move(what));
}

std::optional<Checker::Committed> Checker::committed(Index index) const {
  if (index.get() == 0 || index.get() > committed_.size()) return std::nullopt;
  return committed_[index.get() - 1];
}

void Checker::check_hard_state(Time now, NodeId node, const HardState& hs, const char* when) {
  const uint8_t i = node.get();
  if (hs.term < durable_term_[i]) {
    violation(now, at(now, node) + "durable term went back from " +
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
    violation(now, at(now, node) + "vote in term " + std::to_string(hs.term.get()) +
                       " changed from n" + std::to_string(recorded.get()) + " to " + now_vote +
                       " (" + when + ")");
    return;
  }
  vote_in_durable_term_[i] = hs.voted_for;
}

void Checker::on_recovered(Time now, NodeId node, const HardState& hs, const EntryLog& log) {
  if (listener_) listener_->on_recovered(now, node, hs, log);
  check_hard_state(now, node, hs, "on restart");
  if (!log.invariants_hold()) violation(now, at(now, node) + "recovered log is malformed");
}

void Checker::on_hard_state_synced(Time now, NodeId node, const HardState& hs) {
  if (listener_) listener_->on_hard_state_synced(now, node, hs);
  check_hard_state(now, node, hs, "after sync");
}

void Checker::on_became_leader(Time now, NodeId node, Term term, const EntryLog& log) {
  if (listener_) listener_->on_became_leader(now, node, term, log);
  auto [it, inserted] = leader_of_.try_emplace(term.get(), node);
  if (!inserted && it->second != node) {
    violation(now, at(now, node) + "second leader in term " + std::to_string(term.get()) +
                       " (first was n" + std::to_string(it->second.get()) + ")");
  }
  for (size_t i = 0; i < committed_.size(); ++i) {
    // Leader completeness covers entries committed in EARLIER terms only. A
    // candidate whose votes were granted before the cluster moved on can win
    // a stale term after a later term has already committed entries; it can
    // never get them replicated, so it is harmless and not a violation.
    if (committed_[i].commit_term >= term) continue;
    const Index idx{i + 1};
    if (!log.has(idx) || log.term_at(idx) != committed_[i].term ||
        hash_of(log.payload_at(idx)) != committed_[i].payload_hash) {
      violation(now, at(now, node) + "leader of term " + std::to_string(term.get()) +
                         " is missing committed entry " + std::to_string(idx.get()));
      return;
    }
  }
}

void Checker::on_commit(Time now, NodeId node, Index index, Term term, Term node_term,
                        std::span<const uint8_t> payload) {
  if (listener_) listener_->on_commit(now, node, index, term, node_term, payload);
  ++commit_reports_;
  const uint64_t h = hash_of(payload);
  const uint64_t i = index.get();
  if (i == committed_.size() + 1) {
    committed_.push_back({term, node_term, h, now});
    apply_to_model(payload);
    return;
  }
  if (i > committed_.size() + 1) {
    // A node can learn a commit index before anyone reported the indices in
    // between only if it skipped them, which the host never does.
    violation(now, at(now, node) + "commit of index " + std::to_string(i) + " before index " +
                       std::to_string(committed_.size() + 1));
    return;
  }
  Committed& c = committed_[i - 1];
  // Every reporter's term bounds the commit term from above; keep the
  // tightest bound we have seen.
  if (node_term < c.commit_term) c.commit_term = node_term;
  if (c.term != term || c.payload_hash != h) {
    violation(now, at(now, node) + "committed a different entry at index " + std::to_string(i) +
                       " (term " + std::to_string(term.get()) + ", first committed with term " +
                       std::to_string(c.term.get()) + ")");
  }
}

}  // namespace raftdst::harness

namespace raftdst::harness {

void Checker::apply_to_model(std::span<const uint8_t> payload) {
  if (!payload.empty()) {
    kv::Command c;
    if (!kv::decode_command(payload, c)) {
      violation(Time::max(),
                "committed entry " + std::to_string(committed_.size()) + " is not a valid command");
    } else if (c.op == kv::Op::kPut) {
      model_.insert_or_assign(std::string{c.key}, std::string{c.value});
    } else if (auto it = model_.find(c.key); it != model_.end()) {
      model_.erase(it);
    }
  }
  uint64_t d = 0;
  for (const auto& [k, v] : model_) d ^= kv::pair_hash(k, v);
  model_digest_.push_back(d);
}

void Checker::on_applied(Time now, NodeId node, Index index, uint64_t digest) {
  if (listener_) listener_->on_applied(now, node, index, digest);
  ++apply_reports_;
  const uint64_t i = index.get();
  if (i == 0 || i > model_digest_.size()) {
    violation(now,
              at(now, node) + "applied index " + std::to_string(i) + " before it was committed");
    return;
  }
  if (model_digest_[i - 1] != digest) {
    violation(now, at(now, node) + "state after applying index " + std::to_string(i) +
                       " differs from the reference model");
  }
}

}  // namespace raftdst::harness
