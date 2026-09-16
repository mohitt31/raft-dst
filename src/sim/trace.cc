#include "sim/trace.h"

#include <array>
#include <charconv>

namespace raftdst::sim {

std::string_view trace_kind_name(TraceKind k) {
  static constexpr std::array<std::string_view, 16> kNames = {
      "start",     "send", "drop-random", "drop-partition", "drop-dead",         "dup",
      "deliver",   "wake", "crash",       "crash-in-sync",  "arm-crash-in-sync", "restart",
      "partition", "heal", "faults-end",  "user",
  };
  const auto i = static_cast<size_t>(k);
  return i < kNames.size() ? kNames[i] : "?";
}

namespace {

void append_u64(std::string& out, uint64_t v) {
  char buf[20];
  auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, end);
}

void append_i64(std::string& out, int64_t v) {
  char buf[21];
  auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, end);
}

}  // namespace

void Trace::record(Time t, TraceKind kind, uint8_t node, uint64_t a, uint64_t b, uint64_t c) {
  ++records_;
  hash_.add_u64(static_cast<uint64_t>(t.ns_since_epoch()));
  hash_.add_u8(static_cast<uint8_t>(kind));
  hash_.add_u8(node);
  hash_.add_u64(a);
  hash_.add_u64(b);
  hash_.add_u64(c);

  if (!text_enabled_) return;
  append_i64(text_, t.ns_since_epoch());
  text_ += " n";
  append_u64(text_, node);
  text_ += ' ';
  text_ += trace_kind_name(kind);
  text_ += ' ';
  append_u64(text_, a);
  text_ += ' ';
  append_u64(text_, b);
  text_ += ' ';
  append_u64(text_, c);
  text_ += '\n';
}

}  // namespace raftdst::sim
