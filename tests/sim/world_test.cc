#include "sim/world.h"

#include <gtest/gtest.h>

#include <map>
#include <sstream>
#include <string>

#include "sim/ping_node.h"
#include "support/property.h"

namespace raftdst::sim {
namespace {

using test::ping_factory;
using test::PingNode;
using test::PingOracle;

SimConfig calm(uint64_t seed, uint8_t nodes, Duration run) {
  SimConfig c;
  c.seed = seed;
  c.nodes = nodes;
  c.stop_at = Time::zero() + run;
  return c;
}

SimConfig chaos(uint64_t seed, uint8_t nodes, Duration run) {
  SimConfig c = calm(seed, nodes, run);
  c.net.drop_ppm = 50'000;
  c.net.duplicate_ppm = 20'000;
  c.net.long_delay_ppm = 30'000;
  c.crash.enabled = true;
  c.crash.gap_min = Duration::ms(200);
  c.crash.gap_max = Duration::sec(2);
  c.crash.in_sync_ppm = 300'000;
  c.partition.enabled = true;
  c.partition.gap_min = Duration::ms(500);
  c.partition.gap_max = Duration::sec(3);
  return c;
}

struct TraceLine {
  int64_t t;
  int node;
  std::string kind;
  uint64_t a, b, c;
};

std::vector<TraceLine> parse(const std::string& text) {
  std::vector<TraceLine> out;
  std::istringstream in{text};
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls{line};
    TraceLine tl{};
    std::string n;
    ls >> tl.t >> n >> tl.kind >> tl.a >> tl.b >> tl.c;
    tl.node = std::stoi(n.substr(1));
    out.push_back(tl);
  }
  return out;
}

TEST(World, CalmNetworkLosesNothingAndRespectsDelayBounds) {
  PingOracle oracle;
  SimConfig c = calm(3, 3, Duration::sec(5));
  c.net.delay_min = Duration::ms(2);
  c.net.delay_max = Duration::ms(9);
  c.trace_text = true;
  World w{c, ping_factory(oracle)};
  w.run();

  const auto& s = w.stats();
  EXPECT_GT(s.sends, 500u);
  EXPECT_EQ(s.drop_random + s.drop_partition + s.drop_dead + s.duplicates, 0u);
  EXPECT_LE(s.sends - s.delivered, 10u);  // only the ones still in flight at stop_at
  EXPECT_EQ(oracle.violations, 0u);

  std::map<uint64_t, int64_t> sent_at;
  uint64_t checked = 0;
  for (const auto& l : parse(w.trace().text())) {
    if (l.kind == "send") sent_at[l.b] = l.t;
    if (l.kind == "deliver") {
      const int64_t d = l.t - sent_at.at(l.b);
      ASSERT_GE(d, Duration::ms(2).count_ns());
      ASSERT_LE(d, Duration::ms(9).count_ns());
      ++checked;
    }
  }
  EXPECT_EQ(checked, s.delivered);
}

TEST(World, BlockedLinkDropsInOneDirectionOnly) {
  PingOracle oracle;
  World w{calm(4, 2, Duration::sec(3)), ping_factory(oracle)};
  w.set_link(NodeId{0}, NodeId{1}, false);
  w.run();
  auto* n0 = static_cast<PingNode*>(w.node(NodeId{0}));
  auto* n1 = static_cast<PingNode*>(w.node(NodeId{1}));
  EXPECT_GT(w.stats().drop_partition, 0u);
  EXPECT_EQ(n0->pongs(), 0u);  // its pings never arrive
  EXPECT_EQ(n1->pongs(), 0u);  // node 1's pings arrive, but the pongs are cut
  EXPECT_GT(n0->counter(), 0u);
}

TEST(World, ScriptedCrashAndRestartRecoversDurableState) {
  PingOracle oracle;
  World w{calm(5, 3, Duration::sec(10)), ping_factory(oracle)};
  w.run_until(Time::zero() + Duration::sec(2));
  const uint64_t before = static_cast<PingNode*>(w.node(NodeId{1}))->counter();
  ASSERT_GT(before, 0u);

  w.crash_node(NodeId{1});
  EXPECT_FALSE(w.is_up(NodeId{1}));
  EXPECT_EQ(w.node(NodeId{1}), nullptr);
  w.run_until(Time::zero() + Duration::sec(4));
  EXPECT_GT(w.stats().drop_dead, 0u);
  EXPECT_FALSE(w.is_up(NodeId{1}));  // scripted crashes do not auto-restart

  w.restart_node(NodeId{1});
  EXPECT_EQ(w.incarnation(NodeId{1}), 2u);
  EXPECT_GE(static_cast<PingNode*>(w.node(NodeId{1}))->counter(), before);
  w.run();
  EXPECT_EQ(oracle.violations, 0u);
}

// Only the latest wake_at counts.
TEST(World, OnlyTheLatestWakeupFires) {
  struct Sleeper final : INode {
    explicit Sleeper(NodeContext& c) : ctx(c) {
      ctx.clock.wake_at(Time::zero() + Duration::ms(10));
      ctx.clock.wake_at(Time::zero() + Duration::ms(30));
      ctx.clock.wake_at(Time::zero() + Duration::ms(5));
    }
    void on_frame(NodeId, std::span<const uint8_t>) override {}
    void on_wake() override { woke.push_back(ctx.clock.now()); }
    NodeContext& ctx;
    std::vector<Time> woke;
  };
  World w{calm(1, 1, Duration::sec(1)),
          [](NodeContext& c) -> std::unique_ptr<INode> { return std::make_unique<Sleeper>(c); }};
  w.run();
  auto* s = static_cast<Sleeper*>(w.node(NodeId{0}));
  ASSERT_EQ(s->woke.size(), 1u);
  EXPECT_EQ(s->woke[0], Time::zero() + Duration::ms(5));
  EXPECT_EQ(w.stats().stale_wakes, 2u);
}

// A node that dies inside sync() must not run anything after it: here, the
// reply that follows the sync must never be sent.
TEST(World, CrashInSyncStopsTheNodeAtTheCrashPoint) {
  PingOracle oracle;
  SimConfig c = calm(8, 2, Duration::sec(20));
  c.crash.enabled = true;
  c.crash.gap_min = Duration::ms(50);
  c.crash.gap_max = Duration::ms(300);
  c.crash.in_sync_ppm = 1'000'000;  // every injected crash happens inside a sync
  c.trace_text = true;
  World w{c, ping_factory(oracle)};
  w.run();

  const auto& s = w.stats();
  ASSERT_GT(s.crashes_in_sync, 5u);
  EXPECT_EQ(oracle.violations, 0u);

  // After each crash-in-sync, the dead node sends nothing at that instant.
  auto lines = parse(w.trace().text());
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].kind != "crash-in-sync") continue;
    for (size_t j = i + 1; j < lines.size() && lines[j].t == lines[i].t; ++j) {
      EXPECT_FALSE(lines[j].kind == "send" && lines[j].node == lines[i].node)
          << "node " << lines[i].node << " sent after dying at t=" << lines[i].t;
    }
  }
}

TEST(World, FaultsStopAtFaultsUntil) {
  PingOracle oracle;
  SimConfig c = chaos(21, 5, Duration::sec(60));
  c.faults_until = Time::zero() + Duration::sec(30);
  c.crash.down_max = Duration::sec(1);
  World w{c, ping_factory(oracle)};
  w.run_until(c.faults_until + Duration::sec(2));  // let pending restarts finish
  const SimStats at_end_of_faults = w.stats();
  EXPECT_GT(at_end_of_faults.injected_faults(), 0u);
  w.run();

  const SimStats& s = w.stats();
  EXPECT_EQ(s.drop_random, at_end_of_faults.drop_random);
  EXPECT_EQ(s.duplicates, at_end_of_faults.duplicates);
  EXPECT_EQ(s.long_delays, at_end_of_faults.long_delays);
  EXPECT_EQ(s.crashes, at_end_of_faults.crashes);
  EXPECT_EQ(s.partitions, at_end_of_faults.partitions);
  EXPECT_EQ(s.drop_partition, at_end_of_faults.drop_partition);
  for (uint8_t i = 0; i < 5; ++i) EXPECT_TRUE(w.is_up(NodeId{i}));
  EXPECT_EQ(oracle.violations, 0u);
}

}  // namespace
}  // namespace raftdst::sim
