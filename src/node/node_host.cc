#include "node/node_host.h"

#include "raft/codec.h"

namespace raftdst::node {

Recovered NodeHost::load(NodeContext& ctx, IObserver* observer) {
  Recovered r = ctx.storage.load();
  if (observer) observer->on_recovered(ctx.clock.now(), ctx.self, r.hard_state, r.log);
  return r;
}

NodeHost::NodeHost(NodeContext& ctx, const raft::Config& config, IObserver* observer)
    : ctx_(ctx),
      observer_(observer),
      core_(ctx.self, ctx.cluster_size, config, load(ctx, observer), ctx.seed, ctx.clock.now()) {
  ctx_.clock.wake_at(core_.next_deadline());
}

void NodeHost::on_frame(NodeId from, std::span<const uint8_t> frame) {
  raft::Message m;
  if (raft::decode(frame, ctx_.cluster_size, m) != raft::DecodeError::kOk) {
    ++stats_.frames_rejected;
    return;
  }
  if (m.from != from || m.to != ctx_.self) {
    ++stats_.frames_misrouted;
    return;
  }
  core_.step(ctx_.clock.now(), m);
  drain();
}

void NodeHost::on_wake() {
  core_.tick(ctx_.clock.now());
  drain();
}

raft::ProposeResult NodeHost::propose(std::span<const uint8_t> command) {
  RAFT_ASSERT(!command.empty(), "empty commands are reserved for leader no-ops");
  raft::ProposeResult r = core_.propose(ctx_.clock.now(), command);
  drain();
  return r;
}

void NodeHost::drain() {
  const raft::Ready& r = core_.ready();

  // 1-2. Persist and sync before anything leaves this node. Hard state goes
  // first so a torn write can never keep entries from a term whose term
  // bump was lost.
  const bool log_changed = r.persist_from != Index{};
  if (r.hard_state_dirty) ctx_.storage.set_hard_state(r.hard_state);
  if (log_changed) ctx_.storage.append(core_.log().slice_all_from(r.persist_from));
  if (r.hard_state_dirty || log_changed) {
    ctx_.storage.sync();
    ++stats_.syncs;
    if (observer_ && r.hard_state_dirty) {
      observer_->on_hard_state_synced(ctx_.clock.now(), ctx_.self, r.hard_state);
    }
  }
  if (observer_ && core_.role() == raft::Role::kLeader && leader_reported_for_ != core_.term()) {
    leader_reported_for_ = core_.term();
    observer_->on_became_leader(ctx_.clock.now(), ctx_.self, core_.term(), core_.log());
  }

  // 3. Send.
  for (const raft::Message& m : r.messages) {
    tx_.clear();
    raft::encode(m, tx_);
    ctx_.net.send(m.to, tx_);
  }
  stats_.messages_sent += r.messages.size();

  // 4. Apply. The key-value state machine hooks in here.
  for (Index i = reported_commit_.next(); i <= r.commit; i = i.next()) {
    if (observer_) {
      observer_->on_commit(ctx_.clock.now(), ctx_.self, i, core_.log().term_at(i),
                           core_.log().payload_at(i));
    }
  }
  if (r.commit > reported_commit_) reported_commit_ = r.commit;

  // 5.
  core_.advance();
  ctx_.clock.wake_at(core_.next_deadline());
}

}  // namespace raftdst::node
