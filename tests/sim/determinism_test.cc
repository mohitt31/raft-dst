#include <gtest/gtest.h>

#include <set>
#include <string>

#include "sim/ping_node.h"
#include "sim/world.h"
#include "support/property.h"

namespace raftdst::sim {
namespace {

using test::ping_factory;
using test::PingOracle;

SimConfig chaos(uint64_t seed, bool text) {
  SimConfig c;
  c.seed = seed;
  c.nodes = 5;
  c.stop_at = Time::zero() + Duration::sec(60);
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
  c.trace_text = text;
  return c;
}

struct RunResult {
  uint64_t fingerprint;
  std::string text;
  SimStats stats;
  uint64_t oracle_violations;
};

RunResult run(const SimConfig& c) {
  PingOracle oracle;
  World w{c, ping_factory(oracle)};
  w.run();
  return {w.trace().fingerprint(), w.trace().text(), w.stats(), oracle.violations};
}

// The core promise of the simulator.
TEST(Determinism, SameSeedGivesByteIdenticalTrace) {
  testing::for_each_seed(10, [](uint64_t seed) {
    RunResult a = run(chaos(seed, true));
    RunResult b = run(chaos(seed, true));
    ASSERT_EQ(a.fingerprint, b.fingerprint);
    ASSERT_EQ(a.text, b.text);  // byte-identical, not just same hash
    ASSERT_EQ(a.stats, b.stats);
    ASSERT_EQ(a.oracle_violations, 0u);
  });
}

// The run must actually exercise every fault class, or the test above proves
// little.
TEST(Determinism, ChaosRunExercisesEveryFaultClass) {
  RunResult r = run(chaos(1, false));
  EXPECT_GT(r.stats.delivered, 10'000u);
  EXPECT_GT(r.stats.drop_random, 0u);
  EXPECT_GT(r.stats.duplicates, 0u);
  EXPECT_GT(r.stats.long_delays, 0u);
  EXPECT_GT(r.stats.drop_partition, 0u);
  EXPECT_GT(r.stats.drop_dead, 0u);
  EXPECT_GT(r.stats.crashes_in_sync, 0u);
  EXPECT_GT(r.stats.crashes, r.stats.crashes_in_sync);
  EXPECT_GT(r.stats.restarts, 0u);
  EXPECT_GT(r.stats.partitions, 0u);
}

TEST(Determinism, TextTracingDoesNotChangeBehaviour) {
  testing::for_each_seed(5, [](uint64_t seed) {
    RunResult with = run(chaos(seed, true));
    RunResult without = run(chaos(seed, false));
    ASSERT_EQ(with.fingerprint, without.fingerprint);
    ASSERT_EQ(with.stats, without.stats);
    ASSERT_TRUE(without.text.empty());
  });
}

TEST(Determinism, DifferentSeedsDiverge) {
  std::set<uint64_t> prints;
  for (uint64_t seed = 100; seed < 120; ++seed) prints.insert(run(chaos(seed, false)).fingerprint);
  EXPECT_EQ(prints.size(), 20u);
}

// What the reducer relies on: turning one fault class off leaves the random
// schedule of another class untouched. Crash injection does not depend on
// messages when no crash happens inside a sync, so crash and partition
// events must be identical with and without message drops.
TEST(Determinism, FaultStreamsAreIndependent) {
  auto schedule = [](bool drops) {
    SimConfig c = chaos(7, true);
    c.crash.in_sync_ppm = 0;
    c.net.drop_ppm = drops ? 200'000 : 0;
    c.net.duplicate_ppm = 0;
    c.net.long_delay_ppm = 0;
    std::string out;
    std::string text = run(c).text;
    size_t pos = 0;
    while (pos < text.size()) {
      size_t end = text.find('\n', pos);
      std::string line = text.substr(pos, end - pos);
      for (const char* k : {" crash ", " restart ", " partition ", " heal "}) {
        if (line.find(k) != std::string::npos) out += line + "\n";
      }
      pos = end + 1;
    }
    return out;
  };
  const std::string with_drops = schedule(true);
  const std::string without_drops = schedule(false);
  EXPECT_FALSE(with_drops.empty());
  EXPECT_EQ(with_drops, without_drops);
}

}  // namespace
}  // namespace raftdst::sim
