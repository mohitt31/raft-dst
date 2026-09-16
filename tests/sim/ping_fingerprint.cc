// Prints the trace fingerprint of one chaos run of the ping protocol. CTest
// runs it twice in separate processes: address-space randomisation differs
// between processes, so anything that leaks pointer values or other
// per-process state into behaviour shows up as a mismatch. Two runs inside
// one process usually get the same heap layout and would miss it.
#include <cstdio>
#include <cstdlib>

#include "sim/ping_node.h"
#include "sim/world.h"

int main(int argc, char** argv) {
  using namespace raftdst;
  using namespace raftdst::sim;
  SimConfig c;
  c.seed = argc > 1 ? std::strtoull(argv[1], nullptr, 0) : 1;
  c.nodes = 5;
  c.stop_at = Time::zero() + Duration::sec(60);
  c.net.drop_ppm = 50'000;
  c.net.duplicate_ppm = 20'000;
  c.net.long_delay_ppm = 30'000;
  c.crash.enabled = true;
  c.crash.in_sync_ppm = 300'000;
  c.partition.enabled = true;
  test::PingOracle oracle;
  World w{c, test::ping_factory(oracle)};
  w.run();
  std::printf("%016llx %llu\n", static_cast<unsigned long long>(w.trace().fingerprint()),
              static_cast<unsigned long long>(w.trace().records()));
  return oracle.violations == 0 ? 0 : 1;
}
