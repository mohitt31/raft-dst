#include "sim/event_queue.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "common/rng.h"
#include "support/property.h"

namespace raftdst::sim {
namespace {

TEST(EventQueue, EqualTimesPopInInsertionOrder) {
  EventQueue q;
  for (uint32_t i = 0; i < 100; ++i) q.push(Time::from_ns(5), EventKind::kWake, 0, i);
  q.push(Time::from_ns(1), EventKind::kWake, 0, 999);
  EXPECT_EQ(q.pop().arg, 999u);
  for (uint32_t i = 0; i < 100; ++i) ASSERT_EQ(q.pop().arg, i);
  EXPECT_TRUE(q.empty());
}

// Differential test against the slowest correct model: a vector searched
// for the minimum (at, seq) on every pop. Times come from a tiny range so
// ties are the common case.
TEST(EventQueue, MatchesLinearScanModel) {
  testing::for_each_seed(200, [](uint64_t seed) {
    Rng rng{seed};
    EventQueue q;
    std::vector<Event> model;
    uint64_t seq = 0;
    for (int step = 0; step < 2000; ++step) {
      if (model.empty() || rng.below(3) != 0) {
        const Time at = Time::from_ns(static_cast<int64_t>(rng.below(16)));
        const auto arg = static_cast<uint32_t>(rng.next());
        q.push(at, EventKind::kDeliver, static_cast<uint8_t>(rng.below(5)), arg);
        model.push_back(Event{at, seq++, arg, EventKind::kDeliver, 0});
      } else {
        auto it = std::min_element(model.begin(), model.end(), event_before);
        Event got = q.pop();
        ASSERT_EQ(got.at, it->at);
        ASSERT_EQ(got.seq, it->seq);
        ASSERT_EQ(got.arg, it->arg);
        model.erase(it);
      }
      ASSERT_EQ(q.size(), model.size());
    }
  });
}

}  // namespace
}  // namespace raftdst::sim
