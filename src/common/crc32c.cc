#include "common/crc32c.h"

#include <array>

namespace raftdst {
namespace {

constexpr uint32_t kPoly = 0x82f63b78;  // reflected 0x1EDC6F41

constexpr std::array<uint32_t, 256> make_table() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ kPoly : c >> 1;
    t[i] = c;
  }
  return t;
}

constexpr std::array<uint32_t, 256> kTable = make_table();

}  // namespace

uint32_t crc32c_extend(uint32_t crc, std::span<const uint8_t> data) {
  uint32_t c = ~crc;
  for (uint8_t b : data) c = kTable[(c ^ b) & 0xff] ^ (c >> 8);
  return ~c;
}

}  // namespace raftdst
