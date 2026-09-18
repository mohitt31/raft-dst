// Command-line client.
//
//   raft_client --port 7000 put key value
//   raft_client --port 7000 get key
//   raft_client --port 7000 del key
//
// Follows NotLeader hints when --peers lists the cluster.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "real/client.h"

int main(int argc, char** argv) {
  using namespace raftdst;
  std::string host = "127.0.0.1";
  uint16_t port = 0;
  std::vector<uint16_t> ports;
  std::vector<std::string> words;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strcmp(a, "--host") == 0 && i + 1 < argc) {
      host = argv[++i];
    } else if (std::strcmp(a, "--port") == 0 && i + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
      ports.push_back(port);
    } else {
      words.emplace_back(a);
    }
  }
  if (ports.empty() || words.empty()) {
    std::fprintf(stderr, "usage: %s --port P [--port P2 ...] put|get|del key [value]\n", argv[0]);
    return 2;
  }

  real::ClientRequest req;
  if (words[0] == "put" && words.size() == 3) {
    req = {real::ClientOp::kPut, words[1], words[2]};
  } else if (words[0] == "get" && words.size() == 2) {
    req = {real::ClientOp::kGet, words[1], ""};
  } else if (words[0] == "del" && words.size() == 2) {
    req = {real::ClientOp::kDel, words[1], ""};
  } else {
    std::fprintf(stderr, "bad command\n");
    return 2;
  }

  for (size_t attempt = 0; attempt < ports.size() * 3 + 3; ++attempt) {
    const uint16_t target = ports[attempt % ports.size()];
    std::string error;
    auto client = real::Client::connect(host, target, &error);
    if (!client) {
      std::fprintf(stderr, "connect %u: %s\n", target, error.c_str());
      continue;
    }
    real::ClientResponse resp;
    if (!client->request(req, resp, Duration::sec(5), &error)) {
      std::fprintf(stderr, "request to %u: %s\n", target, error.c_str());
      continue;
    }
    switch (resp.status) {
      case real::ClientStatus::kOk:
        if (req.op == real::ClientOp::kGet) {
          std::printf("%.*s\n", static_cast<int>(resp.value.size()), resp.value.data());
        } else {
          std::printf("ok\n");
        }
        return 0;
      case real::ClientStatus::kNotFound:
        std::printf("(not found)\n");
        return 1;
      case real::ClientStatus::kNotLeader:
        continue;  // try the next port
      case real::ClientStatus::kLost:
        std::fprintf(stderr, "write lost in a leader change; retry it\n");
        return 1;
      case real::ClientStatus::kInvalid:
        std::fprintf(stderr, "server rejected the request\n");
        return 2;
    }
  }
  std::fprintf(stderr, "no leader answered\n");
  return 1;
}
