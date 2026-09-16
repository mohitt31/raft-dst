#pragma once

#include "common/entry_log.h"
#include "common/types.h"

namespace raftdst {

struct HardState {
  Term term{};
  NodeId voted_for = NodeId::none();
  friend bool operator==(const HardState&, const HardState&) = default;
};

struct Recovered {
  HardState hard_state;
  EntryLog log;
};

// Durable Raft state: hard state plus the log. Writes are buffered until
// sync(); after sync() returns, everything written before it survives a
// crash. A crash before sync() returns may keep any prefix of the unsynced
// writes, in order, but never a later write without the earlier ones.
class IStorage {
 public:
  virtual ~IStorage() = default;

  virtual void set_hard_state(const HardState& hs) = 0;

  // Replaces the log from entries.first() onward with `entries`. The slice
  // only needs to stay valid for the duration of the call.
  virtual void append(const LogSlice& entries) = 0;

  virtual void sync() = 0;

  // Durable state as of the last completed sync (or a crash-legal prefix of
  // later writes). Called once when a node starts.
  [[nodiscard]] virtual Recovered load() = 0;
};

}  // namespace raftdst
