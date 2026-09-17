#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "common/entry_log.h"
#include "common/rng.h"
#include "common/types.h"
#include "env/storage.h"
#include "raft/message.h"

namespace raftdst::raft {

enum class Role : uint8_t { kFollower, kCandidate, kLeader };

struct Config {
  Duration election_timeout_min = Duration::ms(150);
  Duration election_timeout_max = Duration::ms(300);  // exclusive
  Duration heartbeat_interval = Duration::ms(50);
  uint64_t max_entries_per_append = 256;
  size_t max_bytes_per_append = 1u << 20;
};

struct ProposeResult {
  bool accepted = false;
  Index index;         // where the command was appended (accepted only)
  Term term;           // the term it was appended in (accepted only)
  NodeId leader_hint;  // who this node thinks leads (rejected only; may be none)
};

// Output of one or more steps. The host must, in order:
//   1. persist hard_state (if dirty) and the log from persist_from (if set)
//   2. sync
//   3. send every message
//   4. apply entries up to commit
//   5. call Core::advance()
// Messages hold views into the log; they are valid until advance() or the
// next call that mutates the Core.
struct Ready {
  bool hard_state_dirty = false;
  HardState hard_state;
  Index persist_from;  // Index{0} when the log did not change
  std::span<const Message> messages;
  Index commit;
};

struct Progress {
  enum class Mode : uint8_t { kProbe, kReplicate };
  Index next{1};
  Index match;
  Index match_at_heartbeat;
  Mode mode = Mode::kProbe;
  bool paused = false;  // probe mode: an append is outstanding
};

// Inputs the Core refused. A correct cluster never produces any of these;
// the simulator's checker asserts they stay zero.
struct CoreStats {
  uint64_t elections_started = 0;
  uint64_t append_from_same_term_leader = 0;
  uint64_t refused_committed_truncation = 0;
  uint64_t response_beyond_log = 0;
  uint64_t stale_responses = 0;
};

// Raft consensus as a deterministic state machine with no I/O. See DESIGN.md
// sections 1, 2 and 7.
class Core {
 public:
  Core(NodeId self, uint8_t cluster_size, const Config& config, Recovered state, uint64_t seed,
       Time now);

  void step(Time now, const Message& m);
  void tick(Time now);
  ProposeResult propose(Time now, std::span<const uint8_t> command);

  const Ready& ready();
  void advance();

  [[nodiscard]] Time next_deadline() const {
    return role_ == Role::kLeader ? heartbeat_deadline_ : election_deadline_;
  }

  [[nodiscard]] NodeId self() const { return self_; }
  [[nodiscard]] uint8_t cluster_size() const { return n_; }
  [[nodiscard]] Role role() const { return role_; }
  [[nodiscard]] Term term() const { return term_; }
  [[nodiscard]] NodeId voted_for() const { return voted_for_; }
  [[nodiscard]] NodeId leader() const { return leader_; }
  [[nodiscard]] Index commit_index() const { return commit_; }
  [[nodiscard]] const EntryLog& log() const { return log_; }
  [[nodiscard]] const CoreStats& stats() const { return stats_; }
  [[nodiscard]] const Progress& progress(NodeId peer) const { return progress_[peer.get()]; }

  // Asserts every internal invariant. Runs after each input in DCHECK builds.
  void check_invariants() const;

 private:
  [[nodiscard]] uint8_t quorum() const { return static_cast<uint8_t>(n_ / 2 + 1); }
  [[nodiscard]] uint32_t bit(NodeId id) const { return uint32_t{1} << id.get(); }

  void reset_election_deadline(Time now);
  void become_follower(Time now, Term term, NodeId leader);
  void start_election(Time now);
  void become_leader(Time now);

  void on_request_vote(Time now, const Message& m);
  void on_vote_response(Time now, const Message& m);
  void on_append_entries(Time now, const Message& m);
  void on_append_response(const Message& m);

  void send_append(NodeId peer);
  void send_heartbeats();
  void flush_appends();
  void maybe_commit();
  void mark_unstable(Index from);
  void reply_append(const Message& to, bool ok, Index index, Index hint_index, Term hint_term);
  Message& push(MsgType type, NodeId to);

  NodeId self_;
  uint8_t n_;
  Config config_;
  Rng rng_;

  // Persistent.
  Term term_;
  NodeId voted_for_;
  EntryLog log_;

  // Volatile.
  Role role_ = Role::kFollower;
  NodeId leader_;
  Index commit_;
  Time election_deadline_;
  Time heartbeat_deadline_;
  uint32_t votes_ = 0;
  std::array<Progress, kMaxNodes> progress_{};

  // Pending output.
  std::vector<Message> outbox_;
  bool hard_state_dirty_ = false;
  Index unstable_from_;
  Ready ready_;

  CoreStats stats_;
};

}  // namespace raftdst::raft
