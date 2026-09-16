#pragma once

#include "common/types.h"

namespace raftdst {

// A node's view of time plus a single wakeup slot.
class IClock {
 public:
  virtual ~IClock() = default;

  // Monotonic. Never goes backwards within one process lifetime.
  [[nodiscard]] virtual Time now() const = 0;

  // Asks for INode::on_wake() at or after `t`. Replaces any earlier request,
  // so at most one wakeup is ever pending. A time in the past means "soon".
  virtual void wake_at(Time t) = 0;
};

}  // namespace raftdst
