#include "sim/world.h"

#include <algorithm>
#include <utility>

namespace raftdst::sim {

class World::SimClock final : public IClock {
 public:
  SimClock(World& w, uint8_t id) : w_(w), id_(id) {}
  [[nodiscard]] Time now() const override { return w_.now_; }
  void wake_at(Time t) override { w_.schedule_wake(id_, t); }

 private:
  World& w_;
  uint8_t id_;
};

class World::Endpoint final : public INetwork {
 public:
  Endpoint(World& w, uint8_t id) : w_(w), id_(id) {}
  void send(NodeId to, std::span<const uint8_t> frame) override { w_.send(id_, to.get(), frame); }

 private:
  World& w_;
  uint8_t id_;
};

struct World::Slot {
  Slot(World& w, uint8_t id, uint8_t n, uint64_t torn_seed)
      : clock(w, id),
        net(w, id),
        storage(torn_seed),
        ctx{NodeId{id}, n, 0, clock, net, storage},
        id(id) {}

  SimClock clock;
  Endpoint net;
  SimStorage storage;
  NodeContext ctx;
  std::unique_ptr<INode> proc;
  uint8_t id;
  bool up = false;
  uint32_t timer_gen = 0;
  uint32_t incarnation = 0;
  uint32_t restart_token = 0;
};

World::World(const SimConfig& config, NodeFactory factory)
    : config_(config),
      factory_(std::move(factory)),
      trace_(config.trace_text),
      drop_rng_(Rng::derive(config.seed, stream::kNetDrop)),
      dup_rng_(Rng::derive(config.seed, stream::kNetDuplicate)),
      delay_rng_(Rng::derive(config.seed, stream::kNetDelay)),
      crash_rng_(Rng::derive(config.seed, stream::kCrash)),
      partition_rng_(Rng::derive(config.seed, stream::kPartition)) {
  RAFT_ASSERT(config_.nodes >= 1 && config_.nodes <= kMaxNodes, "cluster size out of range");
  RAFT_ASSERT(config_.net.delay_min >= Duration{} && config_.net.delay_min <= config_.net.delay_max,
              "bad network delay range");
  queue_.reserve(1024);

  for (uint8_t i = 0; i < config_.nodes; ++i) {
    const uint64_t torn_seed = Rng::derive(config_.seed, (stream::kTornWrite << 8) | i).next();
    slots_[i] = std::make_unique<Slot>(*this, i, config_.nodes, torn_seed);
  }

  if (config_.crash.enabled) {
    queue_.push(now_ + draw(crash_rng_, config_.crash.gap_min, config_.crash.gap_max),
                EventKind::kCrashTick, 0, 0);
  }
  if (config_.partition.enabled) {
    queue_.push(now_ + draw(partition_rng_, config_.partition.gap_min, config_.partition.gap_max),
                EventKind::kPartitionStart, 0, 0);
  }
  if (config_.faults_until != Time::max()) {
    queue_.push(config_.faults_until, EventKind::kFaultsEnd, 0, 0);
  }

  for (uint8_t i = 0; i < config_.nodes; ++i) start(*slots_[i]);
}

World::~World() = default;

Duration World::draw(Rng& rng, Duration lo, Duration hi) {
  return Duration::ns(rng.between(lo.count_ns(), hi.count_ns()));
}

void World::run_until(Time limit) {
  while (step(limit)) {
  }
  if (now_ < limit) now_ = limit;
}

bool World::step(Time limit) {
  if (queue_.empty() || queue_.top().at > limit) return false;
  const Event ev = queue_.pop();
  RAFT_DCHECK(ev.at >= now_, "event queue went back in time");
  now_ = ev.at;
  ++stats_.events;
  dispatch(ev);
  return true;
}

void World::dispatch(const Event& ev) {
  switch (ev.kind) {
    case EventKind::kDeliver:
      on_deliver(ev);
      return;
    case EventKind::kWake:
      on_wake_event(ev);
      return;
    case EventKind::kRestart:
      on_restart(ev);
      return;
    case EventKind::kCrashTick:
      on_crash_tick();
      return;
    case EventKind::kPartitionStart:
      on_partition_start();
      return;
    case EventKind::kHeal:
      on_heal(ev);
      return;
    case EventKind::kFaultsEnd:
      on_faults_end();
      return;
  }
  RAFT_ASSERT(false, "unknown event kind");
}

// Runs a node callback. If the node dies inside sync(), the exception stops
// its code at that point and the node is destroyed before anything else runs.
template <class F>
void World::invoke(Slot& s, F&& f) {
  try {
    std::forward<F>(f)();
  } catch (const SimCrash&) {
    ++stats_.crashes_in_sync;
    trace_.record(now_, TraceKind::kCrashInSync, s.id, s.incarnation);
    kill(s, true);
  }
}

void World::start(Slot& s) {
  ++s.incarnation;
  ++s.timer_gen;
  s.up = true;
  s.ctx.seed =
      Rng::derive(config_.seed, stream::kNodeBase + (uint64_t{s.id} << 20) + s.incarnation).next();
  trace_.record(now_, s.incarnation == 1 ? TraceKind::kStart : TraceKind::kRestart, s.id,
                s.incarnation);
  invoke(s, [&] { s.proc = factory_(s.ctx); });
}

void World::kill(Slot& s, bool schedule_restart) {
  s.proc.reset();
  s.up = false;
  ++s.timer_gen;
  s.storage.crash();
  ++stats_.crashes;
  const uint32_t token = ++s.restart_token;
  if (schedule_restart) {
    queue_.push(now_ + draw(crash_rng_, config_.crash.down_min, config_.crash.down_max),
                EventKind::kRestart, s.id, token);
  }
}

// ---------------------------------------------------------------- network

void World::send(uint8_t from, uint8_t to, std::span<const uint8_t> bytes) {
  RAFT_ASSERT(to < config_.nodes, "send to a node outside the cluster");
  ++stats_.sends;
  const uint64_t id = next_msg_id_++;
  Fnv1a64 h;
  h.add_bytes(bytes);
  trace_.record(now_, TraceKind::kSend, from, to, id, h.value());

  if (blocked_[from][to]) {
    ++stats_.drop_partition;
    trace_.record(now_, TraceKind::kDropPartition, from, to, id);
    return;
  }
  const bool faulty = faults_active();
  if (faulty && drop_rng_.chance_ppm(config_.net.drop_ppm)) {
    ++stats_.drop_random;
    trace_.record(now_, TraceKind::kDropRandom, from, to, id);
    return;
  }
  schedule_delivery(from, to, bytes, id, faulty);
  if (faulty && dup_rng_.chance_ppm(config_.net.duplicate_ppm)) {
    ++stats_.duplicates;
    trace_.record(now_, TraceKind::kDuplicate, from, to, id);
    schedule_delivery(from, to, bytes, id, faulty);
  }
}

void World::schedule_delivery(uint8_t from, uint8_t to, std::span<const uint8_t> bytes,
                              uint64_t msg_id, bool faulty) {
  Duration delay = draw(delay_rng_, config_.net.delay_min, config_.net.delay_max);
  if (faulty && delay_rng_.chance_ppm(config_.net.long_delay_ppm)) {
    ++stats_.long_delays;
    delay = delay + draw(delay_rng_, Duration{}, config_.net.long_delay_max);
  }

  uint32_t slot;
  if (free_messages_.empty()) {
    slot = static_cast<uint32_t>(messages_.size());
    messages_.emplace_back();
  } else {
    slot = free_messages_.back();
    free_messages_.pop_back();
  }
  Message& m = messages_[slot];
  m.bytes.assign(bytes.begin(), bytes.end());
  m.id = msg_id;
  m.from = from;
  m.to = to;
  queue_.push(now_ + delay, EventKind::kDeliver, to, slot);
}

void World::on_deliver(const Event& ev) {
  const uint32_t slot = ev.arg;
  const uint8_t from = messages_[slot].from;
  const uint8_t to = messages_[slot].to;
  const uint64_t id = messages_[slot].id;
  Slot& s = *slots_[to];

  if (!s.up) {
    ++stats_.drop_dead;
    trace_.record(now_, TraceKind::kDropDead, to, from, id);
  } else if (blocked_[from][to]) {
    ++stats_.drop_partition;
    trace_.record(now_, TraceKind::kDropPartition, from, to, id);
  } else {
    ++stats_.delivered;
    trace_.record(now_, TraceKind::kDeliver, to, from, id);
    // The node may send while handling this frame, which can grow messages_.
    // Swap the bytes into a buffer that nothing else touches until we return.
    rx_buffer_.swap(messages_[slot].bytes);
    invoke(s, [&] { s.proc->on_frame(NodeId{from}, rx_buffer_); });
    rx_buffer_.swap(messages_[slot].bytes);
  }
  free_messages_.push_back(slot);
}

// ------------------------------------------------------------------ timers

void World::schedule_wake(uint8_t id, Time at) {
  Slot& s = *slots_[id];
  RAFT_ASSERT(s.up, "wake_at from a node that is down");
  ++s.timer_gen;
  queue_.push(std::max(at, now_), EventKind::kWake, id, s.timer_gen);
}

void World::on_wake_event(const Event& ev) {
  Slot& s = *slots_[ev.node];
  if (!s.up || ev.arg != s.timer_gen) {
    ++stats_.stale_wakes;
    return;
  }
  ++stats_.wakes;
  trace_.record(now_, TraceKind::kWake, s.id);
  invoke(s, [&] { s.proc->on_wake(); });
}

// ------------------------------------------------------------------ faults

void World::on_restart(const Event& ev) {
  Slot& s = *slots_[ev.node];
  if (s.up || ev.arg != s.restart_token) return;
  ++stats_.restarts;
  start(s);
}

void World::on_crash_tick() {
  if (!faults_active()) return;

  std::array<uint8_t, kMaxNodes> candidates{};
  uint64_t n = 0;
  for (uint8_t i = 0; i < config_.nodes; ++i) {
    if (slots_[i]->up) candidates[n++] = i;
  }
  if (n > 0) {
    Slot& s = *slots_[candidates[crash_rng_.below(n)]];
    if (!s.storage.crash_armed() && crash_rng_.chance_ppm(config_.crash.in_sync_ppm)) {
      s.storage.arm_crash_in_sync();
      trace_.record(now_, TraceKind::kArmCrashInSync, s.id, s.incarnation);
    } else {
      trace_.record(now_, TraceKind::kCrash, s.id, s.incarnation);
      kill(s, true);
    }
  }
  queue_.push(now_ + draw(crash_rng_, config_.crash.gap_min, config_.crash.gap_max),
              EventKind::kCrashTick, 0, 0);
}

void World::on_partition_start() {
  if (!faults_active()) return;
  const uint8_t n = config_.nodes;
  if (n < 2) return;

  const uint64_t kind = partition_rng_.below(3);
  uint64_t detail = 0;
  if (kind == 0) {  // isolate one node in both directions
    const auto x = static_cast<uint8_t>(partition_rng_.below(n));
    for (uint8_t i = 0; i < n; ++i) {
      if (i == x) continue;
      blocked_[x][i] = blocked_[i][x] = true;
    }
    detail = x;
  } else if (kind == 1) {  // split into two non-empty groups
    const uint64_t mask = 1 + partition_rng_.below((uint64_t{1} << n) - 2);
    for (uint8_t i = 0; i < n; ++i) {
      for (uint8_t j = 0; j < n; ++j) {
        blocked_[i][j] = ((mask >> i) & 1) != ((mask >> j) & 1);
      }
    }
    detail = mask;
  } else {  // cut one direction of one link
    const auto a = static_cast<uint8_t>(partition_rng_.below(n));
    const auto b = static_cast<uint8_t>((a + 1 + partition_rng_.below(n - 1u)) % n);
    blocked_[a][b] = true;
    detail = (uint64_t{a} << 8) | b;
  }

  ++stats_.partitions;
  const uint32_t token = ++partition_token_;
  trace_.record(now_, TraceKind::kPartition, 0, kind, detail, token);
  queue_.push(
      now_ + draw(partition_rng_, config_.partition.length_min, config_.partition.length_max),
      EventKind::kHeal, 0, token);
}

void World::on_heal(const Event& ev) {
  if (ev.arg != partition_token_) return;
  heal_all_links();
  trace_.record(now_, TraceKind::kHeal, 0, ev.arg);
  if (faults_active()) {
    queue_.push(now_ + draw(partition_rng_, config_.partition.gap_min, config_.partition.gap_max),
                EventKind::kPartitionStart, 0, 0);
  }
}

void World::on_faults_end() {
  ++partition_token_;  // any pending heal becomes stale
  heal_all_links();
  for (uint8_t i = 0; i < config_.nodes; ++i) slots_[i]->storage.disarm_crash_in_sync();
  trace_.record(now_, TraceKind::kFaultsEnd, 0);
}

bool World::call_node(NodeId id, uint64_t tag, const std::function<void(INode&)>& f) {
  Slot& s = *slots_[id.get()];
  if (!s.up || !s.proc) return false;
  trace_.record(now_, TraceKind::kUser, s.id, tag);
  invoke(s, [&] { f(*s.proc); });
  return true;
}

// ---------------------------------------------------------- scripted faults

bool World::is_up(NodeId id) const { return slots_[id.get()]->up; }
INode* World::node(NodeId id) const { return slots_[id.get()]->proc.get(); }
const SimStorage& World::storage(NodeId id) const { return slots_[id.get()]->storage; }
uint32_t World::incarnation(NodeId id) const { return slots_[id.get()]->incarnation; }

void World::crash_node(NodeId id) {
  Slot& s = *slots_[id.get()];
  if (!s.up) return;
  trace_.record(now_, TraceKind::kCrash, s.id, s.incarnation);
  kill(s, false);
}

void World::restart_node(NodeId id) {
  Slot& s = *slots_[id.get()];
  if (s.up) return;
  ++s.restart_token;  // cancels any scheduled restart
  ++stats_.restarts;
  start(s);
}

void World::set_link(NodeId from, NodeId to, bool up) { blocked_[from.get()][to.get()] = !up; }

void World::heal_all_links() {
  for (auto& row : blocked_) row.fill(false);
}

}  // namespace raftdst::sim
