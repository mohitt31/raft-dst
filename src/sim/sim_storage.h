#pragma once

#include <cstdint>
#include <vector>

#include "common/entry_log.h"
#include "common/rng.h"
#include "env/storage.h"

namespace raftdst::sim {

// Thrown out of SimStorage::sync() when a crash was armed for it. The world
// catches it at the node callback boundary and destroys the node, so no code
// after the crash point runs: no sends, no applies, no observations.
struct SimCrash {};

// In-memory disk with crash semantics. Writes are staged until sync().
//   crash()           staged writes are lost
//   armed + sync()    a random prefix of the staged writes survives, then SimCrash
//
// The prefix is counted in units: one per set_hard_state and one per appended
// entry, in write order. An append's truncation only lands together with its
// first entry, matching a WAL where the truncation point is part of the
// entries record.
class SimStorage final : public IStorage {
 public:
  explicit SimStorage(uint64_t torn_seed) : torn_rng_(torn_seed) {}

  void set_hard_state(const HardState& hs) override;
  void append(const LogSlice& entries) override;
  void sync() override;
  [[nodiscard]] Recovered load() override;

  void crash();
  void arm_crash_in_sync() { crash_armed_ = true; }
  void disarm_crash_in_sync() { crash_armed_ = false; }
  [[nodiscard]] bool crash_armed() const { return crash_armed_; }

  [[nodiscard]] const HardState& durable_hard_state() const { return hard_state_; }
  [[nodiscard]] const EntryLog& durable_log() const { return log_; }
  [[nodiscard]] uint64_t staged_units() const { return staged_units_; }

 private:
  struct Op {
    bool is_append;
    HardState hs;
    Index first;
    uint64_t count;
    size_t term_offset;  // into staged_terms_ / staged_ends_
  };

  void apply_units(uint64_t units);
  void clear_staged();

  HardState hard_state_;
  EntryLog log_;

  std::vector<Op> ops_;
  std::vector<Term> staged_terms_;
  std::vector<uint32_t> staged_ends_;  // end offsets into staged_bytes_
  std::vector<uint8_t> staged_bytes_;
  uint64_t staged_units_ = 0;

  bool crash_armed_ = false;
  Rng torn_rng_;
};

}  // namespace raftdst::sim
