#include <gtest/gtest.h>

#include "harness/sim_cluster.h"
#include "support/property.h"

namespace raftdst::harness {
namespace {

sim::SimConfig calm(uint64_t seed, uint8_t nodes, Duration run) {
  sim::SimConfig c;
  c.seed = seed;
  c.nodes = nodes;
  c.stop_at = Time::zero() + run;
  return c;
}

sim::SimConfig chaos(uint64_t seed, uint8_t nodes) {
  sim::SimConfig c = calm(seed, nodes, Duration{});
  c.net.drop_ppm = 50'000;
  c.net.duplicate_ppm = 20'000;
  c.net.long_delay_ppm = 30'000;
  c.crash.enabled = true;
  c.crash.gap_min = Duration::ms(300);
  c.crash.gap_max = Duration::sec(3);
  c.crash.in_sync_ppm = 300'000;
  c.partition.enabled = true;
  c.partition.gap_min = Duration::ms(500);
  c.partition.gap_max = Duration::sec(4);
  c.faults_until = Time::zero() + Duration::sec(30);
  // Faults stop at 30 s, the last crashed node is back by 33 s, and the
  // cluster then gets 15 s (50 maximum election timeouts) to settle.
  c.stop_at = c.faults_until + c.crash.down_max + Duration::sec(15);
  return c;
}

void expect_clean(SimCluster& cluster) {
  cluster.check_node_stats();
  for (const auto& v : cluster.checker().violations()) ADD_FAILURE() << v;
}

TEST(Election, CalmClusterAgreesOnOneLeader) {
  for (uint8_t n : {1, 3, 5}) {
    SimCluster cluster{calm(1, n, Duration::sec(2)), raft::Config{}};
    cluster.world().run();
    const auto l = cluster.agreed_leader();
    ASSERT_TRUE(l.has_value()) << "cluster of " << int{n};
    for (uint8_t i = 0; i < n; ++i) {
      EXPECT_GE(cluster.host(NodeId{i})->core().commit_index(), Index{1});  // the no-op
    }
    expect_clean(cluster);
  }
}

TEST(Election, LeaderCrashLeadsToNewLeaderAndOldOneRejoins) {
  SimCluster cluster{calm(2, 5, Duration::sec(10)), raft::Config{}};
  auto& w = cluster.world();
  w.run_until(Time::zero() + Duration::sec(2));
  const auto first = cluster.agreed_leader();
  ASSERT_TRUE(first.has_value());

  w.crash_node(first->leader);
  w.run_until(Time::zero() + Duration::sec(4));
  NodeId next = NodeId::none();
  for (uint8_t i = 0; i < 5; ++i) {
    auto* h = cluster.host(NodeId{i});
    if (h && h->core().role() == raft::Role::kLeader) next = NodeId{i};
  }
  ASSERT_TRUE(next.valid());
  EXPECT_NE(next, first->leader);
  EXPECT_GT(cluster.host(next)->core().term(), first->term);

  w.restart_node(first->leader);
  w.run();
  const auto last = cluster.agreed_leader();
  ASSERT_TRUE(last.has_value());
  EXPECT_NE(cluster.host(first->leader)->core().role(), raft::Role::kLeader);
  expect_clean(cluster);
}

// Without CheckQuorum an isolated leader keeps believing it leads. The
// majority side must still elect a new leader, and on heal the stale one
// must step down.
TEST(Election, IsolatedLeaderIsReplacedAndStepsDownOnHeal) {
  SimCluster cluster{calm(3, 5, Duration::sec(10)), raft::Config{}};
  auto& w = cluster.world();
  w.run_until(Time::zero() + Duration::sec(2));
  const auto first = cluster.agreed_leader();
  ASSERT_TRUE(first.has_value());
  for (uint8_t i = 0; i < 5; ++i) {
    if (NodeId{i} == first->leader) continue;
    w.set_link(first->leader, NodeId{i}, false);
    w.set_link(NodeId{i}, first->leader, false);
  }
  w.run_until(Time::zero() + Duration::sec(5));
  EXPECT_EQ(cluster.host(first->leader)->core().role(), raft::Role::kLeader);
  uint32_t newer_leaders = 0;
  for (uint8_t i = 0; i < 5; ++i) {
    const auto& core = cluster.host(NodeId{i})->core();
    newer_leaders += core.role() == raft::Role::kLeader && core.term() > first->term;
  }
  EXPECT_EQ(newer_leaders, 1u);

  w.heal_all_links();
  w.run();
  const auto last = cluster.agreed_leader();
  ASSERT_TRUE(last.has_value());
  EXPECT_GT(last->term, first->term);
  expect_clean(cluster);
}

// The main property test for this slice: under every fault class, safety
// holds at every step, and once faults stop the cluster agrees on a leader.
TEST(Election, SafetyUnderChaosAndLeaderAfterFaultsStop) {
  uint64_t elections = 0, crashes = 0, partitions = 0;
  testing::for_each_seed(40, [&](uint64_t seed) {
    for (uint8_t n : {3, 5}) {
      SimCluster cluster{chaos(seed, n), raft::Config{}};
      cluster.world().run();
      SCOPED_TRACE("nodes=" + std::to_string(n));
      expect_clean(cluster);
      ASSERT_TRUE(cluster.agreed_leader().has_value()) << "no agreed leader after faults stopped";
      elections += cluster.checker().leaders_elected();
      crashes += cluster.world().stats().crashes;
      partitions += cluster.world().stats().partitions;
    }
  });
  // The faults must have forced real leadership changes.
  EXPECT_GT(elections, 80u * 3);
  EXPECT_GT(crashes, 80u * 5);
  EXPECT_GT(partitions, 80u * 3);
}

TEST(Election, RaftClusterTraceIsDeterministic) {
  auto fingerprint = [](uint64_t seed) {
    SimCluster cluster{chaos(seed, 5), raft::Config{}};
    cluster.world().run();
    return cluster.world().trace().fingerprint();
  };
  EXPECT_EQ(fingerprint(11), fingerprint(11));
  EXPECT_NE(fingerprint(11), fingerprint(12));
}

}  // namespace
}  // namespace raftdst::harness
