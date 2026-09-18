#include "real/file_wal.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/rng.h"
#include "sim/sim_storage.h"
#include "support/property.h"

namespace raftdst::real {
namespace {

namespace fs = std::filesystem;

// A temporary file that removes itself.
class TempPath {
 public:
  explicit TempPath(const std::string& name)
      : path_(fs::temp_directory_path() /
              (name + "." + std::to_string(::getpid()) + "." + std::to_string(counter_++))) {}
  ~TempPath() {
    std::error_code ignored;
    fs::remove(path_, ignored);
  }
  [[nodiscard]] const std::string string() const { return path_.string(); }
  [[nodiscard]] std::vector<uint8_t> read() const {
    std::ifstream in{path_, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  }
  void write(const std::vector<uint8_t>& bytes) const {
    std::ofstream out{path_, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }

 private:
  static inline int counter_ = 0;
  fs::path path_;
};

std::unique_ptr<FileWal> open_or_fail(const TempPath& p) {
  std::string error;
  auto wal = FileWal::open(p.string(), &error);
  EXPECT_NE(wal, nullptr) << error;
  return wal;
}

bool same(const Recovered& a, const Recovered& b) {
  return a.hard_state == b.hard_state && a.log == b.log;
}

TEST(FileWal, EmptyFileRecoversEmptyState) {
  TempPath p{"wal-empty"};
  auto wal = open_or_fail(p);
  const Recovered r = wal->load();
  EXPECT_EQ(r.hard_state, HardState{});
  EXPECT_EQ(r.log.last_index(), Index{0});
}

TEST(FileWal, UnsyncedWritesAreLost) {
  TempPath p{"wal-unsynced"};
  EntryLog mem;
  mem.append(Term{1}, std::vector<uint8_t>{7});
  {
    auto wal = open_or_fail(p);
    static_cast<void>(wal->load());
    wal->set_hard_state({Term{3}, NodeId{1}});
    wal->sync();
    wal->append(mem.slice_all_from(Index{1}));  // staged, never synced
  }
  auto wal = open_or_fail(p);
  const Recovered r = wal->load();
  EXPECT_EQ(r.hard_state, (HardState{Term{3}, NodeId{1}}));
  EXPECT_EQ(r.log.last_index(), Index{0});
}

// The same script of operations against the simulated disk and the real
// file: after every sync both must hold exactly the same state.
TEST(FileWal, MatchesSimulatedStorage) {
  testing::for_each_seed(60, [](uint64_t seed) {
    TempPath p{"wal-diff"};
    auto wal = open_or_fail(p);
    static_cast<void>(wal->load());
    sim::SimStorage sim{seed};
    static_cast<void>(sim.load());

    Rng rng{seed};
    EntryLog mem;
    for (int step = 0; step < 120; ++step) {
      const uint64_t op = rng.below(10);
      if (op < 3) {
        HardState hs{Term{rng.below(40)}, NodeId{static_cast<uint8_t>(rng.below(6))}};
        if (hs.voted_for.get() == 5) hs.voted_for = NodeId::none();
        wal->set_hard_state(hs);
        sim.set_hard_state(hs);
      } else if (op < 7) {
        const uint64_t first = 1 + rng.below(mem.last_index().get() + 1);
        mem.truncate_after(Index{first - 1});
        Term t = mem.last_term();
        const uint64_t count = 1 + rng.below(4);
        for (uint64_t i = 0; i < count; ++i) {
          t = Term{t.get() + rng.below(2)};
          std::vector<uint8_t> payload(rng.below(20));
          for (auto& b : payload) b = static_cast<uint8_t>(rng.next());
          mem.append(t, payload);
        }
        wal->append(mem.slice_all_from(Index{first}));
        sim.append(mem.slice_all_from(Index{first}));
      } else {
        wal->sync();
        sim.sync();
        Recovered from_file;
        const uint64_t good = FileWal::replay(p.read(), from_file);
        ASSERT_EQ(good, p.read().size()) << "a synced file must have no torn tail";
        ASSERT_EQ(from_file.hard_state, sim.durable_hard_state());
        ASSERT_EQ(from_file.log, sim.durable_log());
        ASSERT_EQ(from_file.log, mem);
      }
    }
  });
}

// Every prefix of a valid file must recover to a state that some prefix of
// the records produces: a crash mid-write can only lose a tail.
TEST(FileWal, EveryTruncatedTailRecoversToARecordPrefix) {
  testing::for_each_seed(20, [](uint64_t seed) {
    TempPath p{"wal-torn"};
    std::vector<Recovered> states;  // state after each record
    {
      auto wal = open_or_fail(p);
      static_cast<void>(wal->load());
      Rng rng{seed};
      EntryLog mem;
      sim::SimStorage model{seed};
      static_cast<void>(model.load());
      for (int step = 0; step < 12; ++step) {
        if (rng.below(3) == 0) {
          const HardState hs{Term{rng.below(20)}, NodeId{static_cast<uint8_t>(rng.below(3))}};
          wal->set_hard_state(hs);
          model.set_hard_state(hs);
        } else {
          const uint64_t first = 1 + rng.below(mem.last_index().get() + 1);
          mem.truncate_after(Index{first - 1});
          Term t = mem.last_term();
          for (uint64_t i = 0, n = 1 + rng.below(3); i < n; ++i) {
            t = Term{t.get() + rng.below(2)};
            mem.append(t, std::vector<uint8_t>(rng.below(8), static_cast<uint8_t>(step)));
          }
          wal->append(mem.slice_all_from(Index{first}));
          model.append(mem.slice_all_from(Index{first}));
        }
        wal->sync();  // one record per sync, so record ends are state points
        model.sync();
        states.push_back(Recovered{model.durable_hard_state(), model.durable_log()});
      }
    }

    const std::vector<uint8_t> image = p.read();
    for (size_t len = 0; len <= image.size(); ++len) {
      Recovered recovered;
      FileWal::replay({image.data(), len}, recovered);
      bool matched = recovered.log.last_index() == Index{0} && recovered.hard_state == HardState{};
      for (const Recovered& s : states) matched = matched || same(recovered, s);
      ASSERT_TRUE(matched) << "truncating to " << len << " bytes gave a state no record prefix can";
    }
  });
}

TEST(FileWal, StopsAtACorruptedRecordAndTruncatesIt) {
  TempPath p{"wal-corrupt"};
  EntryLog mem;
  {
    auto wal = open_or_fail(p);
    static_cast<void>(wal->load());
    wal->set_hard_state({Term{2}, NodeId{0}});
    wal->sync();
    mem.append(Term{2}, std::vector<uint8_t>{1, 2, 3});
    wal->append(mem.slice_all_from(Index{1}));
    wal->sync();
  }
  std::vector<uint8_t> image = p.read();
  const size_t good_prefix = image.size();
  // Corrupt a byte inside the last record's body.
  image[image.size() - 2] ^= 0x40;
  p.write(image);

  Recovered r;
  const uint64_t good = FileWal::replay(image, r);
  EXPECT_LT(good, good_prefix);
  EXPECT_EQ(r.hard_state, (HardState{Term{2}, NodeId{0}}));
  EXPECT_EQ(r.log.last_index(), Index{0});  // the entries record was rejected

  // Opening cuts the damaged tail, and the file is writable again.
  {
    auto wal = open_or_fail(p);
    const Recovered loaded = wal->load();
    EXPECT_EQ(loaded.log.last_index(), Index{0});
    EXPECT_EQ(wal->stats().torn_tail_bytes, image.size() - good);
    wal->append(mem.slice_all_from(Index{1}));
    wal->sync();
  }
  auto wal = open_or_fail(p);
  const Recovered after = wal->load();
  EXPECT_EQ(after.log, mem);
  EXPECT_EQ(wal->stats().torn_tail_bytes, 0u);
}

TEST(FileWal, GarbageFileNeverCrashesReplay) {
  testing::for_each_seed(500, [](uint64_t seed) {
    Rng rng{seed};
    std::vector<uint8_t> image(rng.below(80));
    for (auto& b : image) b = static_cast<uint8_t>(rng.below(4) == 0 ? rng.next() : rng.below(3));
    Recovered r;
    const uint64_t good = FileWal::replay(image, r);
    ASSERT_LE(good, image.size());
    ASSERT_TRUE(r.log.invariants_hold());
  });
}

}  // namespace
}  // namespace raftdst::real
