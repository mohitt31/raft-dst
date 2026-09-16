#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "common/check.h"
#include "common/types.h"

namespace raftdst {

class EntryLog;

// Non-owning view of entries [first, first + size) of an EntryLog. Holds the
// log pointer and indices, not spans, so appends to the log do not invalidate
// it. Truncating the log does; debug builds check that with a generation
// counter.
class LogSlice {
 public:
  LogSlice() = default;

  [[nodiscard]] Index first() const { return first_; }
  [[nodiscard]] uint64_t size() const { return count_; }
  [[nodiscard]] bool empty() const { return count_ == 0; }

  // i is the position inside the slice, 0-based.
  [[nodiscard]] Term term(uint64_t i) const;
  [[nodiscard]] std::span<const uint8_t> payload(uint64_t i) const;
  [[nodiscard]] size_t payload_bytes() const;
  // Every payload in the slice as one contiguous span.
  [[nodiscard]] std::span<const uint8_t> payload_blob() const;

 private:
  friend class EntryLog;
  LogSlice(const EntryLog* log, Index first, uint64_t count, uint64_t gen)
      : log_(log), first_(first), count_(count), gen_(gen) {}
  void check_valid() const;

  const EntryLog* log_ = nullptr;
  Index first_{1};
  uint64_t count_ = 0;
  uint64_t gen_ = 0;
};

// Append-mostly Raft log stored as a struct of arrays:
//
//   terms_[i]  term of entry i          (terms_[0] = 0 is the sentinel)
//   ends_[i]   end offset of payload i  (ends_[0] = 0)
//   arena_     every payload, back to back
//
// Entry i's payload is arena_[ends_[i-1], ends_[i]). Index equals position
// because there is no compaction. Invariants:
//   terms_.size() == ends_.size() >= 1
//   terms_ is non-decreasing (a decrease means a Raft bug; asserted)
//   ends_ is non-decreasing and ends_.back() == arena_.size()
class EntryLog {
 public:
  EntryLog() : terms_{Term{0}}, ends_{0} {}

  [[nodiscard]] Index last_index() const { return Index{terms_.size() - 1}; }
  [[nodiscard]] Term last_term() const { return terms_.back(); }

  // True for 0 (the sentinel) through last_index().
  [[nodiscard]] bool has(Index i) const { return i.get() < terms_.size(); }

  [[nodiscard]] Term term_at(Index i) const {
    RAFT_DCHECK(has(i), "term_at past end of log");
    return terms_[i.get()];
  }

  [[nodiscard]] std::span<const uint8_t> payload_at(Index i) const {
    RAFT_DCHECK(i.get() >= 1 && has(i), "payload_at out of range");
    const uint32_t b = ends_[i.get() - 1];
    return {arena_.data() + b, ends_[i.get()] - b};
  }

  void append(Term term, std::span<const uint8_t> payload);

  // Appends every entry of `s`. Requires s.first() == last_index() + 1.
  void append(const LogSlice& s);

  // Keeps entries [1, keep] and drops the rest. keep may equal last_index().
  void truncate_after(Index keep);

  // Up to max_count entries starting at `first`, shortened so the payload
  // total stays within max_bytes, but never shorter than one entry. Empty if
  // first > last_index().
  [[nodiscard]] LogSlice slice(Index first, uint64_t max_count, size_t max_bytes) const;

  [[nodiscard]] LogSlice slice_all_from(Index first) const {
    return slice(first, UINT64_MAX, SIZE_MAX);
  }

  [[nodiscard]] size_t payload_bytes() const { return arena_.size(); }
  [[nodiscard]] uint64_t generation() const { return generation_; }

  // Same entries, same terms, same payloads. Ignores generation.
  friend bool operator==(const EntryLog& a, const EntryLog& b) {
    return a.terms_ == b.terms_ && a.ends_ == b.ends_ && a.arena_ == b.arena_;
  }

  // O(n) structural check for tests.
  [[nodiscard]] bool invariants_hold() const;

 private:
  friend class LogSlice;

  std::vector<Term> terms_;
  std::vector<uint32_t> ends_;
  std::vector<uint8_t> arena_;
  uint64_t generation_ = 0;
};

inline void LogSlice::check_valid() const {
  RAFT_DCHECK(log_ != nullptr, "LogSlice without a log");
  RAFT_DCHECK(gen_ == log_->generation_, "LogSlice used after its log was truncated");
}

inline Term LogSlice::term(uint64_t i) const {
  check_valid();
  RAFT_DCHECK(i < count_, "LogSlice::term out of range");
  return log_->term_at(first_ + i);
}

inline std::span<const uint8_t> LogSlice::payload(uint64_t i) const {
  check_valid();
  RAFT_DCHECK(i < count_, "LogSlice::payload out of range");
  return log_->payload_at(first_ + i);
}

inline size_t LogSlice::payload_bytes() const {
  if (count_ == 0) return 0;
  check_valid();
  return log_->ends_[first_.get() + count_ - 1] - log_->ends_[first_.get() - 1];
}

inline std::span<const uint8_t> LogSlice::payload_blob() const {
  if (count_ == 0) return {};
  check_valid();
  const uint32_t b = log_->ends_[first_.get() - 1];
  const uint32_t e = log_->ends_[first_.get() + count_ - 1];
  return {log_->arena_.data() + b, e - b};
}

}  // namespace raftdst
