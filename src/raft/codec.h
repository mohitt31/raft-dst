#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "raft/message.h"

namespace raftdst::raft {

// Frame layout, all integers little-endian:
//
//   u32 body_length        bytes after this field
//   u8  version            kWireVersion
//   u8  type               MsgType
//   u8  from, u8 to
//   u64 term
//   ... type-specific fields, in the order listed in message.h
//
// AppendEntries entries are a struct of arrays: u32 count, then count records
// of (u64 term, u32 payload_len), then every payload back to back.
//
// Decoding is strict. Every accepted frame has exactly one encoding, so
// decode followed by encode reproduces the input bytes.

inline constexpr uint8_t kWireVersion = 1;
inline constexpr size_t kFrameHeaderBytes = 16;  // length + version/type/from/to + term
inline constexpr size_t kMaxFrameBytes = 16u << 20;

enum class DecodeError : uint8_t {
  kOk,
  kTruncated,
  kTrailingBytes,
  kTooLarge,
  kBadVersion,
  kBadType,
  kBadNode,
  kBadBool,
  kBadTerm,
  kBadLogPosition,
  kBadEntries,
  kBadResponse,
};

std::string_view decode_error_name(DecodeError e);

// Appends one frame. The message must be valid (checked in DCHECK builds).
void encode(const Message& m, std::vector<uint8_t>& out);

// Decodes exactly one frame. On success `out.entries` points into `frame`.
DecodeError decode(std::span<const uint8_t> frame, uint8_t cluster_size, Message& out);

// Semantic checks shared by the decoder and the encoder's DCHECK.
DecodeError validate(const Message& m, uint8_t cluster_size);

// For byte streams (TCP). Looks at the front of `buf`:
//   size == 0          need more bytes
//   size > 0           a whole frame of `size` bytes is available
//   error == kTooLarge the length prefix exceeds kMaxFrameBytes
struct FrameProbe {
  size_t size = 0;
  DecodeError error = DecodeError::kOk;
};
FrameProbe probe_frame(std::span<const uint8_t> buf);

}  // namespace raftdst::raft
