#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "common/bytes.h"
#include "common/entry_log.h"
#include "common/rng.h"
#include "env/node.h"
#include "sim/world.h"

namespace raftdst::sim::test {

// Durability oracle shared by all PingNodes in one run. A node records its
// counter only after sync() has returned, which it cannot do if it died in
// the sync. On restart the recovered counter must be at least that value.
struct PingOracle {
  std::array<uint64_t, kMaxNodes> synced_counter{};
  uint64_t violations = 0;
  uint64_t restarts_checked = 0;
};

// Toy protocol that touches every part of the environment: random pings to
// peers, a durable counter and log written on every ping received, occasional
// log rewrites that truncate, and a randomised timer. Not Raft; it exists to
// prove the simulator is deterministic before Raft runs on it.
class PingNode final : public INode {
 public:
  PingNode(NodeContext& ctx, PingOracle& oracle) : ctx_(ctx), oracle_(oracle), rng_(ctx.seed) {
    Recovered r = ctx_.storage.load();
    counter_ = r.hard_state.term.get();
    log_ = std::move(r.log);
    const uint8_t id = ctx_.self.get();
    if (oracle_.synced_counter[id] > 0 || counter_ > 0) ++oracle_.restarts_checked;
    if (counter_ < oracle_.synced_counter[id] || !log_.invariants_hold()) ++oracle_.violations;
    arm();
  }

  void on_wake() override {
    if (ctx_.cluster_size > 1) {
      auto peer = static_cast<uint8_t>(rng_.below(ctx_.cluster_size - 1u));
      if (peer >= ctx_.self.get()) ++peer;
      send(NodeId{peer}, kPing);
    }
    if (log_.last_index().get() > 0 && rng_.below(8) == 0) rewrite_tail();
    arm();
  }

  void on_frame(NodeId from, std::span<const uint8_t> frame) override {
    ByteReader r{frame};
    const uint8_t kind = r.u8();
    const uint64_t value = r.u64();
    if (!r.ok()) return;
    if (kind == kPong) {
      ++pongs_;
      return;
    }
    counter_ = std::max(counter_, value) + 1;
    ctx_.storage.set_hard_state({Term{counter_}, from});
    log_.append(Term{counter_}, frame);
    ctx_.storage.append(log_.slice(log_.last_index(), 1, SIZE_MAX));
    ctx_.storage.sync();
    oracle_.synced_counter[ctx_.self.get()] = counter_;
    send(from, kPong);
  }

  [[nodiscard]] uint64_t counter() const { return counter_; }
  [[nodiscard]] uint64_t pongs() const { return pongs_; }

 private:
  static constexpr uint8_t kPing = 1;
  static constexpr uint8_t kPong = 2;

  void arm() {
    ctx_.clock.wake_at(ctx_.clock.now() + Duration::ns(rng_.between(Duration::ms(1).count_ns(),
                                                                    Duration::ms(20).count_ns())));
  }

  void send(NodeId to, uint8_t kind) {
    buf_.clear();
    ByteWriter w{buf_};
    w.u8(kind);
    w.u64(counter_);
    ctx_.net.send(to, buf_);
  }

  void rewrite_tail() {
    const uint64_t last = log_.last_index().get();
    const uint64_t first = last > 2 ? last - rng_.below(3) : 1;
    log_.truncate_after(Index{first - 1});
    std::vector<uint8_t> payload(8, static_cast<uint8_t>(rng_.next()));
    log_.append(Term{counter_}, payload);
    ctx_.storage.append(log_.slice_all_from(Index{first}));
    ctx_.storage.sync();
  }

  NodeContext& ctx_;
  PingOracle& oracle_;
  Rng rng_;
  EntryLog log_;
  uint64_t counter_ = 0;
  uint64_t pongs_ = 0;
  std::vector<uint8_t> buf_;
};

inline NodeFactory ping_factory(PingOracle& oracle) {
  return [&oracle](NodeContext& ctx) -> std::unique_ptr<INode> {
    return std::make_unique<PingNode>(ctx, oracle);
  };
}

}  // namespace raftdst::sim::test
