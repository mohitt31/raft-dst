#include "harness/checker.h"

#include <gtest/gtest.h>

#include <vector>

namespace raftdst::harness {
namespace {

const Time t0 = Time::zero();
const NodeId n0{0}, n1{1}, n2{2};

std::vector<uint8_t> p(uint8_t b) { return {b}; }

EntryLog log_of(std::initializer_list<std::pair<uint64_t, uint8_t>> entries) {
  EntryLog l;
  for (auto [t, b] : entries) l.append(Term{t}, p(b));
  return l;
}

TEST(Checker, NormalHistoryIsClean) {
  Checker c;
  c.on_recovered(t0, n0, {}, EntryLog{});
  c.on_hard_state_synced(t0, n0, {Term{1}, n0});
  c.on_became_leader(t0, n0, Term{1}, EntryLog{});
  c.on_commit(t0, n0, Index{1}, Term{1}, p(7));
  c.on_commit(t0, n1, Index{1}, Term{1}, p(7));  // same entry on another node
  c.on_commit(t0, n1, Index{1}, Term{1}, p(7));  // re-applied after a restart
  c.on_hard_state_synced(t0, n0, {Term{2}, NodeId::none()});
  c.on_hard_state_synced(t0, n0, {Term{2}, n1});
  c.on_became_leader(t0, n1, Term{2}, log_of({{1, 7}}));
  EXPECT_TRUE(c.ok()) << c.violations().front();
}

// Regression: a restart into a newer term with no vote must clear the vote
// recorded for the older term, or a legitimate vote later in the new term
// looks like a changed vote.
TEST(Checker, RestartIntoNewTermWithoutVoteThenVotingIsClean) {
  Checker c;
  c.on_hard_state_synced(t0, n0, {Term{4}, n1});
  c.on_recovered(t0, n0, {Term{5}, NodeId::none()}, EntryLog{});
  c.on_hard_state_synced(t0, n0, {Term{5}, n2});
  EXPECT_TRUE(c.ok()) << c.violations().front();
}

TEST(Checker, SecondLeaderInOneTerm) {
  Checker c;
  c.on_became_leader(t0, n0, Term{3}, EntryLog{});
  c.on_became_leader(t0, n0, Term{3}, EntryLog{});  // same node again is fine
  EXPECT_TRUE(c.ok());
  c.on_became_leader(t0, n1, Term{3}, EntryLog{});
  ASSERT_FALSE(c.ok());
  EXPECT_NE(c.violations()[0].find("second leader in term 3"), std::string::npos);
}

TEST(Checker, VoteChangedWithinATermAcrossRestart) {
  Checker c;
  c.on_hard_state_synced(t0, n0, {Term{2}, n1});
  c.on_recovered(t0, n0, {Term{2}, NodeId::none()}, EntryLog{});  // vote lost on disk
  ASSERT_FALSE(c.ok());
  EXPECT_NE(c.violations()[0].find("vote in term 2 changed from n1 to none"), std::string::npos);
}

TEST(Checker, DurableTermWentBackwards) {
  Checker c;
  c.on_hard_state_synced(t0, n2, {Term{7}, NodeId::none()});
  c.on_recovered(t0, n2, {Term{6}, NodeId::none()}, EntryLog{});
  ASSERT_FALSE(c.ok());
  EXPECT_NE(c.violations()[0].find("went back from 7 to 6"), std::string::npos);
}

TEST(Checker, DifferentEntriesCommittedAtOneIndex) {
  Checker c;
  c.on_commit(t0, n0, Index{1}, Term{1}, p(1));
  c.on_commit(t0, n1, Index{1}, Term{1}, p(2));  // same term, different payload
  c.on_commit(t0, n2, Index{1}, Term{2}, p(1));  // different term
  EXPECT_EQ(c.violations().size(), 2u);
}

TEST(Checker, CommitThatSkipsAnIndex) {
  Checker c;
  c.on_commit(t0, n0, Index{2}, Term{1}, p(1));
  EXPECT_FALSE(c.ok());
}

TEST(Checker, LeaderMissingACommittedEntry) {
  Checker c;
  c.on_commit(t0, n0, Index{1}, Term{1}, p(1));
  c.on_commit(t0, n0, Index{2}, Term{1}, p(2));
  c.on_became_leader(t0, n1, Term{2}, log_of({{1, 1}}));          // too short
  c.on_became_leader(t0, n2, Term{3}, log_of({{1, 1}, {1, 9}}));  // wrong payload
  ASSERT_EQ(c.violations().size(), 2u);
  EXPECT_NE(c.violations()[0].find("missing committed entry 2"), std::string::npos);
}

}  // namespace
}  // namespace raftdst::harness
