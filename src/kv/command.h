#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace raftdst::kv {

// Command encoding, little-endian:
//   u8 op   1 = put, 2 = del
//   u16 key_len, key bytes         (1 <= key_len <= kMaxKey)
//   put only: u32 value_len, value (value_len <= kMaxValue)
// Strict: no trailing bytes, so every valid command has one encoding.

inline constexpr size_t kMaxKey = 1024;
inline constexpr size_t kMaxValue = 1u << 20;

enum class Op : uint8_t { kPut = 1, kDel = 2 };

struct Command {
  Op op = Op::kPut;
  std::string_view key;
  std::string_view value;  // empty for del
};

void encode_command(const Command& c, std::vector<uint8_t>& out);

// On success `out` views into `bytes`.
[[nodiscard]] bool decode_command(std::span<const uint8_t> bytes, Command& out);

}  // namespace raftdst::kv
