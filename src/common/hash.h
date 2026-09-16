#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace raftdst {

// Streaming 64-bit FNV-1a. Used to fingerprint traces and state machine
// contents so runs can be compared by one number. Not a defence against
// adversarial collisions, which do not exist here. Chosen for a fixed public
// definition with published test vectors; if it shows up in a profile it
// gets replaced with a word-at-a-time hash.
class Fnv1a64 {
 public:
  static constexpr uint64_t kOffset = 0xcbf29ce484222325ULL;
  static constexpr uint64_t kPrime = 0x100000001b3ULL;

  constexpr void add_bytes(std::span<const uint8_t> bytes) {
    for (uint8_t b : bytes) {
      h_ ^= b;
      h_ *= kPrime;
    }
  }

  // Integers are absorbed as little-endian bytes so the value is the same on
  // every platform.
  constexpr void add_u64(uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h_ ^= static_cast<uint8_t>(v >> (8 * i));
      h_ *= kPrime;
    }
  }

  constexpr void add_u8(uint8_t v) {
    h_ ^= v;
    h_ *= kPrime;
  }

  [[nodiscard]] constexpr uint64_t value() const { return h_; }

 private:
  uint64_t h_ = kOffset;
};

}  // namespace raftdst
