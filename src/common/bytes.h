#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "common/check.h"

namespace raftdst {

// Explicit little-endian encoding. Structs are never memcpy'd onto the wire:
// padding and endianness would leak into the format.

class ByteWriter {
 public:
  explicit ByteWriter(std::vector<uint8_t>& out) : out_(out) {}

  void u8(uint8_t v) { out_.push_back(v); }
  void u16(uint16_t v) { put_le(v, 2); }
  void u32(uint32_t v) { put_le(v, 4); }
  void u64(uint64_t v) { put_le(v, 8); }
  void bytes(std::span<const uint8_t> b) { out_.insert(out_.end(), b.begin(), b.end()); }

  // Overwrite a u32 written earlier, e.g. a frame length known only at the end.
  void patch_u32(size_t offset, uint32_t v) {
    RAFT_ASSERT(offset + 4 <= out_.size(), "patch_u32 past end");
    for (size_t i = 0; i < 4; ++i) out_[offset + i] = static_cast<uint8_t>(v >> (8 * i));
  }

  [[nodiscard]] size_t size() const { return out_.size(); }

 private:
  void put_le(uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i) out_.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }

  std::vector<uint8_t>& out_;
};

// Reads from a borrowed span. Any read past the end sets a sticky error flag
// and returns zero or an empty span, so a decoder can read a whole record and
// check ok() once. Never reads out of bounds.
class ByteReader {
 public:
  explicit ByteReader(std::span<const uint8_t> in) : in_(in) {}

  uint8_t u8() { return static_cast<uint8_t>(get_le(1)); }
  uint16_t u16() { return static_cast<uint16_t>(get_le(2)); }
  uint32_t u32() { return static_cast<uint32_t>(get_le(4)); }
  uint64_t u64() { return get_le(8); }

  // View of the next n bytes, valid as long as the underlying buffer is.
  std::span<const uint8_t> bytes(size_t n) {
    if (!ok_ || n > remaining()) {
      ok_ = false;
      return {};
    }
    auto s = in_.subspan(pos_, n);
    pos_ += n;
    return s;
  }

  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] size_t remaining() const { return in_.size() - pos_; }
  [[nodiscard]] size_t position() const { return pos_; }
  [[nodiscard]] bool at_end() const { return pos_ == in_.size(); }

 private:
  uint64_t get_le(size_t n) {
    if (!ok_ || n > remaining()) {
      ok_ = false;
      return 0;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= static_cast<uint64_t>(in_[pos_ + i]) << (8 * i);
    pos_ += n;
    return v;
  }

  std::span<const uint8_t> in_;
  size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace raftdst
