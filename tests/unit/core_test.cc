#include "raft/core.h"

#include <gtest/gtest.h>

#include <vector>

#include "common/entry_log.h"

namespace raftdst::raft {
namespace {

const Config kConfig{};
const NodeId n0{0}, n1{1}, n2{2};

std::vector<uint8_t> bytes(std::initializer_list<uint8_t> b) { return b; }

Recovered with_log(Term hs_term, std::initializer_list<uint64_t> terms) {
  Recovered r;
  r.hard_state.term = hs_term;
  for (uint64_t t : terms) r.log.append(Term{t}, bytes({static_cast<uint8_t>(t)}));
  return r;
}

// Collects and clears pending output. Messages are copied; their entry views
// stay valid until the core's log is next truncated.
struct Output {
  bool hard_state_dirty;
  HardState hs;
  Index persist_from;
  std::vector<Message> msgs;
  Index commit;
};

Output drain(Core& c) {
  const Ready& r = c.ready();
  Output o{r.hard_state_dirty,
           r.hard_state,
           r.persist_from,
           {r.messages.begin(), r.messages.end()},
           r.commit};
  c.advance();
  return o;
}

std::vector<Message> of_type(const Output& o, MsgType t) {
  std::vector<Message> out;
  for (const auto& m : o.msgs)
    if (m.type == t) out.push_back(m);
  return out;
}

Message request_vote(NodeId from, NodeId to, Term term, Index last_index, Term last_term) {
  Message m;
  m.type = MsgType::kRequestVote;
  m.from = from;
  m.to = to;
  m.term = term;
  m.last_log_index = last_index;
  m.last_log_term = last_term;
  return m;
}

Message vote(NodeId from, NodeId to, Term term, bool granted) {
  Message m;
  m.type = MsgType::kRequestVoteResp;
  m.from = from;
  m.to = to;
  m.term = term;
  m.ok = granted;
  return m;
}

Message append(NodeId from, NodeId to, Term term, Index prev, Term prev_term, Index commit,
               const LogSlice& entries = {}) {
  Message m;
  m.type = MsgType::kAppendEntries;
  m.from = from;
  m.to = to;
  m.term = term;
  m.prev_index = prev;
  m.prev_term = prev_term;
  m.commit = commit;
  if (!entries.empty()) m.entries = EntriesView::from_log(entries);
  return m;
}

Message append_ok(NodeId from, NodeId to, Term term, Index index) {
  Message m;
  m.type = MsgType::kAppendEntriesResp;
  m.from = from;
  m.to = to;
  m.term = term;
  m.ok = true;
  m.index = index;
  return m;
}

Message append_reject(NodeId from, NodeId to, Term term, Index rejected, Index hint,
                      Term hint_term) {
  Message m = append_ok(from, to, term, rejected);
  m.ok = false;
  m.hint_index = hint;
  m.hint_term = hint_term;
  return m;
}

// A leader's log to feed as AppendEntries entries.
EntryLog leader_log(std::initializer_list<uint64_t> terms) {
  EntryLog l;
  for (uint64_t t : terms) l.append(Term{t}, bytes({static_cast<uint8_t>(t)}));
  return l;
}

// Makes node 0 of a 3-node cluster leader of the next term.
Output elect(Core& c, Time& now) {
  now = c.next_deadline();
  c.tick(now);
  drain(c);
  c.step(now, vote(n1, n0, c.term(), true));
  EXPECT_EQ(c.role(), Role::kLeader);
  return drain(c);
}

// ------------------------------------------------------------------ timers

TEST(CoreElection, NothingHappensBeforeTheDeadline) {
  Core c{n0, 3, kConfig, Recovered{}, 1, Time::zero()};
  const Time deadline = c.next_deadline();
  EXPECT_GE(deadline - Time::zero(), kConfig.election_timeout_min);
  EXPECT_LT(deadline - Time::zero(), kConfig.election_timeout_max);
  c.tick(deadline - Duration::ns(1));
  EXPECT_EQ(c.role(), Role::kFollower);
  EXPECT_TRUE(drain(c).msgs.empty());
}

TEST(CoreElection, TimeoutsAreRandomizedPerSeed) {
  std::vector<Time> deadlines;
  for (uint64_t seed = 0; seed < 20; ++seed) {
    deadlines.push_back(Core{n0, 3, kConfig, Recovered{}, seed, Time::zero()}.next_deadline());
  }
  std::sort(deadlines.begin(), deadlines.end());
  EXPECT_GT(std::unique(deadlines.begin(), deadlines.end()) - deadlines.begin(), 15);
}

TEST(CoreElection, SingleNodeElectsItselfAndCommitsItsNoop) {
  Core c{n0, 1, kConfig, Recovered{}, 1, Time::zero()};
  c.tick(c.next_deadline());
  EXPECT_EQ(c.role(), Role::kLeader);
  EXPECT_EQ(c.term(), Term{1});
  const Output o = drain(c);
  EXPECT_TRUE(o.hard_state_dirty);
  EXPECT_EQ(o.hs.voted_for, n0);
  EXPECT_EQ(o.persist_from, Index{1});
  EXPECT_EQ(o.commit, Index{1});
  EXPECT_TRUE(c.log().payload_at(Index{1}).empty());
}

TEST(CoreElection, CandidateAsksEveryPeerWithItsLastLogPosition) {
  Core c{n0, 3, kConfig, with_log(Term{2}, {1, 2}), 1, Time::zero()};
  c.tick(c.next_deadline());
  EXPECT_EQ(c.role(), Role::kCandidate);
  EXPECT_EQ(c.term(), Term{3});
  const Output o = drain(c);
  EXPECT_TRUE(o.hard_state_dirty);
  EXPECT_EQ(o.hs, (HardState{Term{3}, n0}));
  auto rv = of_type(o, MsgType::kRequestVote);
  ASSERT_EQ(rv.size(), 2u);
  EXPECT_EQ(rv[0].to, n1);
  EXPECT_EQ(rv[1].to, n2);
  for (auto& m : rv) {
    EXPECT_EQ(m.last_log_index, Index{2});
    EXPECT_EQ(m.last_log_term, Term{2});
  }
}

TEST(CoreElection, CandidateRestartsElectionOnTimeout) {
  Core c{n0, 3, kConfig, Recovered{}, 1, Time::zero()};
  c.tick(c.next_deadline());
  drain(c);
  c.tick(c.next_deadline());
  EXPECT_EQ(c.role(), Role::kCandidate);
  EXPECT_EQ(c.term(), Term{2});
  EXPECT_EQ(c.stats().elections_started, 2u);
}

// ------------------------------------------------------------------- votes

TEST(CoreVote, GrantsAtMostOneCandidatePerTerm) {
  Core c{n0, 3, kConfig, Recovered{}, 1, Time::zero()};
  c.step(Time::zero(), request_vote(n1, n0, Term{1}, Index{0}, Term{0}));
  Output o = drain(c);
  EXPECT_TRUE(of_type(o, MsgType::kRequestVoteResp).at(0).ok);
  EXPECT_EQ(o.hs, (HardState{Term{1}, n1}));
  EXPECT_TRUE(o.hard_state_dirty);

  c.step(Time::zero(), request_vote(n2, n0, Term{1}, Index{0}, Term{0}));
  o = drain(c);
  EXPECT_FALSE(of_type(o, MsgType::kRequestVoteResp).at(0).ok);
  EXPECT_FALSE(o.hard_state_dirty);

  // A repeated request from the same candidate is granted again.
  c.step(Time::zero(), request_vote(n1, n0, Term{1}, Index{0}, Term{0}));
  EXPECT_TRUE(of_type(drain(c), MsgType::kRequestVoteResp).at(0).ok);
}

TEST(CoreVote, RefusesCandidatesWithAnOlderLog) {
  struct Case {
    Index last_index;
    Term last_term;
    bool granted;
  };
  // Our log: terms {1, 2}, so last position (2, 2).
  for (const Case& k : {Case{Index{5}, Term{1}, false},    // longer but older term
                        Case{Index{1}, Term{2}, false},    // same term, shorter
                        Case{Index{2}, Term{2}, true},     // identical
                        Case{Index{3}, Term{2}, true},     // same term, longer
                        Case{Index{1}, Term{3}, true}}) {  // newer term, shorter
    Core c{n0, 3, kConfig, with_log(Term{2}, {1, 2}), 1, Time::zero()};
    c.step(Time::zero(), request_vote(n1, n0, Term{3}, k.last_index, k.last_term));
    EXPECT_EQ(of_type(drain(c), MsgType::kRequestVoteResp).at(0).ok, k.granted)
        << "candidate last (" << k.last_index.get() << ", " << k.last_term.get() << ")";
    EXPECT_EQ(c.term(), Term{3});  // the term moves forward either way
  }
}

TEST(CoreVote, OnlyAGrantedVoteResetsTheElectionTimer) {
  Core c{n0, 3, kConfig, with_log(Term{1}, {1}), 1, Time::zero()};
  const Time before = c.next_deadline();
  const Time t = Time::zero() + Duration::ms(100);
  c.step(t, request_vote(n1, n0, Term{2}, Index{0}, Term{0}));  // log behind: denied
  EXPECT_EQ(c.next_deadline(), before);
  c.step(t, request_vote(n2, n0, Term{2}, Index{1}, Term{1}));  // granted
  EXPECT_GE(c.next_deadline(), t + kConfig.election_timeout_min);
}

TEST(CoreVote, HigherTermStepsLeaderDown) {
  Core c{n0, 3, kConfig, Recovered{}, 1, Time::zero()};
  Time now;
  elect(c, now);
  c.step(now, request_vote(n2, n0, Term{9}, Index{0}, Term{0}));
  EXPECT_EQ(c.role(), Role::kFollower);
  EXPECT_EQ(c.term(), Term{9});
  const Output o = drain(c);
  EXPECT_EQ(o.hs, (HardState{Term{9}, NodeId::none()}));
  EXPECT_FALSE(of_type(o, MsgType::kRequestVoteResp).at(0).ok);  // our log has the no-op
}

TEST(CoreVote, StaleVoteResponsesAreIgnored) {
  Core c{n0, 5, kConfig, Recovered{}, 1, Time::zero()};
  c.tick(c.next_deadline());  // term 1
  drain(c);
  c.tick(c.next_deadline());  // term 2
  drain(c);
  c.step(Time::zero(), vote(n1, n0, Term{1}, true));
  c.step(Time::zero(), vote(n2, n0, Term{1}, true));
  EXPECT_EQ(c.role(), Role::kCandidate);  // term-1 votes do not count in term 2
  c.step(Time::zero(), vote(n1, n0, Term{2}, true));
  c.step(Time::zero(), vote(n1, n0, Term{2}, true));  // duplicate counts once
  EXPECT_EQ(c.role(), Role::kCandidate);
  c.step(Time::zero(), vote(NodeId{3}, n0, Term{2}, true));
  EXPECT_EQ(c.role(), Role::kLeader);
}

// --------------------------------------------------------------- leadership

TEST(CoreLeader, NewLeaderAppendsNoopAndProbesEveryPeer) {
  Core c{n0, 3, kConfig, with_log(Term{1}, {1, 1}), 1, Time::zero()};
  Time now;
  const Output o = elect(c, now);
  EXPECT_EQ(c.log().last_index(), Index{3});
  EXPECT_EQ(c.log().last_term(), Term{2});
  EXPECT_EQ(o.persist_from, Index{3});
  auto ae = of_type(o, MsgType::kAppendEntries);
  ASSERT_EQ(ae.size(), 2u);
  for (auto& m : ae) {
    EXPECT_EQ(m.prev_index, Index{2});
    EXPECT_EQ(m.prev_term, Term{1});
    EXPECT_EQ(m.entries.size(), 1u);
  }
  // Probe mode: nothing more goes out until a response or a heartbeat.
  c.propose(now, bytes({7}));
  EXPECT_TRUE(of_type(drain(c), MsgType::kAppendEntries).empty());
}

TEST(CoreLeader, CandidateStepsDownForAppendFromLeaderOfSameTerm) {
  Core c{n0, 3, kConfig, Recovered{}, 1, Time::zero()};
  c.tick(c.next_deadline());
  drain(c);
  c.step(Time::zero(), append(n1, n0, Term{1}, Index{0}, Term{0}, Index{0}));
  EXPECT_EQ(c.role(), Role::kFollower);
  EXPECT_EQ(c.leader(), n1);
  EXPECT_EQ(c.voted_for(), n0);  // the vote in term 1 is kept
}

TEST(CoreLeader, AppendFromStaleTermIsRejectedWithOurTerm) {
  Core c{n0, 3, kConfig, with_log(Term{5}, {}), 1, Time::zero()};
  c.step(Time::zero(), append(n1, n0, Term{3}, Index{0}, Term{0}, Index{0}));
  const auto r = of_type(drain(c), MsgType::kAppendEntriesResp).at(0);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.term, Term{5});
  EXPECT_EQ(c.leader(), NodeId::none());
}

TEST(CoreLeader, HeartbeatsGoOutEveryInterval) {
  Core c{n0, 3, kConfig, Recovered{}, 1, Time::zero()};
  Time now;
  elect(c, now);
  EXPECT_EQ(c.next_deadline(), now + kConfig.heartbeat_interval);
  c.tick(now + kConfig.heartbeat_interval - Duration::ns(1));
  EXPECT_TRUE(drain(c).msgs.empty());
  c.tick(now + kConfig.heartbeat_interval);
  EXPECT_EQ(of_type(drain(c), MsgType::kAppendEntries).size(), 2u);
}

TEST(CoreLeader, FollowerOnlyProposalsAreRejectedWithLeaderHint) {
  Core c{n0, 3, kConfig, Recovered{}, 1, Time::zero()};
  c.step(Time::zero(), append(n2, n0, Term{1}, Index{0}, Term{0}, Index{0}));
  const ProposeResult r = c.propose(Time::zero(), bytes({1}));
  EXPECT_FALSE(r.accepted);
  EXPECT_EQ(r.leader_hint, n2);
}

// ------------------------------------------------------- append, follower

TEST(CoreAppend, RejectsWhenLogIsTooShort) {
  Core c{n0, 3, kConfig, with_log(Term{2}, {1, 2}), 1, Time::zero()};
  c.step(Time::zero(), append(n1, n0, Term{2}, Index{5}, Term{2}, Index{0}));
  const auto r = of_type(drain(c), MsgType::kAppendEntriesResp).at(0);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.index, Index{5});
  EXPECT_EQ(r.hint_index, Index{3});
  EXPECT_EQ(r.hint_term, Term{0});
}

TEST(CoreAppend, RejectsTermMismatchWithFirstIndexOfConflictingTerm) {
  Core c{n0, 3, kConfig, with_log(Term{3}, {1, 2, 2, 2}), 1, Time::zero()};
  c.step(Time::zero(), append(n1, n0, Term{3}, Index{4}, Term{3}, Index{0}));
  const auto r = of_type(drain(c), MsgType::kAppendEntriesResp).at(0);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.index, Index{4});
  EXPECT_EQ(r.hint_term, Term{2});
  EXPECT_EQ(r.hint_index, Index{2});
}

TEST(CoreAppend, AppendsNewEntriesAndReportsWhatToPersist) {
  Core c{n0, 3, kConfig, with_log(Term{1}, {1}), 1, Time::zero()};
  EntryLog l = leader_log({1, 1, 2});
  c.step(Time::zero(),
         append(n1, n0, Term{2}, Index{1}, Term{1}, Index{0}, l.slice(Index{2}, 10, SIZE_MAX)));
  const Output o = drain(c);
  EXPECT_EQ(c.log(), l);
  EXPECT_EQ(o.persist_from, Index{2});
  const auto r = of_type(o, MsgType::kAppendEntriesResp).at(0);
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.index, Index{3});
}

// The classic bug: an older AppendEntries arriving late must not remove
// entries a newer one already added.
TEST(CoreAppend, StaleShorterAppendDoesNotTruncate) {
  Core c{n0, 3, kConfig, Recovered{}, 1, Time::zero()};
  EntryLog l = leader_log({1, 1, 1});
  c.step(Time::zero(),
         append(n1, n0, Term{1}, Index{0}, Term{0}, Index{0}, l.slice(Index{1}, 3, SIZE_MAX)));
  drain(c);
  ASSERT_EQ(c.log().last_index(), Index{3});

  c.step(Time::zero(),
         append(n1, n0, Term{1}, Index{0}, Term{0}, Index{0}, l.slice(Index{1}, 1, SIZE_MAX)));
  const Output o = drain(c);
  EXPECT_EQ(c.log().last_index(), Index{3});
  EXPECT_EQ(o.persist_from, Index{});  // nothing changed on disk either
  const auto r = of_type(o, MsgType::kAppendEntriesResp).at(0);
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(r.index, Index{1});  // only what this message proves
}

TEST(CoreAppend, TruncatesFromTheFirstConflictOnly) {
  Core c{n0, 3, kConfig, with_log(Term{2}, {1, 1, 2, 2}), 1, Time::zero()};
  EntryLog l = leader_log({1, 1, 3});
  c.step(Time::zero(),
         append(n1, n0, Term{3}, Index{1}, Term{1}, Index{0}, l.slice(Index{2}, 2, SIZE_MAX)));
  const Output o = drain(c);
  EXPECT_EQ(c.log(), l);
  EXPECT_EQ(o.persist_from, Index{3});
}

TEST(CoreAppend, CommitIsBoundedByTheLastEntryThisMessageVerified) {
  Core c{n0, 3, kConfig, with_log(Term{1}, {1, 1, 1, 1}), 1, Time::zero()};
  // Leader says commit 10, but the message only proves indices up to 2.
  c.step(Time::zero(), append(n1, n0, Term{1}, Index{2}, Term{1}, Index{10}));
  EXPECT_EQ(c.commit_index(), Index{2});
  // A later, shorter message never moves commit backwards.
  c.step(Time::zero(), append(n1, n0, Term{1}, Index{1}, Term{1}, Index{10}));
  EXPECT_EQ(c.commit_index(), Index{2});
}

TEST(CoreAppend, RefusesToTruncateCommittedEntries) {
  Core c{n0, 3, kConfig, with_log(Term{2}, {1, 1}), 1, Time::zero()};
  c.step(Time::zero(), append(n1, n0, Term{2}, Index{2}, Term{1}, Index{2}));
  ASSERT_EQ(c.commit_index(), Index{2});
  drain(c);
  EntryLog bad = leader_log({1, 2});  // conflicts at committed index 2
  c.step(Time::zero(),
         append(n2, n0, Term{2}, Index{1}, Term{1}, Index{0}, bad.slice(Index{2}, 1, SIZE_MAX)));
  EXPECT_EQ(c.stats().refused_committed_truncation, 1u);
  EXPECT_EQ(c.log().term_at(Index{2}), Term{1});
}

// --------------------------------------------------------- append, leader

// Paper Figure 8: an entry from an earlier term that is on a majority is
// not committed by counting replicas. It commits once an entry of the
// current term reaches a majority.
TEST(CoreCommit, LeaderDoesNotCommitEarlierTermEntriesByCounting) {
  Core c{n0, 3, kConfig, with_log(Term{2}, {1, 2}), 1, Time::zero()};
  Time now;
  elect(c, now);  // term 3, no-op at index 3
  ASSERT_EQ(c.term(), Term{3});

  c.step(now, append_ok(n1, n0, Term{3}, Index{2}));  // n1 has index 2 (term 2)
  EXPECT_EQ(c.progress(n1).match, Index{2});
  EXPECT_EQ(c.commit_index(), Index{0});  // index 2 is on a majority but from term 2

  c.step(now, append_ok(n1, n0, Term{3}, Index{3}));
  EXPECT_EQ(c.commit_index(), Index{3});  // current-term entry commits both
}

TEST(CoreCommit, NeedsAMajorityNotJustOnePeer) {
  Core c{n0, 5, kConfig, Recovered{}, 1, Time::zero()};
  Time now = c.next_deadline();
  c.tick(now);
  drain(c);
  c.step(now, vote(n1, n0, Term{1}, true));
  c.step(now, vote(n2, n0, Term{1}, true));
  ASSERT_EQ(c.role(), Role::kLeader);
  drain(c);
  c.step(now, append_ok(n1, n0, Term{1}, Index{1}));
  EXPECT_EQ(c.commit_index(), Index{0});  // 2 of 5
  c.step(now, append_ok(n2, n0, Term{1}, Index{1}));
  EXPECT_EQ(c.commit_index(), Index{1});  // 3 of 5
}

TEST(CoreCommit, SuccessSwitchesPeerToPipelinedReplication) {
  Core c{n0, 3, kConfig, Recovered{}, 1, Time::zero()};
  Time now;
  elect(c, now);
  c.step(now, append_ok(n1, n0, Term{1}, Index{1}));
  drain(c);
  EXPECT_EQ(c.progress(n1).mode, Progress::Mode::kReplicate);

  c.propose(now, bytes({1}));
  c.propose(now, bytes({2}));
  const Output o = drain(c);
  auto ae = of_type(o, MsgType::kAppendEntries);
  ASSERT_EQ(ae.size(), 1u);  // n1 only: n2 is still probing
  EXPECT_EQ(ae[0].to, n1);
  EXPECT_EQ(ae[0].entries.size(), 2u);  // both proposals in one message
  EXPECT_EQ(c.progress(n1).next, Index{4});
}

TEST(CoreCommit, RejectionHintJumpsPastTheWholeConflictingTerm) {
  // Leader log: 1 1 2 2 2 4 4, then elect adds the term-5 no-op at 8.
  Core c{n0, 3, kConfig, with_log(Term{4}, {1, 1, 2, 2, 2, 4, 4}), 1, Time::zero()};
  Time now;
  elect(c, now);
  ASSERT_EQ(c.term(), Term{5});
  // Follower has term 3 from index 3 on: we have no term 3, so use its hint.
  c.step(now, append_reject(n1, n0, Term{5}, Index{7}, Index{3}, Term{3}));
  EXPECT_EQ(c.progress(n1).next, Index{3});
  EXPECT_EQ(c.progress(n1).mode, Progress::Mode::kProbe);
  auto ae = of_type(drain(c), MsgType::kAppendEntries);
  ASSERT_EQ(ae.size(), 1u);
  EXPECT_EQ(ae[0].prev_index, Index{2});

  // Follower's conflicting term is one we have: resume after our last entry of it.
  Core d{n0, 3, kConfig, with_log(Term{4}, {1, 1, 2, 2, 2, 4, 4}), 1, Time::zero()};
  elect(d, now);
  d.step(now, append_reject(n1, n0, Term{5}, Index{7}, Index{3}, Term{2}));
  EXPECT_EQ(d.progress(n1).next, Index{6});
}

TEST(CoreCommit, StaleRejectionBelowMatchIsIgnored) {
  Core c{n0, 3, kConfig, Recovered{}, 1, Time::zero()};
  Time now;
  elect(c, now);
  c.step(now, append_ok(n1, n0, Term{1}, Index{1}));
  const Index next = c.progress(n1).next;
  c.step(now, append_reject(n1, n0, Term{1}, Index{1}, Index{1}, Term{0}));
  EXPECT_EQ(c.progress(n1).next, next);
  EXPECT_EQ(c.stats().stale_responses, 1u);
}

// "Lost" means a whole heartbeat interval passed with no progress from the
// peer. An ack inside the current interval counts as progress.
TEST(CoreCommit, LostPipelinedAppendsFallBackToProbingAfterAQuietInterval) {
  Core c{n0, 3, kConfig, Recovered{}, 1, Time::zero()};
  Time now;
  elect(c, now);
  c.step(now, append_ok(n1, n0, Term{1}, Index{1}));  // progress in interval 1
  drain(c);
  c.propose(now, bytes({1}));
  drain(c);  // sent to n1 and lost
  ASSERT_EQ(c.progress(n1).next, Index{3});

  now = c.next_deadline();
  c.tick(now);  // interval 1 had progress: keep pipelining
  drain(c);
  EXPECT_EQ(c.progress(n1).mode, Progress::Mode::kReplicate);

  now = c.next_deadline();
  c.tick(now);  // interval 2 had none: retry from match + 1
  auto ae = of_type(drain(c), MsgType::kAppendEntries);
  EXPECT_EQ(c.progress(n1).mode, Progress::Mode::kProbe);
  bool resent = false;
  for (auto& m : ae) resent |= m.to == n1 && m.prev_index == Index{1} && m.entries.size() == 1;
  EXPECT_TRUE(resent);
}

class CoreDeathTest : public ::testing::Test {
 protected:
  void SetUp() override { GTEST_FLAG_SET(death_test_style, "threadsafe"); }
};

TEST_F(CoreDeathTest, RecoveredLogAboveHardStateTermIsRejected) {
  EXPECT_DEATH(Core(n0, 3, kConfig, with_log(Term{1}, {1, 2}), 1, Time::zero()),
               "above the persisted term");
}

}  // namespace
}  // namespace raftdst::raft
