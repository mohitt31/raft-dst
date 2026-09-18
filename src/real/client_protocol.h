#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace raftdst::real {

// Client wire protocol, length-prefixed like the Raft one:
//   u32 body_len, then the body below.
//
// request:  u8 op (1 put, 2 del, 3 get), u16 key_len, key, [u32 value_len, value]
// response: u8 status, u8 leader_hint, [u32 value_len, value]
//
// A connection's first frame is a hello: u8 kind (1 peer, 2 client), u8 node.

inline constexpr uint8_t kHelloPeer = 1;
inline constexpr uint8_t kHelloClient = 2;

enum class ClientOp : uint8_t { kPut = 1, kDel = 2, kGet = 3 };

enum class ClientStatus : uint8_t {
  kOk = 0,
  kNotLeader = 1,  // leader_hint may name one
  kInvalid = 2,    // malformed request
  kLost = 3,       // the entry's index was taken by another term
  kNotFound = 4,   // get with no such key
};

struct ClientRequest {
  ClientOp op = ClientOp::kGet;
  std::string_view key;
  std::string_view value;
};

struct ClientResponse {
  ClientStatus status = ClientStatus::kOk;
  NodeId leader_hint;
  std::string_view value;
};

void encode_hello(uint8_t kind, NodeId node, std::vector<uint8_t>& out);
bool decode_hello(std::span<const uint8_t> body, uint8_t& kind, NodeId& node);

void encode_request(const ClientRequest& r, std::vector<uint8_t>& out);
bool decode_request(std::span<const uint8_t> body, ClientRequest& out);

void encode_response(const ClientResponse& r, std::vector<uint8_t>& out);
bool decode_response(std::span<const uint8_t> body, ClientResponse& out);

// Frame body of the first complete frame in `buf`, or empty with size 0 when
// more bytes are needed. `consumed` is how many bytes the frame occupied.
std::span<const uint8_t> next_frame(std::span<const uint8_t> buf, size_t& consumed);

}  // namespace raftdst::real
