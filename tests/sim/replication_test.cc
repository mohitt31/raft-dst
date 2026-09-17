#include <gtest/gtest.h>

#include "harness/scenario.h"
#include "harness/sim_cluster.h"
#include "harness/workload.h"
#include "kv/kv_store.h"
#include "support/property.h"

namespace raftdst::harness {
namespace {

void expect_ok(const ScenarioResult& r) {
  for (const auto& v : r.violations) ADD_FAILURE() << v;
}

ScenarioConfig calm(uint64_t seed, uint8_t nodes, Duration run) {
  ScenarioConfig c;
  c.sim.seed = seed;
  c.sim.nodes = nodes;
  c.sim.stop_at = Time::zero() + run;
  return c;
}

TEST(Replication, CalmClusterAcknowledgesWritesAndConverges) {
  for (uint8_t n : {1, 3, 5}) {
    SCOPED_TRACE("nodes=" + std::to_string(n));
    ScenarioConfig cfg = calm(1, n, Duration::sec(11));
    SimCluster cluster{cfg.sim, cfg.raft};
    Workload w{cfg.workload, 99};
    // Writes for 10 s, then one quiet second: followers learn the final
    // commit index from the next heartbeat, not at the moment it commits.
    const Time writes_end = Time::zero() + Duration::sec(10);
    while (w.next_at() < writes_end) {
      cluster.world().run_until(w.next_at());
      w.step(cluster);
      w.resolve(cluster);
    }
    cluster.world().run();
    w.resolve(cluster);
    cluster.check_node_stats();
    for (const auto& v : cluster.checker().violations()) ADD_FAILURE() << v;

    const auto& s = w.stats();
    EXPECT_GT(s.proposed, 500u);
    EXPECT_EQ(s.lost, 0u);  // nothing fails, so nothing is lost
    EXPECT_EQ(w.pending(), 0u);
    EXPECT_EQ(s.acknowledged, s.proposed);

    // Every node has applied everything committed and holds the same state.
    const uint64_t committed = cluster.checker().committed_entries();
    const uint64_t digest = cluster.host(NodeId{0})->state_machine().digest();
    for (uint8_t i = 0; i < n; ++i) {
      auto* h = cluster.host(NodeId{i});
      EXPECT_EQ(h->applied(), Index{committed}) << "node " << int{i};
      EXPECT_EQ(h->state_machine().digest(), digest) << "node " << int{i};
    }
  }
}

TEST(Replication, BatchesProposalsIntoFewerSyncsThanEntries) {
  ScenarioConfig cfg = calm(2, 3, Duration::sec(5));
  cfg.workload.interval_min = Duration::ns(100'000);  // many proposals per round trip
  cfg.workload.interval_max = Duration::ns(200'000);
  SimCluster cluster{cfg.sim, cfg.raft};
  Workload w{cfg.workload, 7};
  while (w.next_at() < cfg.sim.stop_at) {
    cluster.world().run_until(w.next_at());
    w.step(cluster);
    w.resolve(cluster);
  }
  cluster.world().run();
  // Followers receive many entries per AppendEntries, so they sync far less
  // often than once per entry.
  const auto leader = cluster.agreed_leader();
  ASSERT_TRUE(leader.has_value());
  for (uint8_t i = 0; i < 3; ++i) {
    if (NodeId{i} == leader->leader) continue;
    EXPECT_LT(cluster.host(NodeId{i})->stats().syncs * 5, cluster.checker().committed_entries());
  }
}

TEST(Replication, WritesResumeAfterLeaderCrash) {
  ScenarioConfig cfg = calm(3, 5, Duration::sec(20));
  SimCluster cluster{cfg.sim, cfg.raft};
  Workload w{cfg.workload, 5};
  auto run_to = [&](Time t) {
    while (w.next_at() < t) {
      cluster.world().run_until(w.next_at());
      w.step(cluster);
      w.resolve(cluster);
    }
    cluster.world().run_until(t);
  };
  run_to(Time::zero() + Duration::sec(5));
  const auto first = cluster.agreed_leader();
  ASSERT_TRUE(first.has_value());
  const uint64_t acked_before = w.stats().acknowledged;
  cluster.world().crash_node(first->leader);
  run_to(Time::zero() + Duration::sec(12));
  cluster.world().restart_node(first->leader);
  run_to(cfg.sim.stop_at);
  w.resolve(cluster);

  EXPECT_GT(w.acknowledged_proposed_since(Time::zero() + Duration::sec(6)), 300u);
  EXPECT_GT(w.stats().acknowledged, acked_before);
  // The restarted node replays the log into a fresh state machine and catches up.
  const auto* old = cluster.host(first->leader);
  const uint64_t digest = cluster.host(cluster.agreed_leader()->leader)->state_machine().digest();
  EXPECT_EQ(old->state_machine().digest(), digest);
  cluster.check_node_stats();
  for (const auto& v : cluster.checker().violations()) ADD_FAILURE() << v;
}

// The main property test for this slice: every fault class, a live
// workload, continuous safety and reference-model checks, and commit
// liveness once faults stop.
TEST(Replication, ChaosWithWorkloadIsSafeAndLive) {
  uint64_t acked = 0, lost = 0, crashes = 0, in_sync = 0, elections = 0;
  testing::for_each_seed(30, [&](uint64_t seed) {
    for (uint8_t n : {3, 5}) {
      SCOPED_TRACE("nodes=" + std::to_string(n));
      const ScenarioResult r = run_scenario(chaos_scenario(seed, n));
      expect_ok(r);
      if (::testing::Test::HasFailure()) return;
      acked += r.workload.acknowledged;
      lost += r.workload.lost;
      crashes += r.sim.crashes;
      in_sync += r.sim.crashes_in_sync;
      elections += r.leaders_elected;
    }
  });
  // Not vacuous: real writes, real losses under faults, real crashes.
  EXPECT_GT(acked, 60u * 1000);
  EXPECT_GT(lost, 0u);
  EXPECT_GT(crashes, 60u * 5);
  EXPECT_GT(in_sync, 60u);
  EXPECT_GT(elections, 60u * 3);
}

TEST(Replication, ScenarioIsDeterministic) {
  const ScenarioResult a = run_scenario(chaos_scenario(21, 5));
  const ScenarioResult b = run_scenario(chaos_scenario(21, 5));
  EXPECT_EQ(a.fingerprint, b.fingerprint);
  EXPECT_EQ(a.workload, b.workload);
  EXPECT_EQ(a.sim, b.sim);
  EXPECT_NE(a.fingerprint, run_scenario(chaos_scenario(22, 5)).fingerprint);
}

}  // namespace
}  // namespace raftdst::harness
