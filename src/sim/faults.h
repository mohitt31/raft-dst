#pragma once

#include <cstdint>

#include "common/types.h"

namespace raftdst::sim {

// Probabilities are integer parts per million. No floating point anywhere in
// the simulator.

struct NetworkFaults {
  Duration delay_min = Duration::ms(1);
  Duration delay_max = Duration::ms(5);
  uint32_t drop_ppm = 0;
  uint32_t duplicate_ppm = 0;
  uint32_t long_delay_ppm = 0;  // extra delay in [0, long_delay_max]; causes reordering
  Duration long_delay_max = Duration::ms(500);
};

struct CrashFaults {
  bool enabled = false;
  Duration gap_min = Duration::sec(1);  // time between crash injections
  Duration gap_max = Duration::sec(5);
  Duration down_min = Duration::ms(100);
  Duration down_max = Duration::sec(3);
  uint32_t in_sync_ppm = 0;  // share of crashes that happen inside the node's next sync()
};

struct PartitionFaults {
  bool enabled = false;
  Duration gap_min = Duration::sec(1);
  Duration gap_max = Duration::sec(10);
  Duration length_min = Duration::ms(200);
  Duration length_max = Duration::sec(5);
};

struct SimConfig {
  uint64_t seed = 1;
  uint8_t nodes = 3;
  NetworkFaults net;
  CrashFaults crash;
  PartitionFaults partition;
  // Random faults stop here: partitions heal, pending in-sync crashes are
  // disarmed, drops/duplicates/long delays stop. Crashed nodes still restart
  // on schedule. Used to check that the cluster recovers.
  Time faults_until = Time::max();
  Time stop_at = Time::zero() + Duration::sec(10);
  bool trace_text = false;
};

// Independent random streams derived from the one seed. Each fault class has
// its own stream so switching one off does not shift the draws of another.
namespace stream {
inline constexpr uint64_t kNetDrop = 1;
inline constexpr uint64_t kNetDuplicate = 2;
inline constexpr uint64_t kNetDelay = 3;
inline constexpr uint64_t kCrash = 4;
inline constexpr uint64_t kPartition = 5;
inline constexpr uint64_t kTornWrite = 6;
inline constexpr uint64_t kWorkload = 7;
// Node programs: kNodeBase + node * 2^20 + incarnation.
inline constexpr uint64_t kNodeBase = 1u << 16;
}  // namespace stream

}  // namespace raftdst::sim
