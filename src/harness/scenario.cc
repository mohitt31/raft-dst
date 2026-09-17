#include "harness/scenario.h"

#include <algorithm>
#include <string>

#include "harness/sim_cluster.h"

namespace raftdst::harness {

ScenarioResult run_scenario(const ScenarioConfig& config) {
  SimCluster cluster{config.sim, config.raft};
  sim::World& world = cluster.world();
  const Time stop = config.sim.stop_at;
  Workload workload{config.workload, Rng::derive(config.sim.seed, sim::stream::kWorkload).next()};

  const bool has_quiet_period = config.sim.faults_until < stop;
  const Time settle_point =
      has_quiet_period ? config.sim.faults_until + config.sim.crash.down_max + config.settle
                       : Time::max();

  while (true) {
    const Time next = config.workload_enabled ? std::min(workload.next_at(), stop) : stop;
    world.run_until(next);
    if (config.stop_at_first_violation && !cluster.checker().ok()) break;
    if (next >= stop) break;
    workload.step(cluster);
    workload.resolve(cluster);
    if (config.stop_at_first_violation && !cluster.checker().ok()) break;
  }
  workload.resolve(cluster);

  const bool stopped_early = world.now() < stop;
  if (!stopped_early) {
    cluster.check_node_stats();
    if (has_quiet_period && settle_point < stop) {
      if (!cluster.agreed_leader()) {
        cluster.checker().fail(world.now(), "no agreed leader after faults stopped");
      }
      if (config.workload_enabled) {
        const uint64_t acks = workload.acknowledged_proposed_since(settle_point);
        if (acks < config.min_acks_after_settle) {
          cluster.checker().fail(world.now(), "only " + std::to_string(acks) +
                                                  " writes acknowledged after the settle point");
        }
      }
    }
  }

  ScenarioResult r;
  r.violations = cluster.checker().violations();
  r.first_violation_at = cluster.checker().first_violation_at();
  r.ended_at = world.now();
  r.sim = world.stats();
  r.workload = workload.stats();
  r.fingerprint = world.trace().fingerprint();
  r.committed_entries = cluster.checker().committed_entries();
  r.leaders_elected = cluster.checker().leaders_elected();
  return r;
}

ScenarioConfig chaos_scenario(uint64_t seed, uint8_t nodes, Duration fault_window) {
  ScenarioConfig c;
  c.sim.seed = seed;
  c.sim.nodes = nodes;
  c.sim.net.drop_ppm = 50'000;
  c.sim.net.duplicate_ppm = 20'000;
  c.sim.net.long_delay_ppm = 30'000;
  c.sim.crash.enabled = true;
  c.sim.crash.gap_min = Duration::ms(300);
  c.sim.crash.gap_max = Duration::sec(3);
  c.sim.crash.in_sync_ppm = 300'000;
  c.sim.partition.enabled = true;
  c.sim.partition.gap_min = Duration::ms(500);
  c.sim.partition.gap_max = Duration::sec(4);
  c.sim.faults_until = Time::zero() + fault_window;
  c.sim.stop_at = c.sim.faults_until + c.sim.crash.down_max + c.settle + Duration::sec(10);
  return c;
}

}  // namespace raftdst::harness
