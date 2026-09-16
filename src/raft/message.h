#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "common/check.h"
#include "common/entry_log.h"
#include "common/types.h"

namespace raftdst::raft {

// Read-only view of a run of log entries. Backed either by an EntryLog slice
// (outgoing messages) or by bytes of a decoded frame (incoming messages).
// Either way it is only valid while its backing storage is: the log until
// its next truncation, the frame until the receive callback returns.
class EntriesView {
 public:
  EntriesView() = default;

  static EntriesView from_log(const LogSlice& s) {
    EntriesView v;
    v.slice_ = s;
    v.count_ = s.size();
    return v;
  }

  // `headers` holds count * kWireHeader bytes of (u64 term, u32 len) records.
  // The decoder has already checked that the lengths sum to blob.size().
  static EntriesView from_wire(std::span<const uint8_t> headers, std::span<const uint8_t> blob,
                               uint64_t count) {
    RAFT_DCHECK(headers.size() == count * kWireHeader, "entry header size mismatch");
    EntriesView v;
    v.wire_ = true;
    v.headers_ = headers;
    v.blob_ = blob;
    v.count_ = count;
    return v;
  }

  static constexpr size_t kWireHeader = 12;

  [[nodiscard]] uint64_t size() const { return count_; }
  [[nodiscard]] bool empty() const { return count_ == 0; }

  [[nodiscard]] Term term(uint64_t i) const {
    RAFT_DCHECK(i < count_, "EntriesView::term out of range");
    if (!wire_) return slice_.term(i);
    return Term{load_le(i * kWireHeader, 8)};
  }

  [[nodiscard]] uint32_t payload_size(uint64_t i) const {
    RAFT_DCHECK(i < count_, "EntriesView::payload_size out of range");
    if (!wire_) return static_cast<uint32_t>(slice_.payload(i).size());
    return static_cast<uint32_t>(load_le(i * kWireHeader + 8, 4));
  }

  // All payloads back to back.
  [[nodiscard]] std::span<const uint8_t> payload_blob() const {
    return wire_ ? blob_ : slice_.payload_blob();
  }

  // Calls f(Term, std::span<const uint8_t>) for entries [from, size()).
  template <class F>
  void for_each_from(uint64_t from, F&& f) const {
    if (!wire_) {
      for (uint64_t i = from; i < count_; ++i) f(slice_.term(i), slice_.payload(i));
      return;
    }
    size_t off = 0;
    for (uint64_t i = 0; i < from; ++i) off += payload_size(i);
    for (uint64_t i = from; i < count_; ++i) {
      const uint32_t n = payload_size(i);
      f(term(i), blob_.subspan(off, n));
      off += n;
    }
  }

 private:
  [[nodiscard]] uint64_t load_le(size_t at, size_t n) const {
    uint64_t v = 0;
    for (size_t k = 0; k < n; ++k) v |= static_cast<uint64_t>(headers_[at + k]) << (8 * k);
    return v;
  }

  LogSlice slice_;
  std::span<const uint8_t> headers_;
  std::span<const uint8_t> blob_;
  uint64_t count_ = 0;
  bool wire_ = false;
};

enum class MsgType : uint8_t {
  kRequestVote = 1,
  kRequestVoteResp = 2,
  kAppendEntries = 3,
  kAppendEntriesResp = 4,
};

// One flat struct for all message types, tagged by `type`. Fields a type does
// not use stay zero; the codec neither writes nor accepts them.
//
//   RequestVote        term, last_log_index, last_log_term     (candidate = from)
//   RequestVoteResp    term, ok (vote granted)
//   AppendEntries      term, prev_index, prev_term, commit, entries  (leader = from)
//   AppendEntriesResp  term, ok (success), index, hint_index, hint_term
//
// AppendEntriesResp.index: on success, the last index the follower now knows
// matches the leader (prev_index + entries). On rejection, the prev_index that
// was rejected, so the leader can ignore stale rejections. The hint is the
// follower's conflict position: its last index + 1 with hint_term 0 when its
// log is too short, else the conflicting term and that term's first index.
struct Message {
  MsgType type{};
  NodeId from;
  NodeId to;
  Term term;

  Index last_log_index;
  Term last_log_term;

  Index prev_index;
  Term prev_term;
  Index commit;
  EntriesView entries;

  bool ok = false;
  Index index;
  Index hint_index;
  Term hint_term;
};

}  // namespace raftdst::raft
