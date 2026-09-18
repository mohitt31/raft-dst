// Three real nodes, three threads, real sockets and real files on
// 127.0.0.1. This is the only place the project runs threads, so it is what
// the tsan preset exercises. The threads share nothing but the sockets: each
// NodeServer owns its Raft state and runs a single-threaded poll loop.
#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "real/client.h"
#include "real/node_server.h"

namespace raftdst::real {
namespace {

namespace fs = std::filesystem;

class Cluster {
 public:
  explicit Cluster(uint8_t n, const std::string& name) : n_(n) {
    dir_ = fs::temp_directory_path() / (name + "." + std::to_string(::getpid()));
    fs::create_directories(dir_);
    for (uint8_t i = 0; i < n_; ++i) start(NodeId{i});
    // Every node learns the others' ephemeral ports, then starts running.
    for (uint8_t i = 0; i < n_; ++i) {
      for (uint8_t j = 0; j < n_; ++j) {
        servers_[i]->set_peer(NodeId{j}, PeerAddress{"127.0.0.1", servers_[j]->port()});
      }
    }
    for (uint8_t i = 0; i < n_; ++i) spawn(i);
  }

  ~Cluster() {
    for (uint8_t i = 0; i < n_; ++i) kill(NodeId{i});
    std::error_code ignored;
    fs::remove_all(dir_, ignored);
  }

  [[nodiscard]] uint16_t port(NodeId id) const { return ports_[id.get()]; }
  [[nodiscard]] bool up(NodeId id) const { return servers_[id.get()] != nullptr; }

  void kill(NodeId id) {
    const uint8_t i = id.get();
    if (!servers_[i]) return;
    servers_[i]->stop();
    if (threads_[i].joinable()) threads_[i].join();
    servers_[i].reset();  // closes sockets and the WAL
  }

  // Restarts on the same port and the same write-ahead log.
  void restart(NodeId id) {
    kill(id);
    start(id, ports_[id.get()]);
    for (uint8_t j = 0; j < n_; ++j) {
      servers_[id.get()]->set_peer(NodeId{j}, PeerAddress{"127.0.0.1", ports_[j]});
    }
    spawn(id.get());
  }

  // Sends a request to whichever node leads, following NotLeader hints.
  ClientStatus call(const ClientRequest& req, std::string& value, Duration budget) {
    const auto start = std::chrono::steady_clock::now();
    uint8_t target = 0;
    while (std::chrono::steady_clock::now() - start < std::chrono::nanoseconds(budget.count_ns())) {
      if (!servers_[target]) {
        target = static_cast<uint8_t>((target + 1) % n_);
        continue;
      }
      std::string error;
      auto client = Client::connect("127.0.0.1", ports_[target], &error);
      if (!client) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        target = static_cast<uint8_t>((target + 1) % n_);
        continue;
      }
      ClientResponse resp;
      if (!client->request(req, resp, Duration::sec(2), &error)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        target = static_cast<uint8_t>((target + 1) % n_);
        continue;
      }
      if (resp.status == ClientStatus::kNotLeader) {
        target = resp.leader_hint.valid() && resp.leader_hint.get() < n_
                     ? resp.leader_hint.get()
                     : static_cast<uint8_t>((target + 1) % n_);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      value.assign(resp.value);
      return resp.status;
    }
    return ClientStatus::kNotLeader;  // never reached a leader in time
  }

  ClientStatus put(const std::string& key, const std::string& value, Duration budget) {
    std::string ignored;
    return call({ClientOp::kPut, key, value}, ignored, budget);
  }

  ClientStatus get(const std::string& key, std::string& value, Duration budget) {
    return call({ClientOp::kGet, key, ""}, value, budget);
  }

 private:
  void start(NodeId id, uint16_t port = 0) {
    ServerOptions options;
    options.self = id;
    options.cluster_size = n_;
    options.wal_path = (dir_ / ("node" + std::to_string(id.get()) + ".wal")).string();
    options.seed = 1000 + id.get();
    options.peers[id.get()] = PeerAddress{"127.0.0.1", port};
    options.raft.election_timeout_min = Duration::ms(150);
    options.raft.election_timeout_max = Duration::ms(300);
    options.raft.heartbeat_interval = Duration::ms(40);
    std::string error;
    servers_[id.get()] = NodeServer::start(options, &error);
    ASSERT_NE(servers_[id.get()], nullptr) << error;
    ports_[id.get()] = servers_[id.get()]->port();
  }

  void spawn(uint8_t i) {
    threads_[i] = std::thread([this, i] { servers_[i]->run(); });
  }

  uint8_t n_;
  fs::path dir_;
  std::array<std::unique_ptr<NodeServer>, kMaxNodes> servers_;
  std::array<std::thread, kMaxNodes> threads_;
  std::array<uint16_t, kMaxNodes> ports_{};
};

TEST(LoopbackCluster, WritesCommitAndReadBack) {
  Cluster cluster{3, "raft-loopback-basic"};
  for (int i = 0; i < 20; ++i) {
    const std::string key = "k" + std::to_string(i);
    ASSERT_EQ(cluster.put(key, "v" + std::to_string(i), Duration::sec(10)), ClientStatus::kOk)
        << "put " << key;
  }
  for (int i = 0; i < 20; ++i) {
    std::string value;
    ASSERT_EQ(cluster.get("k" + std::to_string(i), value, Duration::sec(10)), ClientStatus::kOk);
    EXPECT_EQ(value, "v" + std::to_string(i));
  }
  std::string missing;
  EXPECT_EQ(cluster.get("absent", missing, Duration::sec(10)), ClientStatus::kNotFound);
}

TEST(LoopbackCluster, SurvivesLosingANodeAndBringsItBack) {
  Cluster cluster{3, "raft-loopback-failover"};
  ASSERT_EQ(cluster.put("before", "1", Duration::sec(10)), ClientStatus::kOk);

  // Find who leads by asking; then kill that node.
  NodeId leader = NodeId::none();
  for (uint8_t i = 0; i < 3 && !leader.valid(); ++i) {
    std::string error;
    if (auto c = Client::connect("127.0.0.1", cluster.port(NodeId{i}), &error)) {
      ClientResponse resp;
      if (c->request({ClientOp::kGet, "before", ""}, resp, Duration::sec(2), &error) &&
          resp.status != ClientStatus::kNotLeader) {
        leader = NodeId{i};
      }
    }
  }
  ASSERT_TRUE(leader.valid());

  cluster.kill(leader);
  // Two of three remain: writes must still commit, after an election.
  EXPECT_EQ(cluster.put("during", "2", Duration::sec(15)), ClientStatus::kOk);

  cluster.restart(leader);
  // The restarted node recovers from its own write-ahead log and catches up.
  EXPECT_EQ(cluster.put("after", "3", Duration::sec(15)), ClientStatus::kOk);
  std::string value;
  EXPECT_EQ(cluster.get("before", value, Duration::sec(15)), ClientStatus::kOk);
  EXPECT_EQ(value, "1");
  EXPECT_EQ(cluster.get("during", value, Duration::sec(15)), ClientStatus::kOk);
  EXPECT_EQ(value, "2");
}

}  // namespace
}  // namespace raftdst::real
