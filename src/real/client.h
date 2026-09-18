#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "real/client_protocol.h"

namespace raftdst::real {

// Blocking client connection. One request in flight at a time; the load
// generator drives several of these instead of pipelining.
class Client {
 public:
  static std::unique_ptr<Client> connect(const std::string& host, uint16_t port,
                                         std::string* error);
  ~Client();

  // Sends and waits for the reply. `out.value` points into storage this
  // client owns and stays valid until the next call on this client.
  bool request(const ClientRequest& req, ClientResponse& out, Duration timeout, std::string* error);

  [[nodiscard]] int fd() const { return fd_; }

 private:
  explicit Client(int fd) : fd_(fd) {}

  int fd_;
  std::vector<uint8_t> in_;
  std::vector<uint8_t> out_;
  std::string value_;  // owns the bytes out.value points at
};

}  // namespace raftdst::real
