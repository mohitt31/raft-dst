#include "raft/codec.h"

#include <array>
#include <limits>

#include "common/bytes.h"

namespace raftdst::raft {

std::string_view decode_error_name(DecodeError e) {
  static constexpr std::array<std::string_view, 12> kNames = {
      "ok",       "truncated", "trailing-bytes", "too-large",        "bad-version", "bad-type",
      "bad-node", "bad-bool",  "bad-term",       "bad-log-position", "bad-entries", "bad-response",
  };
  const auto i = static_cast<size_t>(e);
  return i < kNames.size() ? kNames[i] : "?";
}

void encode(const Message& m, std::vector<uint8_t>& out) {
  RAFT_DCHECK(validate(m, kMaxNodes) == DecodeError::kOk, "encoding an invalid message");
  const size_t start = out.size();
  ByteWriter w{out};
  w.u32(0);  // patched below
  w.u8(kWireVersion);
  w.u8(static_cast<uint8_t>(m.type));
  w.u8(m.from.get());
  w.u8(m.to.get());
  w.u64(m.term.get());

  switch (m.type) {
    case MsgType::kRequestVote:
      w.u64(m.last_log_index.get());
      w.u64(m.last_log_term.get());
      break;
    case MsgType::kRequestVoteResp:
      w.u8(m.ok ? 1 : 0);
      break;
    case MsgType::kAppendEntries: {
      w.u64(m.prev_index.get());
      w.u64(m.prev_term.get());
      w.u64(m.commit.get());
      const uint64_t n = m.entries.size();
      RAFT_ASSERT(n <= std::numeric_limits<uint32_t>::max(), "too many entries in one message");
      w.u32(static_cast<uint32_t>(n));
      for (uint64_t i = 0; i < n; ++i) {
        w.u64(m.entries.term(i).get());
        w.u32(m.entries.payload_size(i));
      }
      w.bytes(m.entries.payload_blob());
      break;
    }
    case MsgType::kAppendEntriesResp:
      w.u8(m.ok ? 1 : 0);
      w.u64(m.index.get());
      w.u64(m.hint_index.get());
      w.u64(m.hint_term.get());
      break;
  }

  const size_t body = out.size() - start - 4;
  RAFT_ASSERT(body + 4 <= kMaxFrameBytes, "encoded frame exceeds kMaxFrameBytes");
  w.patch_u32(start, static_cast<uint32_t>(body));
}

namespace {

bool read_bool(ByteReader& r, bool& out) {
  const uint8_t b = r.u8();
  out = b == 1;
  return b <= 1;
}

}  // namespace

DecodeError decode(std::span<const uint8_t> frame, uint8_t cluster_size, Message& out) {
  out = Message{};
  ByteReader r{frame};
  const uint64_t body = r.u32();
  if (!r.ok()) return DecodeError::kTruncated;
  if (body + 4 > kMaxFrameBytes) return DecodeError::kTooLarge;
  if (body + 4 > frame.size()) return DecodeError::kTruncated;
  if (body + 4 < frame.size()) return DecodeError::kTrailingBytes;

  const uint8_t version = r.u8();
  const uint8_t type = r.u8();
  out.from = NodeId{r.u8()};
  out.to = NodeId{r.u8()};
  out.term = Term{r.u64()};
  if (!r.ok()) return DecodeError::kTruncated;
  if (version != kWireVersion) return DecodeError::kBadVersion;

  switch (type) {
    case static_cast<uint8_t>(MsgType::kRequestVote):
      out.type = MsgType::kRequestVote;
      out.last_log_index = Index{r.u64()};
      out.last_log_term = Term{r.u64()};
      break;
    case static_cast<uint8_t>(MsgType::kRequestVoteResp):
      out.type = MsgType::kRequestVoteResp;
      if (!read_bool(r, out.ok) && r.ok()) return DecodeError::kBadBool;
      break;
    case static_cast<uint8_t>(MsgType::kAppendEntries): {
      out.type = MsgType::kAppendEntries;
      out.prev_index = Index{r.u64()};
      out.prev_term = Term{r.u64()};
      out.commit = Index{r.u64()};
      const uint64_t count = r.u32();
      if (!r.ok()) return DecodeError::kTruncated;
      // Bound the header size by what is left before reading any of it.
      if (count > r.remaining() / EntriesView::kWireHeader) return DecodeError::kTruncated;
      const auto headers = r.bytes(count * EntriesView::kWireHeader);
      uint64_t blob_size = 0;
      for (uint64_t i = 0; i < count; ++i) {
        const size_t at = i * EntriesView::kWireHeader + 8;
        blob_size += uint64_t{headers[at]} | uint64_t{headers[at + 1]} << 8 |
                     uint64_t{headers[at + 2]} << 16 | uint64_t{headers[at + 3]} << 24;
      }
      if (blob_size > r.remaining()) return DecodeError::kTruncated;
      const auto blob = r.bytes(blob_size);
      out.entries = EntriesView::from_wire(headers, blob, count);
      break;
    }
    case static_cast<uint8_t>(MsgType::kAppendEntriesResp):
      out.type = MsgType::kAppendEntriesResp;
      if (!read_bool(r, out.ok) && r.ok()) return DecodeError::kBadBool;
      out.index = Index{r.u64()};
      out.hint_index = Index{r.u64()};
      out.hint_term = Term{r.u64()};
      break;
    default:
      return DecodeError::kBadType;
  }

  if (!r.ok()) return DecodeError::kTruncated;
  if (!r.at_end()) return DecodeError::kTrailingBytes;
  return validate(out, cluster_size);
}

DecodeError validate(const Message& m, uint8_t cluster_size) {
  if (m.from.get() >= cluster_size || m.to.get() >= cluster_size || m.from == m.to) {
    return DecodeError::kBadNode;
  }
  if (m.term == Term{0}) return DecodeError::kBadTerm;

  // A log position (index, term) names the sentinel only as (0, 0), and any
  // real entry has a term of at least 1 and at most the sender's term.
  auto position_ok = [&](Index i, Term t) {
    return (i == Index{0}) == (t == Term{0}) && t <= m.term;
  };

  switch (m.type) {
    case MsgType::kRequestVote:
      if (!position_ok(m.last_log_index, m.last_log_term)) return DecodeError::kBadLogPosition;
      return DecodeError::kOk;

    case MsgType::kRequestVoteResp:
      return DecodeError::kOk;

    case MsgType::kAppendEntries: {
      if (!position_ok(m.prev_index, m.prev_term)) return DecodeError::kBadLogPosition;
      const uint64_t n = m.entries.size();
      if (m.prev_index.get() > std::numeric_limits<uint64_t>::max() - n) {
        return DecodeError::kBadEntries;
      }
      Term prev = m.prev_term;
      for (uint64_t i = 0; i < n; ++i) {
        const Term t = m.entries.term(i);
        if (t == Term{0} || t < prev || t > m.term) return DecodeError::kBadEntries;
        prev = t;
      }
      return DecodeError::kOk;
    }

    case MsgType::kAppendEntriesResp:
      if (m.ok) {
        if (m.hint_index != Index{0} || m.hint_term != Term{0}) return DecodeError::kBadResponse;
      } else {
        if (m.index == Index{0} || m.hint_index == Index{0}) return DecodeError::kBadResponse;
        if (m.hint_term > m.term) return DecodeError::kBadResponse;
      }
      return DecodeError::kOk;
  }
  return DecodeError::kBadType;
}

FrameProbe probe_frame(std::span<const uint8_t> buf) {
  if (buf.size() < 4) return {};
  const uint64_t body =
      uint64_t{buf[0]} | uint64_t{buf[1]} << 8 | uint64_t{buf[2]} << 16 | uint64_t{buf[3]} << 24;
  if (body + 4 > kMaxFrameBytes) return {0, DecodeError::kTooLarge};
  if (buf.size() < body + 4) return {};
  return {static_cast<size_t>(body + 4), DecodeError::kOk};
}

}  // namespace raftdst::raft
