#include "real/client_protocol.h"

#include "common/bytes.h"
#include "raft/codec.h"

namespace raftdst::real {

namespace {

std::span<const uint8_t> as_bytes(std::string_view s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

std::string_view as_view(std::span<const uint8_t> b) {
  return {reinterpret_cast<const char*>(b.data()), b.size()};
}

void frame(std::vector<uint8_t>& out, size_t start) {
  ByteWriter w{out};
  w.patch_u32(start, static_cast<uint32_t>(out.size() - start - 4));
}

}  // namespace

void encode_hello(uint8_t kind, NodeId node, std::vector<uint8_t>& out) {
  const size_t start = out.size();
  ByteWriter w{out};
  w.u32(0);
  w.u8(kind);
  w.u8(node == NodeId::none() ? 0xFF : node.get());
  frame(out, start);
}

bool decode_hello(std::span<const uint8_t> body, uint8_t& kind, NodeId& node) {
  ByteReader r{body};
  kind = r.u8();
  const uint8_t id = r.u8();
  node = id == 0xFF ? NodeId::none() : NodeId{id};
  return r.ok() && r.at_end() && (kind == kHelloPeer || kind == kHelloClient);
}

void encode_request(const ClientRequest& req, std::vector<uint8_t>& out) {
  const size_t start = out.size();
  ByteWriter w{out};
  w.u32(0);
  w.u8(static_cast<uint8_t>(req.op));
  w.u16(static_cast<uint16_t>(req.key.size()));
  w.bytes(as_bytes(req.key));
  if (req.op == ClientOp::kPut) {
    w.u32(static_cast<uint32_t>(req.value.size()));
    w.bytes(as_bytes(req.value));
  }
  frame(out, start);
}

bool decode_request(std::span<const uint8_t> body, ClientRequest& out) {
  ByteReader r{body};
  const uint8_t op = r.u8();
  if (op < 1 || op > 3) return false;
  out.op = static_cast<ClientOp>(op);
  const uint16_t key_len = r.u16();
  if (key_len == 0) return false;
  out.key = as_view(r.bytes(key_len));
  out.value = {};
  if (out.op == ClientOp::kPut) out.value = as_view(r.bytes(r.u32()));
  return r.ok() && r.at_end();
}

void encode_response(const ClientResponse& resp, std::vector<uint8_t>& out) {
  const size_t start = out.size();
  ByteWriter w{out};
  w.u32(0);
  w.u8(static_cast<uint8_t>(resp.status));
  w.u8(resp.leader_hint == NodeId::none() ? 0xFF : resp.leader_hint.get());
  if (resp.status == ClientStatus::kOk && !resp.value.empty()) {
    w.u32(static_cast<uint32_t>(resp.value.size()));
    w.bytes(as_bytes(resp.value));
  }
  frame(out, start);
}

bool decode_response(std::span<const uint8_t> body, ClientResponse& out) {
  ByteReader r{body};
  const uint8_t status = r.u8();
  if (status > 4) return false;
  out.status = static_cast<ClientStatus>(status);
  const uint8_t hint = r.u8();
  out.leader_hint = hint == 0xFF ? NodeId::none() : NodeId{hint};
  out.value = {};
  if (!r.at_end()) out.value = as_view(r.bytes(r.u32()));
  return r.ok() && r.at_end();
}

std::span<const uint8_t> next_frame(std::span<const uint8_t> buf, size_t& consumed) {
  const raft::FrameProbe p = raft::probe_frame(buf);
  if (p.error != raft::DecodeError::kOk || p.size == 0) {
    consumed = 0;
    return {};
  }
  consumed = p.size;
  return buf.subspan(4, p.size - 4);
}

}  // namespace raftdst::real
