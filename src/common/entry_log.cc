#include "common/entry_log.h"

#include <algorithm>
#include <limits>

namespace raftdst {

namespace {

uint32_t checked_end(size_t size) {
  RAFT_ASSERT(size <= std::numeric_limits<uint32_t>::max(),
              "log payload arena exceeds 4 GiB (no compaction; see DESIGN.md)");
  return static_cast<uint32_t>(size);
}

}  // namespace

void EntryLog::append(Term term, std::span<const uint8_t> payload) {
  RAFT_ASSERT(term >= terms_.back(), "appending an entry with a lower term than the last entry");
  arena_.insert(arena_.end(), payload.begin(), payload.end());
  terms_.push_back(term);
  ends_.push_back(checked_end(arena_.size()));
}

void EntryLog::append(const LogSlice& s) {
  if (s.empty()) return;
  s.check_valid();
  RAFT_ASSERT(s.first() == last_index().next(), "slice does not start right after the log end");
  // A non-empty slice of this log cannot start past its end, so s.log_ is
  // never `this` here and the inserts below do not alias.
  RAFT_DCHECK(s.log_ != this, "appending a log to itself");

  const EntryLog& src = *s.log_;
  const uint64_t f = s.first().get();
  const uint64_t l = f + s.size();  // one past the last
  RAFT_ASSERT(src.terms_[f] >= terms_.back(), "appending a slice with a lower term");

  const uint32_t src_base = src.ends_[f - 1];
  const uint32_t src_end = src.ends_[l - 1];
  const size_t my_base = arena_.size();

  terms_.insert(terms_.end(), src.terms_.begin() + static_cast<std::ptrdiff_t>(f),
                src.terms_.begin() + static_cast<std::ptrdiff_t>(l));
  arena_.insert(arena_.end(), src.arena_.begin() + src_base, src.arena_.begin() + src_end);
  checked_end(arena_.size());
  ends_.reserve(ends_.size() + s.size());
  for (uint64_t i = f; i < l; ++i) {
    ends_.push_back(static_cast<uint32_t>(my_base + (src.ends_[i] - src_base)));
  }
}

void EntryLog::truncate_after(Index keep) {
  RAFT_ASSERT(has(keep), "truncate_after past end of log");
  const size_t n = keep.get() + 1;
  if (n == terms_.size()) return;
  terms_.resize(n);
  ends_.resize(n);
  arena_.resize(ends_.back());
  ++generation_;
}

LogSlice EntryLog::slice(Index first, uint64_t max_count, size_t max_bytes) const {
  RAFT_ASSERT(first.get() >= 1, "slice starting at the sentinel");
  if (first > last_index() || max_count == 0) return LogSlice{this, first, 0, generation_};

  const uint64_t f = first.get();
  const uint64_t avail = terms_.size() - f;
  uint64_t count = std::min(max_count, avail);

  // ends_ is sorted, so find the last entry whose end fits the byte budget.
  const uint64_t base = ends_[f - 1];
  if (max_bytes < SIZE_MAX && ends_[f + count - 1] - base > max_bytes) {
    const uint64_t limit = base + max_bytes;
    auto begin = ends_.begin() + static_cast<std::ptrdiff_t>(f);
    auto end = begin + static_cast<std::ptrdiff_t>(count);
    auto it = std::upper_bound(begin, end, limit);
    count = std::max<uint64_t>(1, static_cast<uint64_t>(it - begin));
  }
  return LogSlice{this, first, count, generation_};
}

bool EntryLog::invariants_hold() const {
  if (terms_.empty() || terms_.size() != ends_.size()) return false;
  if (terms_[0] != Term{0} || ends_[0] != 0) return false;
  if (ends_.back() != arena_.size()) return false;
  for (size_t i = 1; i < terms_.size(); ++i) {
    if (terms_[i] < terms_[i - 1] || ends_[i] < ends_[i - 1]) return false;
  }
  return true;
}

}  // namespace raftdst
