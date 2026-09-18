#pragma once

#include <time.h>

#include "common/check.h"
#include "env/clock.h"

namespace raftdst::real {

// CLOCK_MONOTONIC. The wakeup is just a deadline the event loop reads to
// compute its poll timeout; nothing fires on its own.
class RealClock final : public IClock {
 public:
  [[nodiscard]] Time now() const override {
    timespec ts{};
    RAFT_ASSERT(clock_gettime(CLOCK_MONOTONIC, &ts) == 0, "clock_gettime failed");
    return Time::from_ns(static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec);
  }

  void wake_at(Time t) override { deadline_ = t; }

  [[nodiscard]] Time deadline() const { return deadline_; }

 private:
  Time deadline_ = Time::max();
};

}  // namespace raftdst::real
