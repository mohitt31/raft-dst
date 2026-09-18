// One Raft node over TCP.
//
//   raftd --id 0 --peers 127.0.0.1:7000,127.0.0.1:7001,127.0.0.1:7002 \
//         --wal /var/tmp/raft0.wal
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "real/node_server.h"

namespace {

raftdst::real::NodeServer* g_server = nullptr;

void on_signal(int) {
  if (g_server != nullptr) g_server->stop();
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == sep) {
      parts.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return parts;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace raftdst;
  real::ServerOptions options;
  std::string peers_arg;
  int id = -1;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (std::strcmp(a, "--id") == 0) {
      id = std::atoi(next());
    } else if (std::strcmp(a, "--peers") == 0) {
      peers_arg = next();
    } else if (std::strcmp(a, "--wal") == 0) {
      options.wal_path = next();
    } else if (std::strcmp(a, "--seed") == 0) {
      options.seed = std::strtoull(next(), nullptr, 10);
    } else if (std::strcmp(a, "--election-ms") == 0) {
      const int64_t ms = std::atoi(next());
      options.raft.election_timeout_min = Duration::ms(ms);
      options.raft.election_timeout_max = Duration::ms(2 * ms);
      options.raft.heartbeat_interval = Duration::ms(ms / 3);
    } else {
      std::fprintf(stderr, "usage: %s --id N --peers host:port,... --wal PATH [--seed N]\n",
                   argv[0]);
      return 2;
    }
  }

  const std::vector<std::string> peers = split(peers_arg, ',');
  if (id < 0 || peers.size() < 1 || peers.size() > kMaxNodes || options.wal_path.empty()) {
    std::fprintf(stderr, "need --id, --peers (1..%d) and --wal\n", kMaxNodes);
    return 2;
  }
  options.self = NodeId{static_cast<uint8_t>(id)};
  options.cluster_size = static_cast<uint8_t>(peers.size());
  if (options.seed == 1) options.seed = static_cast<uint64_t>(id) * 7919 + 1;
  for (size_t i = 0; i < peers.size(); ++i) {
    const auto hp = split(peers[i], ':');
    if (hp.size() != 2) {
      std::fprintf(stderr, "bad peer: %s\n", peers[i].c_str());
      return 2;
    }
    options.peers[i] = real::PeerAddress{hp[0], static_cast<uint16_t>(std::atoi(hp[1].c_str()))};
  }

  std::string error;
  auto server = real::NodeServer::start(options, &error);
  if (!server) {
    std::fprintf(stderr, "start failed: %s\n", error.c_str());
    return 1;
  }
  g_server = server.get();
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::printf("node %d listening on %u, wal %s\n", id, server->port(), options.wal_path.c_str());
  std::fflush(stdout);
  server->run();

  const auto& s = server->stats();
  std::printf(
      "stopped: sent=%llu received=%llu dropped=%llu client_ok=%llu client_lost=%llu\n",
      static_cast<unsigned long long>(s.frames_sent),
      static_cast<unsigned long long>(s.frames_received),
      static_cast<unsigned long long>(s.frames_dropped_disconnected + s.frames_dropped_queue_full),
      static_cast<unsigned long long>(s.client_ok), static_cast<unsigned long long>(s.client_lost));
  return 0;
}
