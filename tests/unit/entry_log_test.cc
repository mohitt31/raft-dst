#include "common/entry_log.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "common/rng.h"
#include "support/property.h"

namespace raftdst {
namespace {

// The obvious layout the SoA log replaced. Index i lives at model[i - 1].
using Model = std::vector<std::pair<Term, std::vector<uint8_t>>>;

std::vector<uint8_t> random_payload(Rng& rng) {
  std::vector<uint8_t> p(rng.below(24));
  for (auto& b : p) b = static_cast<uint8_t>(rng.next());
  return p;
}

void expect_same(const EntryLog& log, const Model& model) {
  ASSERT_TRUE(log.invariants_hold());
  ASSERT_EQ(log.last_index(), Index{model.size()});
  ASSERT_EQ(log.last_term(), model.empty() ? Term{0} : model.back().first);
  size_t bytes = 0;
  for (size_t i = 0; i < model.size(); ++i) {
    Index idx{i + 1};
    ASSERT_EQ(log.term_at(idx), model[i].first) << "index " << idx.get();
    auto p = log.payload_at(idx);
    ASSERT_TRUE(std::equal(p.begin(), p.end(), model[i].second.begin(), model[i].second.end()))
        << "index " << idx.get();
    bytes += model[i].second.size();
  }
  ASSERT_EQ(log.payload_bytes(), bytes);
  ASSERT_EQ(log.term_at(Index{0}), Term{0});
}

// Expected slice length computed the slow way from the model.
uint64_t model_slice_count(const Model& m, uint64_t first, uint64_t max_count, size_t max_bytes) {
  if (first > m.size() || max_count == 0) return 0;
  uint64_t count = 0;
  size_t bytes = 0;
  for (uint64_t i = first; i <= m.size() && count < max_count; ++i) {
    bytes += m[i - 1].second.size();
    if (bytes > max_bytes && count > 0) break;
    ++count;
  }
  return count;
}

TEST(EntryLog, EmptyLog) {
  EntryLog log;
  EXPECT_EQ(log.last_index(), Index{0});
  EXPECT_EQ(log.last_term(), Term{0});
  EXPECT_TRUE(log.has(Index{0}));
  EXPECT_FALSE(log.has(Index{1}));
  EXPECT_TRUE(log.slice(Index{1}, 10, 100).empty());
  EXPECT_TRUE(log.invariants_hold());
}

TEST(EntryLog, SliceAlwaysReturnsAtLeastOneEntry) {
  EntryLog log;
  std::vector<uint8_t> big(100, 7);
  log.append(Term{1}, big);
  log.append(Term{1}, big);
  auto s = log.slice(Index{1}, 10, 5);
  EXPECT_EQ(s.size(), 1u);
  EXPECT_EQ(s.payload_bytes(), 100u);
}

// Differential test: random append / bulk append / truncate / slice against
// the vector-of-pairs model, with every observable checked after each step.
TEST(EntryLog, MatchesModelUnderRandomOps) {
  testing::for_each_seed(300, [](uint64_t seed) {
    Rng rng{seed};
    EntryLog log;
    Model model;
    for (int step = 0; step < 200; ++step) {
      const uint64_t op = rng.below(10);
      const Term last = model.empty() ? Term{0} : model.back().first;
      if (op < 4) {
        Term t{last.get() + rng.below(2)};
        auto p = random_payload(rng);
        log.append(t, p);
        model.emplace_back(t, p);
      } else if (op < 6) {
        // Bulk append: a source log sharing our prefix, plus new entries.
        EntryLog src;
        for (auto& [t, p] : model) src.append(t, p);
        Term t = last;
        const uint64_t extra = rng.below(6);
        Model added;
        for (uint64_t i = 0; i < extra; ++i) {
          t = Term{t.get() + rng.below(2)};
          added.emplace_back(t, random_payload(rng));
          src.append(added.back().first, added.back().second);
        }
        const uint64_t max_count = rng.below(8);
        const size_t max_bytes = rng.below(2) ? SIZE_MAX : rng.below(64);
        Index from{model.size() + 1};
        LogSlice s = src.slice(from, max_count, max_bytes);
        const uint64_t want = model_slice_count(
            [&] {
              Model all = model;
              all.insert(all.end(), added.begin(), added.end());
              return all;
            }(),
            from.get(), max_count, max_bytes);
        ASSERT_EQ(s.size(), want);
        log.append(s);
        for (uint64_t i = 0; i < s.size(); ++i) model.push_back(added[i]);
      } else if (op < 8) {
        const uint64_t keep = rng.below(model.size() + 1);
        log.truncate_after(Index{keep});
        model.resize(keep);
      } else {
        const uint64_t first = 1 + rng.below(model.size() + 2);
        const uint64_t max_count = rng.below(10);
        const size_t max_bytes = rng.below(80);
        LogSlice s = log.slice(Index{first}, max_count, max_bytes);
        ASSERT_EQ(s.size(), model_slice_count(model, first, max_count, max_bytes));
        size_t bytes = 0;
        for (uint64_t i = 0; i < s.size(); ++i) {
          ASSERT_EQ(s.term(i), model[first - 1 + i].first);
          bytes += s.payload(i).size();
        }
        ASSERT_EQ(s.payload_bytes(), bytes);
      }
      expect_same(log, model);
      if (::testing::Test::HasFatalFailure()) return;
    }
  });
}

TEST(EntryLog, CopiesCompareEqual) {
  EntryLog a;
  a.append(Term{1}, std::vector<uint8_t>{1, 2});
  EntryLog b = a;
  EXPECT_EQ(a, b);
  b.append(Term{1}, std::vector<uint8_t>{});
  EXPECT_FALSE(a == b);
}

class EntryLogDeathTest : public ::testing::Test {
 protected:
  void SetUp() override { GTEST_FLAG_SET(death_test_style, "threadsafe"); }
};

TEST_F(EntryLogDeathTest, RejectsDecreasingTerm) {
  EntryLog log;
  log.append(Term{3}, {});
  EXPECT_DEATH(log.append(Term{2}, {}), "lower term");
}

TEST_F(EntryLogDeathTest, RejectsGapOnSliceAppend) {
  EntryLog src;
  src.append(Term{1}, {});
  src.append(Term{1}, {});
  EntryLog dst;
  EXPECT_DEATH(dst.append(src.slice(Index{2}, 1, SIZE_MAX)), "right after the log end");
}

#ifdef RAFTDST_DCHECKS
TEST_F(EntryLogDeathTest, SliceUsedAfterTruncateIsCaught) {
  EntryLog log;
  log.append(Term{1}, {});
  log.append(Term{1}, {});
  LogSlice s = log.slice(Index{1}, 2, SIZE_MAX);
  log.truncate_after(Index{0});
  EXPECT_DEATH(static_cast<void>(s.term(0)), "after its log was truncated");
}
#endif

}  // namespace
}  // namespace raftdst
