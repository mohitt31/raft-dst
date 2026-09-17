#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "common/bytes.h"
#include "common/rng.h"
#include "kv/command.h"
#include "kv/kv_store.h"
#include "support/property.h"

namespace raftdst::kv {
namespace {

std::string random_string(Rng& rng, size_t min, size_t max) {
  std::string s(min + rng.below(max - min + 1), '\0');
  for (char& c : s) c = static_cast<char>(rng.next());  // any byte, including 0
  return s;
}

TEST(KvCommand, RoundTripAndStrictness) {
  testing::for_each_seed(2000, [](uint64_t seed) {
    Rng rng{seed};
    const bool del = rng.below(3) == 0;
    const std::string key = random_string(rng, 1, 40);
    const std::string value = del ? "" : random_string(rng, 0, 60);
    std::vector<uint8_t> b;
    encode_command({del ? Op::kDel : Op::kPut, key, value}, b);

    Command c;
    ASSERT_TRUE(decode_command(b, c));
    ASSERT_EQ(c.op, del ? Op::kDel : Op::kPut);
    ASSERT_EQ(c.key, key);
    ASSERT_EQ(c.value, value);

    for (size_t len = 0; len < b.size(); ++len) {
      std::vector<uint8_t> prefix(b.begin(), b.begin() + static_cast<long>(len));
      ASSERT_FALSE(decode_command(prefix, c)) << "prefix " << len;
    }
    b.push_back(0);
    ASSERT_FALSE(decode_command(b, c)) << "trailing byte accepted";
  });
}

TEST(KvCommand, RejectsBadOpAndKeyLength) {
  std::vector<uint8_t> b;
  encode_command({Op::kPut, "k", "v"}, b);
  auto bad = b;
  bad[0] = 3;
  Command c;
  EXPECT_FALSE(decode_command(bad, c));
  bad = b;
  bad[1] = 0;  // key_len 0
  bad[2] = 0;
  EXPECT_FALSE(decode_command(bad, c));
  EXPECT_FALSE(decode_command({}, c));
}

// Field-by-field random commands with small lengths, a sometimes-wrong op
// byte and sometimes a byte too many or too few. Accepted ones must
// re-encode to the same bytes.
TEST(KvCommand, RandomCommandsAcceptedOnlyIfCanonical) {
  uint64_t accepted = 0, total = 0;
  testing::for_each_seed(20000, [&](uint64_t seed) {
    Rng rng{seed};
    std::vector<uint8_t> b;
    ByteWriter w{b};
    w.u8(static_cast<uint8_t>(rng.below(4)));
    const auto key_len = static_cast<uint16_t>(rng.below(8) == 0 ? rng.next() : rng.below(4));
    w.u16(key_len);
    for (uint64_t i = std::min<uint64_t>(key_len, 8); i > 0; --i)
      w.u8(static_cast<uint8_t>(rng.next()));
    if (rng.below(2) == 0) {
      const auto value_len = static_cast<uint32_t>(rng.below(8) == 0 ? rng.next() : rng.below(4));
      w.u32(value_len);
      for (uint64_t i = std::min<uint64_t>(value_len, 8); i > 0; --i)
        w.u8(static_cast<uint8_t>(rng.next()));
    }
    if (rng.below(10) == 0) b.push_back(0);
    if (rng.below(10) == 0 && !b.empty()) b.pop_back();

    ++total;
    Command c;
    if (!decode_command(b, c)) return;
    ++accepted;
    std::vector<uint8_t> again;
    encode_command(c, again);
    ASSERT_EQ(b, again);
  });
  EXPECT_GT(accepted, total / 20);
  EXPECT_LT(accepted, total / 2);
}

// Differential test: the store (unordered_map, incremental content hash)
// against std::map with the hash recomputed from scratch every step.
TEST(KvStore, MatchesOrderedMapModel) {
  testing::for_each_seed(300, [](uint64_t seed) {
    Rng rng{seed};
    KvStore store;
    std::map<std::string, std::string> model;
    std::vector<uint8_t> cmd;
    for (uint64_t i = 1; i <= 400; ++i) {
      cmd.clear();
      const uint64_t kind = rng.below(10);
      if (kind == 0) {
        store.apply(Index{i}, {});  // no-op
      } else {
        const std::string key = "k" + std::to_string(rng.below(12));
        if (kind < 4) {
          encode_command({Op::kDel, key, ""}, cmd);
          model.erase(key);
        } else {
          const std::string value = random_string(rng, 0, 8);
          encode_command({Op::kPut, key, value}, cmd);
          model[key] = value;
        }
        ASSERT_TRUE(store.valid(cmd));
        store.apply(Index{i}, cmd);
      }
      uint64_t expect = 0;
      for (const auto& [k, v] : model) expect ^= pair_hash(k, v);
      ASSERT_EQ(store.digest(), expect) << "after index " << i;
      ASSERT_EQ(store.size(), model.size());
      ASSERT_EQ(store.applied(), Index{i});
    }
    for (const auto& [k, v] : model) ASSERT_EQ(store.get(k), std::string_view{v});
    ASSERT_FALSE(store.get("absent").has_value());
  });
}

TEST(KvStore, DigestDependsOnContentNotHistory) {
  auto build = [](std::initializer_list<std::pair<const char*, const char*>> puts) {
    KvStore s;
    std::vector<uint8_t> cmd;
    uint64_t i = 0;
    for (auto [k, v] : puts) {
      cmd.clear();
      encode_command({Op::kPut, k, v}, cmd);
      s.apply(Index{++i}, cmd);
    }
    return s.digest();
  };
  EXPECT_EQ(build({{"a", "1"}, {"b", "2"}}), build({{"b", "2"}, {"a", "0"}, {"a", "1"}}));
  EXPECT_NE(build({{"ab", "c"}}), build({{"a", "bc"}}));
}

class KvStoreDeathTest : public ::testing::Test {
 protected:
  void SetUp() override { GTEST_FLAG_SET(death_test_style, "threadsafe"); }
};

TEST_F(KvStoreDeathTest, AppliesOnlyInOrder) {
  KvStore s;
  EXPECT_DEATH(s.apply(Index{2}, {}), "out of order");
}

}  // namespace
}  // namespace raftdst::kv
