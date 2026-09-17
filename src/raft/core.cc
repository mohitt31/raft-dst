#include "raft/core.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace raftdst::raft {

#ifdef RAFTDST_MUTANTS
const char* mutation_name(Mutation m) {
  switch (m) {
    case Mutation::kNone:
      return "none";
    case Mutation::kCommitOldTerm:
      return "commit-old-term";
    case Mutation::kVoteWithoutLogCheck:
      return "vote-without-log-check";
    case Mutation::kDoubleVote:
      return "vote-twice";
    case Mutation::kTruncateOnStaleAppend:
      return "truncate-on-stale-append";
    case Mutation::kFollowerCommitUnbounded:
      return "follower-commit-unbounded";
    case Mutation::kNoVotePersist:
      return "no-vote-persist";
  }
  return "?";
}
#endif

Core::Core(NodeId self, uint8_t cluster_size, const Config& config, Recovered state, uint64_t seed,
           Time now)
    : self_(self),
      n_(cluster_size),
      config_(config),
      rng_(seed),
      term_(state.hard_state.term),
      voted_for_(state.hard_state.voted_for),
      log_(std::move(state.log)) {
  RAFT_ASSERT(n_ >= 1 && n_ <= kMaxNodes, "cluster size out of range");
  RAFT_ASSERT(self_.get() < n_, "self id outside the cluster");
  RAFT_ASSERT(config_.election_timeout_min < config_.election_timeout_max,
              "election timeout range is empty");
  RAFT_ASSERT(config_.heartbeat_interval < config_.election_timeout_min,
              "heartbeat interval must be shorter than the election timeout");
  RAFT_ASSERT(config_.max_entries_per_append >= 1, "max_entries_per_append must be >= 1");
  RAFT_ASSERT(log_.last_term() <= term_, "recovered log has an entry above the persisted term");
  outbox_.reserve(2 * kMaxNodes);
  reset_election_deadline(now);
#ifdef RAFTDST_DCHECKS
  check_invariants();
#endif
}

// ------------------------------------------------------------------ inputs

void Core::tick(Time now) {
  if (role_ == Role::kLeader) {
    if (now >= heartbeat_deadline_) {
      heartbeat_deadline_ = now + config_.heartbeat_interval;
      send_heartbeats();
    }
  } else if (now >= election_deadline_) {
    start_election(now);
  }
#ifdef RAFTDST_DCHECKS
  check_invariants();
#endif
}

void Core::step(Time now, const Message& m) {
  RAFT_DCHECK(m.to == self_ && m.from != self_ && m.from.get() < n_, "misrouted message");

  // Any message from a later term moves us to that term as a follower. Only
  // an AppendEntries tells us who leads it.
  if (m.term > term_) {
    become_follower(now, m.term, m.type == MsgType::kAppendEntries ? m.from : NodeId::none());
  }

  switch (m.type) {
    case MsgType::kRequestVote:
      on_request_vote(now, m);
      break;
    case MsgType::kRequestVoteResp:
      on_vote_response(now, m);
      break;
    case MsgType::kAppendEntries:
      on_append_entries(now, m);
      break;
    case MsgType::kAppendEntriesResp:
      on_append_response(m);
      break;
  }
#ifdef RAFTDST_DCHECKS
  check_invariants();
#endif
}

ProposeResult Core::propose(Time /*now*/, std::span<const uint8_t> command) {
  if (role_ != Role::kLeader) return ProposeResult{false, Index{}, Term{}, leader_};
  log_.append(term_, command);
  mark_unstable(log_.last_index());
  maybe_commit();  // a single-node cluster commits immediately
#ifdef RAFTDST_DCHECKS
  check_invariants();
#endif
  return ProposeResult{true, log_.last_index(), term_, self_};
}

const Ready& Core::ready() {
  flush_appends();
  ready_.hard_state_dirty = hard_state_dirty_;
  ready_.hard_state = HardState{term_, voted_for_};
  ready_.persist_from = unstable_from_;
  ready_.messages = outbox_;
  ready_.commit = commit_;
  return ready_;
}

void Core::advance() {
  outbox_.clear();
  hard_state_dirty_ = false;
  unstable_from_ = Index{};
}

// ------------------------------------------------------------ transitions

void Core::reset_election_deadline(Time now) {
  const int64_t lo = config_.election_timeout_min.count_ns();
  const int64_t hi = config_.election_timeout_max.count_ns() - 1;
  election_deadline_ = now + Duration::ns(rng_.between(lo, hi));
}

void Core::become_follower(Time now, Term term, NodeId leader) {
  RAFT_ASSERT(term >= term_, "stepping down to an older term");
  const bool was_follower = role_ == Role::kFollower;
  if (term > term_) {
    term_ = term;
    voted_for_ = NodeId::none();
    hard_state_dirty_ = true;
  }
  role_ = Role::kFollower;
  leader_ = leader;
  votes_ = 0;
  // A follower keeps its timer: only a current leader or a granted vote
  // resets it. A leader or candidate stepping down has no running election
  // timer, so it starts one.
  if (!was_follower) reset_election_deadline(now);
}

void Core::start_election(Time now) {
  term_ = term_.next();
  voted_for_ = self_;
  hard_state_dirty_ = true;
  role_ = Role::kCandidate;
  leader_ = NodeId::none();
  votes_ = bit(self_);
  ++stats_.elections_started;
  reset_election_deadline(now);

  if (std::popcount(votes_) >= quorum()) {
    become_leader(now);
    return;
  }
  for (uint8_t i = 0; i < n_; ++i) {
    if (i == self_.get()) continue;
    Message& m = push(MsgType::kRequestVote, NodeId{i});
    m.last_log_index = log_.last_index();
    m.last_log_term = log_.last_term();
  }
}

void Core::become_leader(Time now) {
  role_ = Role::kLeader;
  leader_ = self_;
  votes_ = 0;
  for (uint8_t i = 0; i < n_; ++i) {
    progress_[i] =
        Progress{log_.last_index().next(), Index{}, Index{}, Progress::Mode::kProbe, false};
  }
  // No-op entry in the new term: lets entries from earlier terms commit
  // without waiting for a client write (thesis section 6.4).
  log_.append(term_, {});
  mark_unstable(log_.last_index());
  heartbeat_deadline_ = now + config_.heartbeat_interval;
  maybe_commit();
  for (uint8_t i = 0; i < n_; ++i) {
    if (i != self_.get()) send_append(NodeId{i});
  }
}

// --------------------------------------------------------------- handlers

void Core::on_request_vote(Time now, const Message& m) {
  const bool log_ok = m.last_log_term > log_.last_term() || (m.last_log_term == log_.last_term() &&
                                                             m.last_log_index >= log_.last_index());
  const bool log_ok_checked = log_ok || mutation_on(Mutation::kVoteWithoutLogCheck);
  const bool vote_free =
      voted_for_ == NodeId::none() || voted_for_ == m.from || mutation_on(Mutation::kDoubleVote);
  const bool grant = m.term == term_ && log_ok_checked && vote_free;
  if (grant) {
    if (voted_for_ != m.from) {
      voted_for_ = m.from;
      hard_state_dirty_ = true;
    }
    reset_election_deadline(now);
  }
  Message& r = push(MsgType::kRequestVoteResp, m.from);
  r.ok = grant;
}

void Core::on_vote_response(Time now, const Message& m) {
  if (role_ != Role::kCandidate || m.term != term_ || !m.ok) return;
  votes_ |= bit(m.from);
  if (std::popcount(votes_) >= quorum()) become_leader(now);
}

void Core::on_append_entries(Time now, const Message& m) {
  if (m.term < term_) {
    // Tell a stale leader about the newer term. The index fields only need
    // to be well-formed; the sender steps down before reading them.
    reply_append(m, false, Index{std::max<uint64_t>(m.prev_index.get(), 1)},
                 log_.last_index().next(), Term{});
    return;
  }
  // m.term == term_ from here on.
  if (role_ == Role::kLeader) {
    // Two leaders in one term. Our own state is fine; the checker reports it.
    ++stats_.append_from_same_term_leader;
    return;
  }
  if (role_ == Role::kCandidate) become_follower(now, term_, m.from);
  leader_ = m.from;
  reset_election_deadline(now);

  if (m.prev_index > log_.last_index()) {
    reply_append(m, false, m.prev_index, log_.last_index().next(), Term{});
    return;
  }
  if (log_.term_at(m.prev_index) != m.prev_term) {
    const Term conflict = log_.term_at(m.prev_index);
    Index first = m.prev_index;
    while (first.get() > 1 && log_.term_at(first.prev()) == conflict) first = first.prev();
    reply_append(m, false, m.prev_index, first, conflict);
    return;
  }

  // prev matches. Skip entries we already have with the same term; truncate
  // only at the first real conflict, so a stale or duplicated append can
  // never remove entries a newer append already added.
  const uint64_t n = m.entries.size();
  if (mutation_on(Mutation::kTruncateOnStaleAppend) && n > 0 && m.prev_index < log_.last_index() &&
      m.prev_index >= commit_) {
    log_.truncate_after(m.prev_index);
  }
  uint64_t i = 0;
  for (; i < n; ++i) {
    const Index idx = m.prev_index + (i + 1);
    if (!log_.has(idx) || log_.term_at(idx) != m.entries.term(i)) break;
  }
  if (i < n) {
    const Index idx = m.prev_index + (i + 1);
    if (log_.has(idx)) {
      if (idx <= commit_) {
        ++stats_.refused_committed_truncation;
        return;
      }
      log_.truncate_after(idx.prev());
    }
    m.entries.for_each_from(i, [&](Term t, std::span<const uint8_t> p) { log_.append(t, p); });
    mark_unstable(idx);
  }

  const Index last_new = m.prev_index + n;
  const Index verified =
      mutation_on(Mutation::kFollowerCommitUnbounded) ? log_.last_index() : last_new;
  commit_ = std::max(commit_, std::min(m.commit, verified));
  reply_append(m, true, last_new, Index{}, Term{});
}

void Core::on_append_response(const Message& m) {
  if (role_ != Role::kLeader || m.term != term_) return;
  Progress& p = progress_[m.from.get()];
  p.paused = false;

  if (m.ok) {
    if (m.index > log_.last_index()) {
      ++stats_.response_beyond_log;
      return;
    }
    p.mode = Progress::Mode::kReplicate;
    if (m.index > p.match) p.match = m.index;
    if (p.next <= m.index) p.next = m.index.next();
    maybe_commit();
    if (p.next <= log_.last_index()) send_append(m.from);
    return;
  }

  if (m.index > log_.last_index()) {
    ++stats_.response_beyond_log;  // we never sent an append past our own log
    return;
  }
  if (m.index <= p.match) {
    ++stats_.stale_responses;
    return;
  }
  // Jump back using the follower's hint. With a conflicting term, resume
  // after our last entry of that term if we have one.
  Index next = m.hint_index;
  if (m.hint_term != Term{}) {
    Index j = std::min(m.index, log_.last_index());
    while (j.get() > 0 && log_.term_at(j) > m.hint_term) j = j.prev();
    if (j.get() > 0 && log_.term_at(j) == m.hint_term) next = j.next();
  }
  // Must move back past the rejected position, and never below match + 1.
  next = std::clamp(next, p.match.next(), m.index);
  p.next = next;
  p.mode = Progress::Mode::kProbe;
  send_append(m.from);
}

// ------------------------------------------------------------ replication

void Core::send_append(NodeId peer) {
  Progress& p = progress_[peer.get()];
  if (p.mode == Progress::Mode::kProbe && p.paused) return;

  const LogSlice s =
      log_.slice(p.next, config_.max_entries_per_append, config_.max_bytes_per_append);
  Message& m = push(MsgType::kAppendEntries, peer);
  m.prev_index = p.next.prev();
  m.prev_term = log_.term_at(m.prev_index);
  m.commit = commit_;
  m.entries = EntriesView::from_log(s);

  if (p.mode == Progress::Mode::kReplicate) {
    p.next = p.next + s.size();  // optimistic: assume it arrives
  } else {
    p.paused = true;
  }
}

void Core::send_heartbeats() {
  for (uint8_t i = 0; i < n_; ++i) {
    if (i == self_.get()) continue;
    Progress& p = progress_[i];
    // Optimistic sends that made no progress for a whole interval were
    // probably lost: fall back to probing from what the peer confirmed.
    if (p.mode == Progress::Mode::kReplicate && p.next > p.match.next() &&
        p.match == p.match_at_heartbeat) {
      p.next = p.match.next();
      p.mode = Progress::Mode::kProbe;
    }
    p.match_at_heartbeat = p.match;
    p.paused = false;
    send_append(NodeId{i});
  }
}

// New entries go to caught-up peers when the host collects output, so many
// proposals in one step share one AppendEntries per peer.
void Core::flush_appends() {
  if (role_ != Role::kLeader) return;
  for (uint8_t i = 0; i < n_; ++i) {
    if (i == self_.get()) continue;
    const Progress& p = progress_[i];
    if (p.mode == Progress::Mode::kReplicate && p.next <= log_.last_index()) {
      send_append(NodeId{i});
    }
  }
}

void Core::maybe_commit() {
  if (role_ != Role::kLeader) return;
  std::array<uint64_t, kMaxNodes> match{};
  for (uint8_t i = 0; i < n_; ++i) {
    match[i] = i == self_.get() ? log_.last_index().get() : progress_[i].match.get();
  }
  // Insertion sort, descending. At most five elements.
  for (uint8_t i = 1; i < n_; ++i) {
    for (uint8_t j = i; j > 0 && match[j - 1] < match[j]; --j) std::swap(match[j - 1], match[j]);
  }
  const Index candidate{match[quorum() - 1]};
  // Only entries from the current term are committed by counting replicas
  // (paper Figure 8); earlier ones commit indirectly.
  if (candidate > commit_ &&
      (log_.term_at(candidate) == term_ || mutation_on(Mutation::kCommitOldTerm))) {
    commit_ = candidate;
  }
}

void Core::mark_unstable(Index from) {
  if (unstable_from_ == Index{} || from < unstable_from_) unstable_from_ = from;
}

void Core::reply_append(const Message& to, bool ok, Index index, Index hint_index, Term hint_term) {
  Message& r = push(MsgType::kAppendEntriesResp, to.from);
  r.ok = ok;
  r.index = index;
  r.hint_index = hint_index;
  r.hint_term = hint_term;
}

Message& Core::push(MsgType type, NodeId to) {
  Message& m = outbox_.emplace_back();
  m.type = type;
  m.from = self_;
  m.to = to;
  m.term = term_;
  return m;
}

// ------------------------------------------------------------- invariants

void Core::check_invariants() const {
  RAFT_ASSERT(log_.last_term() <= term_, "log has an entry above the current term");
  RAFT_ASSERT(commit_ <= log_.last_index(), "commit index beyond the log");
  RAFT_ASSERT((leader_ == self_) == (role_ == Role::kLeader), "leader id disagrees with role");
  switch (role_) {
    case Role::kFollower:
      RAFT_ASSERT(votes_ == 0, "follower holding votes");
      break;
    case Role::kCandidate:
      RAFT_ASSERT(voted_for_ == self_, "candidate did not vote for itself");
      RAFT_ASSERT((votes_ & bit(self_)) != 0, "candidate vote missing from tally");
      break;
    case Role::kLeader:
      RAFT_ASSERT(voted_for_ == self_, "leader did not vote for itself in its term");
      RAFT_ASSERT(log_.last_term() == term_, "leader log does not end in its own term");
      for (uint8_t i = 0; i < n_; ++i) {
        if (i == self_.get()) continue;
        const Progress& p = progress_[i];
        RAFT_ASSERT(p.match <= log_.last_index(), "peer match beyond the log");
        RAFT_ASSERT(p.match < p.next, "peer next not after match");
        RAFT_ASSERT(p.next <= log_.last_index().next(), "peer next beyond log end + 1");
      }
      break;
  }
}

}  // namespace raftdst::raft
