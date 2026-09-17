// Measures whether the checker and fuzzer actually catch known Raft bugs.
// Only built in the mutants preset.
#include <gtest/gtest.h>

#include <string>

#include "harness/fuzz_config.h"
#include "harness/scenario.h"
#include "raft/core.h"

namespace raftdst::harness {
namespace {

using raft::Mutation;

struct Detection {
  bool found = false;
  uint64_t seeds = 0;
  std::string violation;
};

// Runs fuzz seeds with `m` planted until a violation appears.
Detection fuzz_until_detected(Mutation m, uint64_t max_seeds) {
  for (uint64_t seed = 1; seed <= max_seeds; ++seed) {
    ScenarioConfig cfg = FuzzCase::from_seed(seed).to_scenario();
    cfg.raft.mutation = m;
    const ScenarioResult r = run_scenario(cfg);
    if (!r.ok()) return {true, seed, r.violations.front()};
  }
  return {false, max_seeds, {}};
}

// The paper's Figure 8 sequence, driven by hand: an entry from an earlier
// term sits on a majority, and a leader of a later term must not commit it
// by counting replicas.
bool figure8_scenario_detects(Mutation m) {
  raft::Config cfg;
  cfg.mutation = m;
  Recovered state;
  state.hard_state.term = Term{2};
  state.log.append(Term{1}, std::vector<uint8_t>{1});
  state.log.append(Term{2}, std::vector<uint8_t>{2});
  raft::Core c{NodeId{0}, 3, cfg, std::move(state), 1, Time::zero()};

  const Time now = c.next_deadline();
  c.tick(now);  // term 3 candidate
  c.advance();
  raft::Message vote;
  vote.type = raft::MsgType::kRequestVoteResp;
  vote.from = NodeId{1};
  vote.to = NodeId{0};
  vote.term = c.term();
  vote.ok = true;
  c.step(now, vote);
  if (c.role() != raft::Role::kLeader) return false;
  c.advance();

  raft::Message ack;
  ack.type = raft::MsgType::kAppendEntriesResp;
  ack.from = NodeId{1};
  ack.to = NodeId{0};
  ack.term = c.term();
  ack.ok = true;
  ack.index = Index{2};  // follower has the term-2 entry, not the new no-op
  c.step(now, ack);
  // Correct: nothing commits yet. Buggy: index 2 commits by counting.
  return c.commit_index() != Index{0};
}

// A follower holding uncommitted entries from an older leader receives an
// AppendEntries that matches further back and carries fewer entries than the
// leader's commit index. It may only commit what this message verified; the
// entries beyond that are its own stale tail, which the leader does not have.
bool stale_tail_scenario_detects(Mutation m) {
  raft::Config cfg;
  cfg.mutation = m;
  Recovered state;
  state.hard_state.term = Term{3};
  state.log.append(Term{1}, std::vector<uint8_t>{1});
  state.log.append(Term{1}, std::vector<uint8_t>{2});
  state.log.append(Term{2}, std::vector<uint8_t>{9});  // stale, never committed
  raft::Core c{NodeId{0}, 3, cfg, std::move(state), 1, Time::zero()};

  raft::Message ae;
  ae.type = raft::MsgType::kAppendEntries;
  ae.from = NodeId{1};
  ae.to = NodeId{0};
  ae.term = Term{3};
  ae.prev_index = Index{1};
  ae.prev_term = Term{1};
  ae.commit = Index{3};  // the leader has committed its own index 3
  c.step(Time::zero(), ae);
  // Correct: only index 1 is verified by this message. Buggy: commits our
  // stale index 3, which is a different entry from the leader's.
  return c.commit_index() != Index{1};
}

TEST(Mutants, EveryPlantedBugIsCaught) {
  struct Case {
    Mutation m;
    uint64_t max_seeds;
  };
  const Case cases[] = {
      {Mutation::kVoteWithoutLogCheck, 60},   {Mutation::kDoubleVote, 60},
      {Mutation::kTruncateOnStaleAppend, 60}, {Mutation::kFollowerCommitUnbounded, 60},
      {Mutation::kNoVotePersist, 60},         {Mutation::kCommitOldTerm, 60},
  };

  for (const Case& k : cases) {
    const Detection d = fuzz_until_detected(k.m, k.max_seeds);
    if (d.found) {
      std::printf("%-26s caught by fuzzing after %llu seeds: %s\n", raft::mutation_name(k.m),
                  static_cast<unsigned long long>(d.seeds), d.violation.c_str());
      continue;
    }
    // Random search missed it; a targeted scenario must catch it instead.
    const bool targeted = figure8_scenario_detects(k.m) || stale_tail_scenario_detects(k.m);
    std::printf("%-26s NOT caught by %llu fuzz seeds; targeted scenario catches it: %s\n",
                raft::mutation_name(k.m), static_cast<unsigned long long>(d.seeds),
                targeted ? "yes" : "NO");
    EXPECT_TRUE(targeted) << raft::mutation_name(k.m)
                          << " escapes both the fuzzer and the "
                             "targeted scenario";
  }
}

// The clean build must pass the same seeds: no false positives.
TEST(Mutants, NoViolationsWithoutAMutation) {
  const Detection d = fuzz_until_detected(Mutation::kNone, 60);
  EXPECT_FALSE(d.found) << d.violation;
}

}  // namespace
}  // namespace raftdst::harness
