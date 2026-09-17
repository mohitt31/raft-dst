#pragma once

#include <cstdint>
#include <span>

#include "common/types.h"

namespace raftdst {

// The replicated application. Entries are applied in index order, exactly
// once per process lifetime; after a restart they are applied again from
// index 1 (there are no snapshots).
class IStateMachine {
 public:
  virtual ~IStateMachine() = default;

  // Whether `command` is well-formed. Checked before a command is proposed,
  // so every non-empty committed entry is valid.
  [[nodiscard]] virtual bool valid(std::span<const uint8_t> command) const = 0;

  // Applies entry `index`. Empty entries are leader no-ops and change nothing.
  virtual void apply(Index index, std::span<const uint8_t> command) = 0;

  // Fingerprint of the current state; equal states give equal digests.
  [[nodiscard]] virtual uint64_t digest() const = 0;
};

}  // namespace raftdst
