#include "sim/sim_storage.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <variant>
#include <vector>

#include "common/rng.h"
#include "support/property.h"

namespace raftdst::sim {
namespace {

// Independent model of the storage contract, written without EntryLog.
struct Disk {
  HardState hs;
  std::vector<std::pair<Term, std::vector<uint8_t>>> log;
};

struct AppendOp {
  uint64_t first;
  std::vector<std::pair<Term, std::vector<uint8_t>>> entries;
};
using StagedOp = std::variant<HardState, AppendOp>;

uint64_t units_of(const std::vector<StagedOp>& ops) {
  uint64_t u = 0;
  for (auto& op : ops) {
    u += std::holds_alternative<HardState>(op) ? 1 : std::get<AppendOp>(op).entries.size();
  }
  return u;
}

Disk apply_prefix(Disk d, const std::vector<StagedOp>& ops, uint64_t units) {
  for (auto& op : ops) {
    if (units == 0) break;
    if (auto* hs = std::get_if<HardState>(&op)) {
      d.hs = *hs;
      --units;
      continue;
    }
    auto& a = std::get<AppendOp>(op);
    d.log.resize(a.first - 1);
    const uint64_t k = std::min<uint64_t>(units, a.entries.size());
    d.log.insert(d.log.end(), a.entries.begin(), a.entries.begin() + static_cast<long>(k));
    units -= k;
  }
  return d;
}

bool same(const SimStorage& s, const Disk& d) {
  if (!(s.durable_hard_state() == d.hs)) return false;
  const EntryLog& log = s.durable_log();
  if (log.last_index() != Index{d.log.size()}) return false;
  for (size_t i = 0; i < d.log.size(); ++i) {
    Index idx{i + 1};
    auto p = log.payload_at(idx);
    if (log.term_at(idx) != d.log[i].first) return false;
    if (!std::equal(p.begin(), p.end(), d.log[i].second.begin(), d.log[i].second.end()))
      return false;
  }
  return true;
}

TEST(SimStorage, NothingDurableBeforeSync) {
  SimStorage s{1};
  EntryLog mem;
  mem.append(Term{1}, std::vector<uint8_t>{1});
  s.set_hard_state({Term{1}, NodeId{2}});
  s.append(mem.slice_all_from(Index{1}));
  EXPECT_EQ(s.durable_log().last_index(), Index{0});
  s.crash();
  EXPECT_EQ(s.durable_hard_state(), HardState{});
  EXPECT_EQ(s.load().log.last_index(), Index{0});
}

// Property: after a normal sync, durable == every staged write applied.
// After a crash inside sync, durable == some prefix of the staged writes,
// counted in units. After a plain crash, durable is unchanged.
TEST(SimStorage, CrashSemanticsMatchModel) {
  testing::for_each_seed(300, [](uint64_t seed) {
    Rng rng{seed};
    SimStorage storage{seed ^ 0xabcdef};
    Recovered r = storage.load();
    EntryLog mem = std::move(r.log);  // what the "node" believes
    Disk disk;
    std::vector<StagedOp> staged;
    uint64_t torn_seen = 0;

    for (int step = 0; step < 300; ++step) {
      const uint64_t op = rng.below(12);
      if (op < 3) {
        HardState hs{Term{rng.below(50)}, NodeId{static_cast<uint8_t>(rng.below(6))}};
        if (hs.voted_for.get() == 5) hs.voted_for = NodeId::none();
        storage.set_hard_state(hs);
        staged.emplace_back(hs);
      } else if (op < 7) {
        const uint64_t first = 1 + rng.below(mem.last_index().get() + 1);
        mem.truncate_after(Index{first - 1});
        AppendOp a{first, {}};
        Term t = mem.last_term();
        const uint64_t k = 1 + rng.below(4);
        for (uint64_t i = 0; i < k; ++i) {
          t = Term{t.get() + rng.below(2)};
          std::vector<uint8_t> p(rng.below(12));
          for (auto& b : p) b = static_cast<uint8_t>(rng.next());
          mem.append(t, p);
          a.entries.emplace_back(t, p);
        }
        storage.append(mem.slice_all_from(Index{first}));
        staged.emplace_back(std::move(a));
      } else if (op < 9) {
        storage.sync();
        disk = apply_prefix(disk, staged, units_of(staged));
        staged.clear();
        ASSERT_TRUE(same(storage, disk));
        ASSERT_EQ(storage.durable_log(), mem);
      } else if (op < 10) {
        storage.crash();
        staged.clear();
        ASSERT_TRUE(same(storage, disk));
        mem = storage.load().log;
      } else {
        storage.arm_crash_in_sync();
        EXPECT_THROW(storage.sync(), SimCrash);
        ++torn_seen;
        const uint64_t total = units_of(staged);
        bool matched = false;
        for (uint64_t u = 0; u <= total && !matched; ++u) {
          Disk candidate = apply_prefix(disk, staged, u);
          if (same(storage, candidate)) {
            disk = candidate;
            matched = true;
          }
        }
        ASSERT_TRUE(matched) << "durable state is not a prefix of the staged writes";
        staged.clear();
        mem = storage.load().log;
      }
      ASSERT_TRUE(storage.durable_log().invariants_hold());
    }
    ASSERT_GT(torn_seen, 0u);
  });
}

}  // namespace
}  // namespace raftdst::sim
