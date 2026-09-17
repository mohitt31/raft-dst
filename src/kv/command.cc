#include "kv/command.h"

#include "common/bytes.h"
#include "common/check.h"

namespace raftdst::kv {

namespace {

std::span<const uint8_t> as_bytes(std::string_view s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

std::string_view as_view(std::span<const uint8_t> b) {
  return {reinterpret_cast<const char*>(b.data()), b.size()};
}

}  // namespace

void encode_command(const Command& c, std::vector<uint8_t>& out) {
  RAFT_ASSERT(!c.key.empty() && c.key.size() <= kMaxKey, "key length out of range");
  RAFT_ASSERT(c.value.size() <= kMaxValue, "value too large");
  RAFT_ASSERT(c.op == Op::kPut || c.value.empty(), "del carries no value");
  ByteWriter w{out};
  w.u8(static_cast<uint8_t>(c.op));
  w.u16(static_cast<uint16_t>(c.key.size()));
  w.bytes(as_bytes(c.key));
  if (c.op == Op::kPut) {
    w.u32(static_cast<uint32_t>(c.value.size()));
    w.bytes(as_bytes(c.value));
  }
}

bool decode_command(std::span<const uint8_t> bytes, Command& out) {
  ByteReader r{bytes};
  const uint8_t op = r.u8();
  if (op != static_cast<uint8_t>(Op::kPut) && op != static_cast<uint8_t>(Op::kDel)) return false;
  const uint16_t key_len = r.u16();
  if (key_len == 0 || key_len > kMaxKey) return false;
  out.op = static_cast<Op>(op);
  out.key = as_view(r.bytes(key_len));
  out.value = {};
  if (out.op == Op::kPut) {
    const uint32_t value_len = r.u32();
    if (value_len > kMaxValue) return false;
    out.value = as_view(r.bytes(value_len));
  }
  return r.ok() && r.at_end();
}

}  // namespace raftdst::kv
