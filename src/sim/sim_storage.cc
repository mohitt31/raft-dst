#include "sim/sim_storage.h"

#include <algorithm>

namespace raftdst::sim {

void SimStorage::set_hard_state(const HardState& hs) {
  ops_.push_back(Op{false, hs, Index{}, 0, 0});
  ++staged_units_;
}

void SimStorage::append(const LogSlice& entries) {
  RAFT_ASSERT(entries.first().get() >= 1, "append at the sentinel index");
  if (entries.empty()) return;
  ops_.push_back(Op{true, HardState{}, entries.first(), entries.size(), staged_terms_.size()});
  for (uint64_t i = 0; i < entries.size(); ++i) {
    auto p = entries.payload(i);
    staged_bytes_.insert(staged_bytes_.end(), p.begin(), p.end());
    staged_terms_.push_back(entries.term(i));
    staged_ends_.push_back(static_cast<uint32_t>(staged_bytes_.size()));
  }
  staged_units_ += entries.size();
}

void SimStorage::sync() {
  if (crash_armed_) {
    crash_armed_ = false;
    apply_units(torn_rng_.below(staged_units_ + 1));
    clear_staged();
    throw SimCrash{};
  }
  apply_units(staged_units_);
  clear_staged();
}

Recovered SimStorage::load() {
  RAFT_ASSERT(ops_.empty(), "load() with unsynced writes");
  return Recovered{hard_state_, log_};
}

void SimStorage::crash() {
  crash_armed_ = false;
  clear_staged();
}

void SimStorage::apply_units(uint64_t units) {
  for (const Op& op : ops_) {
    if (units == 0) return;
    if (!op.is_append) {
      hard_state_ = op.hs;
      --units;
      continue;
    }
    RAFT_ASSERT(op.first <= log_.last_index().next(),
                "append leaves a gap in the log (storage contract violated)");
    log_.truncate_after(op.first.prev());
    const uint64_t k = std::min(units, op.count);
    for (uint64_t i = 0; i < k; ++i) {
      const size_t j = op.term_offset + i;
      const uint32_t b = j == 0 ? 0 : staged_ends_[j - 1];
      log_.append(staged_terms_[j],
                  std::span<const uint8_t>{staged_bytes_.data() + b, staged_ends_[j] - b});
    }
    units -= k;
  }
}

void SimStorage::clear_staged() {
  ops_.clear();
  staged_terms_.clear();
  staged_ends_.clear();
  staged_bytes_.clear();
  staged_units_ = 0;
}

}  // namespace raftdst::sim
