#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "common/rng.h"
#include "common/types.h"
#include "env/node.h"
#include "sim/event_queue.h"
#include "sim/faults.h"
#include "sim/sim_storage.h"
#include "sim/trace.h"

namespace raftdst::sim {

using NodeFactory = std::function<std::unique_ptr<INode>(NodeContext&)>;

struct SimStats {
  uint64_t events = 0;
  uint64_t sends = 0;
  uint64_t delivered = 0;
  uint64_t drop_random = 0;
  uint64_t drop_partition = 0;
  uint64_t drop_dead = 0;
  uint64_t duplicates = 0;
  uint64_t long_delays = 0;
  uint64_t crashes = 0;  // includes crashes inside sync()
  uint64_t crashes_in_sync = 0;
  uint64_t restarts = 0;
  uint64_t partitions = 0;
  uint64_t wakes = 0;
  uint64_t stale_wakes = 0;

  // Faults the simulator chose to inject (not their consequences, such as
  // messages lost because a node was down).
  [[nodiscard]] uint64_t injected_faults() const {
    return drop_random + duplicates + long_delays + crashes + partitions;
  }

  friend bool operator==(const SimStats&, const SimStats&) = default;
};

// Single-threaded discrete-event simulator. Owns virtual time, the network,
// every node's disk and every node process. All randomness is derived from
// config.seed; given the same seed and the same node program, two runs
// produce identical traces.
class World {
 public:
  World(const SimConfig& config, NodeFactory factory);
  ~World();
  World(const World&) = delete;
  World& operator=(const World&) = delete;

  // Processes events up to config.stop_at.
  void run() { run_until(config_.stop_at); }
  // Processes every event with time <= limit, then sets now() = limit.
  void run_until(Time limit);
  // Processes one event if its time is <= limit. Returns false otherwise.
  bool step(Time limit);

  [[nodiscard]] Time now() const { return now_; }
  [[nodiscard]] const SimConfig& config() const { return config_; }
  [[nodiscard]] const SimStats& stats() const { return stats_; }
  [[nodiscard]] Trace& trace() { return trace_; }
  [[nodiscard]] const Trace& trace() const { return trace_; }

  [[nodiscard]] bool is_up(NodeId id) const;
  [[nodiscard]] INode* node(NodeId id) const;  // nullptr while down
  [[nodiscard]] const SimStorage& storage(NodeId id) const;
  [[nodiscard]] uint32_t incarnation(NodeId id) const;

  // Runs `f` on node `id` as if it were an event: a crash inside it (for
  // example a sync with an armed crash) stops `f` and kills the node, exactly
  // as for network callbacks. Records a trace entry tagged `tag`. Returns
  // false, without calling `f`, if the node is down.
  bool call_node(NodeId id, uint64_t tag, const std::function<void(INode&)>& f);

  // Scripted faults for scenario tests. Do not mix with random partitions:
  // a random heal clears every link.
  void crash_node(NodeId id);    // stays down until restart_node
  void restart_node(NodeId id);  // no-op if already up
  void set_link(NodeId from, NodeId to, bool up);
  void heal_all_links();

 private:
  struct Slot;
  class SimClock;
  class Endpoint;

  struct Message {
    std::vector<uint8_t> bytes;
    uint64_t id = 0;
    uint8_t from = 0;
    uint8_t to = 0;
  };

  void dispatch(const Event& ev);
  void on_deliver(const Event& ev);
  void on_wake_event(const Event& ev);
  void on_restart(const Event& ev);
  void on_crash_tick();
  void on_partition_start();
  void on_heal(const Event& ev);
  void on_faults_end();

  void send(uint8_t from, uint8_t to, std::span<const uint8_t> bytes);
  void schedule_delivery(uint8_t from, uint8_t to, std::span<const uint8_t> bytes, uint64_t msg_id,
                         bool faulty);
  void schedule_wake(uint8_t id, Time at);
  void start(Slot& s);
  void kill(Slot& s, bool schedule_restart);

  template <class F>
  void invoke(Slot& s, F&& f);

  [[nodiscard]] bool faults_active() const { return now_ < config_.faults_until; }
  [[nodiscard]] Duration draw(Rng& rng, Duration lo, Duration hi);

  SimConfig config_;
  NodeFactory factory_;
  Time now_ = Time::zero();
  EventQueue queue_;
  Trace trace_;
  SimStats stats_;

  std::array<std::unique_ptr<Slot>, kMaxNodes> slots_;
  std::array<std::array<bool, kMaxNodes>, kMaxNodes> blocked_{};

  std::vector<Message> messages_;
  std::vector<uint32_t> free_messages_;
  std::vector<uint8_t> rx_buffer_;
  uint64_t next_msg_id_ = 0;
  uint32_t partition_token_ = 0;

  Rng drop_rng_, dup_rng_, delay_rng_, crash_rng_, partition_rng_;
};

}  // namespace raftdst::sim
