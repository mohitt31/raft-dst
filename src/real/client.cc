#include "real/client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "real/real_clock.h"

namespace raftdst::real {

namespace {

std::string errno_message(const char* what) {
  return std::string{what} + ": " + std::strerror(errno);
}

}  // namespace

std::unique_ptr<Client> Client::connect(const std::string& host, uint16_t port,
                                        std::string* error) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    if (error) *error = errno_message("socket");
    return nullptr;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    if (error) *error = "bad host: " + host;
    ::close(fd);
    return nullptr;
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    if (error) *error = errno_message("connect");
    ::close(fd);
    return nullptr;
  }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

  auto c = std::unique_ptr<Client>(new Client(fd));
  std::vector<uint8_t> hello;
  encode_hello(kHelloClient, NodeId::none(), hello);
  if (::write(fd, hello.data(), hello.size()) != static_cast<ssize_t>(hello.size())) {
    if (error) *error = errno_message("write hello");
    return nullptr;
  }
  return c;
}

Client::~Client() {
  if (fd_ >= 0) ::close(fd_);
}

bool Client::request(const ClientRequest& req, ClientResponse& out, Duration timeout,
                     std::string* error) {
  out_.clear();
  encode_request(req, out_);
  size_t written = 0;
  while (written < out_.size()) {
    const ssize_t n = ::write(fd_, out_.data() + written, out_.size() - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (error) *error = errno_message("write");
      return false;
    }
    written += static_cast<size_t>(n);
  }

  RealClock clock;
  const Time deadline = clock.now() + timeout;
  in_.clear();
  for (;;) {
    size_t consumed = 0;
    const auto body = next_frame(in_, consumed);
    if (consumed > 0) {
      const bool ok = decode_response(body, out);
      // Copy the value out before touching in_: the decoded view points into
      // it, and erasing would leave that view past the end of the vector.
      if (ok) {
        value_.assign(out.value);
        out.value = value_;
      }
      in_.erase(in_.begin(), in_.begin() + static_cast<long>(consumed));
      if (!ok && error) *error = "malformed response";
      return ok;
    }
    const Time now = clock.now();
    if (now >= deadline) {
      if (error) *error = "timed out";
      return false;
    }
    pollfd p{fd_, POLLIN, 0};
    const int ms = static_cast<int>((deadline - now).count_ns() / 1'000'000) + 1;
    const int ready = ::poll(&p, 1, ms);
    if (ready < 0) {
      if (errno == EINTR) continue;
      if (error) *error = errno_message("poll");
      return false;
    }
    if (ready == 0) continue;
    uint8_t buf[4096];
    const ssize_t n = ::read(fd_, buf, sizeof buf);
    if (n == 0) {
      if (error) *error = "connection closed";
      return false;
    }
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      if (error) *error = errno_message("read");
      return false;
    }
    in_.insert(in_.end(), buf, buf + n);
  }
}

}  // namespace raftdst::real
