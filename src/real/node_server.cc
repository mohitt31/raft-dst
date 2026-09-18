#include "real/node_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "kv/command.h"
#include "raft/codec.h"

namespace raftdst::real {

namespace {

std::string errno_message(const char* what) {
  return std::string{what} + ": " + std::strerror(errno);
}

bool set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  return flags != -1 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

void set_nodelay(int fd) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

}  // namespace

// The INetwork the Raft host writes to; it just hands frames to the server.
class NodeServer::Net final : public INetwork {
 public:
  explicit Net(NodeServer& s) : s_(s) {}
  void send(NodeId to, std::span<const uint8_t> frame) override { s_.send_to_peer(to, frame); }

 private:
  NodeServer& s_;
};

std::unique_ptr<NodeServer> NodeServer::start(const ServerOptions& options, std::string* error) {
  auto s = std::unique_ptr<NodeServer>(new NodeServer());
  s->options_ = options;

  s->wal_ = FileWal::open(options.wal_path, error);
  if (!s->wal_) return nullptr;

  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    if (error) *error = errno_message("socket");
    return nullptr;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(options.peers[options.self.get()].port);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(fd, 16) != 0 ||
      !set_nonblocking(fd)) {
    if (error) *error = errno_message("bind/listen");
    ::close(fd);
    return nullptr;
  }
  socklen_t len = sizeof addr;
  getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  s->listen_fd_ = fd;
  s->port_ = ntohs(addr.sin_port);

  s->net_ = std::make_unique<Net>(*s);
  auto kv = std::make_unique<kv::KvStore>();
  s->kv_ = kv.get();
  s->ctx_ = std::make_unique<NodeContext>(
      NodeContext{options.self, options.cluster_size, options.seed, s->clock_, *s->net_, *s->wal_});
  s->host_ = std::make_unique<node::NodeHost>(*s->ctx_, options.raft, std::move(kv), s.get());
  for (auto& t : s->reconnect_at_) t = Time::zero();
  return s;
}

NodeServer::~NodeServer() {
  if (listen_fd_ >= 0) ::close(listen_fd_);
  for (Conn& c : outbound_) {
    if (c.fd >= 0) ::close(c.fd);
  }
  for (Conn& c : inbound_) {
    if (c.fd >= 0) ::close(c.fd);
  }
}

// ------------------------------------------------------------------ sending

void NodeServer::send_to_peer(NodeId to, std::span<const uint8_t> frame) {
  Conn& c = outbound_[to.get()];
  if (c.fd < 0 || c.connecting) {
    ++stats_.frames_dropped_disconnected;
    return;
  }
  if (c.out.size() + frame.size() > options_.max_send_queue_bytes) {
    ++stats_.frames_dropped_queue_full;
    return;
  }
  c.out.insert(c.out.end(), frame.begin(), frame.end());
  ++stats_.frames_sent;
}

void NodeServer::queue(Conn& c, const std::vector<uint8_t>& frame) {
  if (c.fd < 0) return;
  if (c.out.size() + frame.size() > options_.max_send_queue_bytes) return;
  c.out.insert(c.out.end(), frame.begin(), frame.end());
}

void NodeServer::dial(NodeId peer, Time now) {
  Conn& c = outbound_[peer.get()];
  if (c.fd >= 0 || now < reconnect_at_[peer.get()]) return;

  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return;
  set_nonblocking(fd);
  set_nodelay(fd);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(options_.peers[peer.get()].port);
  inet_pton(AF_INET, options_.peers[peer.get()].host.c_str(), &addr.sin_addr);

  const int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
  if (rc != 0 && errno != EINPROGRESS) {
    ::close(fd);
    reconnect_at_[peer.get()] = now + options_.reconnect_delay;
    return;
  }
  c.fd = fd;
  c.id = next_conn_id_++;
  c.kind = Conn::Kind::kPeer;
  c.peer = peer;
  c.connecting = rc != 0;
  c.in.clear();
  c.out.clear();
  if (!c.connecting) {
    std::vector<uint8_t> hello;
    encode_hello(kHelloPeer, options_.self, hello);
    queue(c, hello);
    ++stats_.connects;
  }
}

void NodeServer::on_writable(Conn& c) {
  if (c.connecting) {
    int err = 0;
    socklen_t len = sizeof err;
    getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {
      close_conn(c);
      return;
    }
    c.connecting = false;
    ++stats_.connects;
    std::vector<uint8_t> hello;
    encode_hello(kHelloPeer, options_.self, hello);
    // The hello goes in front of anything queued while connecting.
    c.out.insert(c.out.begin(), hello.begin(), hello.end());
  }
  while (!c.out.empty()) {
    const ssize_t n = ::write(c.fd, c.out.data(), c.out.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      close_conn(c);
      return;
    }
    c.out.erase(c.out.begin(), c.out.begin() + n);
  }
}

// ----------------------------------------------------------------- receiving

void NodeServer::accept_new() {
  for (;;) {
    const int fd = accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) return;
    set_nonblocking(fd);
    set_nodelay(fd);
    Conn c;
    c.fd = fd;
    c.id = next_conn_id_++;
    inbound_.push_back(std::move(c));
    ++stats_.accepts;
  }
}

void NodeServer::on_readable(Conn& c) {
  uint8_t buf[64 * 1024];
  for (;;) {
    const ssize_t n = ::read(c.fd, buf, sizeof buf);
    if (n == 0) {
      close_conn(c);
      return;
    }
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      close_conn(c);
      return;
    }
    c.in.insert(c.in.end(), buf, buf + n);
    if (static_cast<size_t>(n) < sizeof buf) break;
  }

  for (;;) {
    size_t consumed = 0;
    const auto body = next_frame(c.in, consumed);
    if (consumed == 0) {
      // A length prefix beyond the frame limit means a broken peer.
      if (raft::probe_frame(c.in).error != raft::DecodeError::kOk) close_conn(c);
      return;
    }
    // Copy out: handling may append to c.in indirectly through reentrancy.
    const std::vector<uint8_t> frame{body.begin(), body.end()};
    c.in.erase(c.in.begin(), c.in.begin() + static_cast<long>(consumed));
    const uint64_t id = c.id;
    handle_frame(c, frame);
    if (find_conn(id) == nullptr) return;  // closed while handling
  }
}

void NodeServer::handle_frame(Conn& c, std::span<const uint8_t> body) {
  if (c.kind == Conn::Kind::kUnknown) {
    uint8_t kind = 0;
    NodeId node;
    if (!decode_hello(body, kind, node) ||
        (kind == kHelloPeer && node.get() >= options_.cluster_size)) {
      ++stats_.bad_frames;
      close_conn(c);
      return;
    }
    c.kind = kind == kHelloPeer ? Conn::Kind::kPeer : Conn::Kind::kClient;
    c.peer = node;
    return;
  }
  if (c.kind == Conn::Kind::kClient) {
    handle_client_request(c, body);
    return;
  }
  // A peer frame: hand the whole frame, with its length prefix, to the host.
  ++stats_.frames_received;
  std::vector<uint8_t> whole;
  whole.reserve(body.size() + 4);
  for (int i = 0; i < 4; ++i) whole.push_back(static_cast<uint8_t>(body.size() >> (8 * i)));
  whole.insert(whole.end(), body.begin(), body.end());
  host_->on_frame(c.peer, whole);
}

void NodeServer::handle_client_request(Conn& c, std::span<const uint8_t> body) {
  ++stats_.client_requests;
  ClientRequest req;
  ClientResponse resp;
  std::vector<uint8_t> out;
  if (!decode_request(body, req)) {
    resp.status = ClientStatus::kInvalid;
    encode_response(resp, out);
    queue(c, out);
    return;
  }

  if (req.op == ClientOp::kGet) {
    // Served from this node's own state machine: a read can be stale, which
    // is why read-index is on the cut list.
    if (host_->core().role() != raft::Role::kLeader) {
      resp.status = ClientStatus::kNotLeader;
      resp.leader_hint = host_->core().leader();
      ++stats_.client_not_leader;
    } else if (const auto v = kv_->get(req.key)) {
      resp.value = *v;
    } else {
      resp.status = ClientStatus::kNotFound;
    }
    encode_response(resp, out);
    queue(c, out);
    return;
  }

  std::vector<uint8_t> command;
  kv::encode_command({req.op == ClientOp::kPut ? kv::Op::kPut : kv::Op::kDel, req.key, req.value},
                     command);
  const node::ProposeOutcome outcome = host_->propose(command);
  switch (outcome.status) {
    case node::ProposeOutcome::Status::kAccepted:
      pending_.push_back({outcome.index, outcome.term, c.id});
      return;  // answered when the entry applies
    case node::ProposeOutcome::Status::kNotLeader:
      resp.status = ClientStatus::kNotLeader;
      resp.leader_hint = outcome.leader_hint;
      ++stats_.client_not_leader;
      break;
    case node::ProposeOutcome::Status::kInvalidCommand:
      resp.status = ClientStatus::kInvalid;
      break;
  }
  encode_response(resp, out);
  queue(c, out);
}

// ------------------------------------------------------------ apply callback

void NodeServer::on_commit(Time, NodeId, Index index, Term entry_term, Term,
                           std::span<const uint8_t>) {
  if (commit_term_.size() < index.get()) commit_term_.resize(index.get());
  commit_term_[index.get() - 1] = entry_term;
}

void NodeServer::on_applied(Time, NodeId, Index index, uint64_t) {
  if (pending_.empty()) return;
  const Term applied_term =
      index.get() <= commit_term_.size() ? commit_term_[index.get() - 1] : Term{};
  auto done = std::remove_if(pending_.begin(), pending_.end(), [&](const PendingClient& p) {
    if (p.index != index) return false;
    ClientResponse resp;
    if (p.term == applied_term) {
      ++stats_.client_ok;
    } else {
      // Another term's entry took this index: the write was lost in a
      // leader change. Without client sessions there is nothing to retry.
      resp.status = ClientStatus::kLost;
      ++stats_.client_lost;
    }
    std::vector<uint8_t> out;
    encode_response(resp, out);
    if (Conn* c = find_conn(p.conn_id)) queue(*c, out);
    return true;
  });
  pending_.erase(done, pending_.end());
}

// ----------------------------------------------------------------- the loop

NodeServer::Conn* NodeServer::find_conn(uint64_t id) {
  for (Conn& c : inbound_) {
    if (c.id == id && c.fd >= 0) return &c;
  }
  for (Conn& c : outbound_) {
    if (c.id == id && c.fd >= 0) return &c;
  }
  return nullptr;
}

void NodeServer::close_conn(Conn& c) {
  if (c.fd >= 0) ::close(c.fd);
  c.fd = -1;
  c.in.clear();
  c.out.clear();
  c.connecting = false;
  if (c.kind == Conn::Kind::kPeer && c.peer.valid()) {
    reconnect_at_[c.peer.get()] = clock_.now() + options_.reconnect_delay;
  }
  c.kind = Conn::Kind::kUnknown;
}

void NodeServer::poll_once(Duration max_wait) {
  const Time now = clock_.now();
  for (uint8_t i = 0; i < options_.cluster_size; ++i) {
    if (i != options_.self.get()) dial(NodeId{i}, now);
  }

  // Dispatch by connection id, never by pointer: accept_new() pushes onto
  // inbound_, which can reallocate and invalidate any Conn* held across the
  // loop. That bug showed up as a thread blocked in read() on a recycled
  // file descriptor.
  std::vector<pollfd> fds;
  std::vector<uint64_t> owners;  // 0 = the listening socket
  fds.push_back(pollfd{listen_fd_, POLLIN, 0});
  owners.push_back(0);
  auto add = [&](Conn& c) {
    if (c.fd < 0) return;
    short events = POLLIN;
    if (!c.out.empty() || c.connecting) events = static_cast<short>(events | POLLOUT);
    fds.push_back(pollfd{c.fd, events, 0});
    owners.push_back(c.id);
  };
  for (Conn& c : outbound_) add(c);
  for (Conn& c : inbound_) add(c);

  // Sleep until the Raft deadline, but never longer than max_wait.
  const Time deadline = std::min(clock_.deadline(), now + max_wait);
  int timeout_ms = 0;
  if (deadline > now) {
    const int64_t ns = (deadline - now).count_ns();
    timeout_ms = static_cast<int>(std::min<int64_t>(ns / 1'000'000 + 1, 1000));
  }

  const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout_ms);
  if (ready > 0) {
    for (size_t i = 0; i < fds.size(); ++i) {
      if (fds[i].revents == 0) continue;
      if (owners[i] == 0) {
        accept_new();
        continue;
      }
      Conn* c = find_conn(owners[i]);  // may have been closed while handling
      if (c == nullptr) continue;
      if (fds[i].revents & POLLOUT) on_writable(*c);
      c = find_conn(owners[i]);
      if (c != nullptr && (fds[i].revents & (POLLIN | POLLHUP))) on_readable(*c);
      c = find_conn(owners[i]);
      if (c != nullptr && (fds[i].revents & POLLERR)) close_conn(*c);
    }
  }

  // Closed inbound connections are dropped here, never while handling them.
  inbound_.erase(
      std::remove_if(inbound_.begin(), inbound_.end(), [](const Conn& c) { return c.fd < 0; }),
      inbound_.end());

  if (clock_.now() >= clock_.deadline()) host_->on_wake();
}

void NodeServer::run() {
  while (running_.load(std::memory_order_relaxed)) poll_once(Duration::ms(50));
}

}  // namespace raftdst::real
