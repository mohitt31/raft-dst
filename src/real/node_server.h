#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/types.h"
#include "kv/kv_store.h"
#include "node/node_host.h"
#include "raft/core.h"
#include "real/client_protocol.h"
#include "real/file_wal.h"
#include "real/real_clock.h"

namespace raftdst::real {

struct PeerAddress {
  std::string host = "127.0.0.1";
  uint16_t port = 0;
};

struct ServerOptions {
  NodeId self;
  uint8_t cluster_size = 3;
  std::array<PeerAddress, kMaxNodes> peers{};  // indexed by node id
  std::string wal_path;
  raft::Config raft;
  uint64_t seed = 1;
  // A slow peer must not grow memory without bound. Raft tolerates dropped
  // frames, so anything over this is dropped and counted.
  size_t max_send_queue_bytes = 4u << 20;
  Duration reconnect_delay = Duration::ms(100);
};

struct ServerStats {
  uint64_t frames_sent = 0;
  uint64_t frames_dropped_disconnected = 0;
  uint64_t frames_dropped_queue_full = 0;
  uint64_t frames_received = 0;
  uint64_t bad_frames = 0;
  uint64_t connects = 0;
  uint64_t accepts = 0;
  uint64_t client_requests = 0;
  uint64_t client_ok = 0;
  uint64_t client_not_leader = 0;
  uint64_t client_lost = 0;
};

// One Raft node over TCP, driven by a single-threaded poll() loop:
// no locks anywhere, because nothing is shared between threads.
//
// Every node listens on one port and dials every peer, so each pair has two
// connections, one per direction. A connection's first frame says whether it
// is a peer (and which) or a client.
class NodeServer final : public node::IObserver {
 public:
  static std::unique_ptr<NodeServer> start(const ServerOptions& options, std::string* error);
  ~NodeServer() override;

  // One poll iteration: waits at most until the Raft deadline or max_wait.
  void poll_once(Duration max_wait);
  // Loops until stop() is called (from a signal handler or another thread).
  void run();
  void stop() { running_.store(false, std::memory_order_relaxed); }

  [[nodiscard]] uint16_t port() const { return port_; }
  // Peers may be told their ports after start, which is how tests bring up a
  // cluster on ephemeral ports.
  void set_peer(NodeId id, const PeerAddress& address) { options_.peers[id.get()] = address; }
  [[nodiscard]] const node::NodeHost& host() const { return *host_; }
  [[nodiscard]] const ServerStats& stats() const { return stats_; }
  [[nodiscard]] const FileWal& wal() const { return *wal_; }

  // node::IObserver: used to answer clients when their entry applies.
  void on_recovered(Time, NodeId, const HardState&, const EntryLog&) override {}
  void on_hard_state_synced(Time, NodeId, const HardState&) override {}
  void on_became_leader(Time, NodeId, Term, const EntryLog&) override {}
  void on_commit(Time now, NodeId node, Index index, Term entry_term, Term node_term,
                 std::span<const uint8_t> payload) override;
  void on_applied(Time now, NodeId node, Index index, uint64_t digest) override;

 private:
  class Net;

  struct Conn {
    int fd = -1;
    uint64_t id = 0;
    std::vector<uint8_t> in;
    std::vector<uint8_t> out;
    enum class Kind : uint8_t { kUnknown, kPeer, kClient } kind = Kind::kUnknown;
    NodeId peer;
    bool connecting = false;
  };

  struct PendingClient {
    Index index;
    Term term;
    uint64_t conn_id = 0;
  };

  NodeServer() = default;

  void send_to_peer(NodeId to, std::span<const uint8_t> frame);
  void dial(NodeId peer, Time now);
  void accept_new();
  void on_readable(Conn& c);
  void on_writable(Conn& c);
  void handle_frame(Conn& c, std::span<const uint8_t> body);
  void handle_client_request(Conn& c, std::span<const uint8_t> body);
  void queue(Conn& c, const std::vector<uint8_t>& frame);
  void close_conn(Conn& c);
  Conn* find_conn(uint64_t id);

  ServerOptions options_;
  RealClock clock_;
  std::unique_ptr<Net> net_;
  std::unique_ptr<FileWal> wal_;
  std::unique_ptr<NodeContext> ctx_;
  std::unique_ptr<node::NodeHost> host_;
  const kv::KvStore* kv_ = nullptr;

  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::array<Conn, kMaxNodes> outbound_{};
  std::array<Time, kMaxNodes> reconnect_at_{};
  std::vector<Conn> inbound_;
  std::vector<PendingClient> pending_;
  std::vector<Term> commit_term_;  // term of the entry at each committed index
  uint64_t next_conn_id_ = 1;
  std::atomic<bool> running_{true};
  ServerStats stats_;
};

}  // namespace raftdst::real
