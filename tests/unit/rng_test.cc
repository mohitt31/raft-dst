#include "common/rng.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "support/property.h"

namespace raftdst {
namespace {

TEST(SplitMix64, PublishedVectors) {
  // Seed 1234567 sequence used by several reference implementations.
  SplitMix64 sm{1234567};
  EXPECT_EQ(sm.next(), 6457827717110365317ULL);
  EXPECT_EQ(sm.next(), 3203168211198807973ULL);
  EXPECT_EQ(sm.next(), 9817491932198370423ULL);
  EXPECT_EQ(sm.next(), 4593380528125082431ULL);
  EXPECT_EQ(sm.next(), 16408922859458223821ULL);

  SplitMix64 zero{0};
  EXPECT_EQ(zero.next(), 0xe220a8397b1dcdafULL);
}

// Straight transcription of the reference xoshiro256starstar.c, kept in the
// reference's own style (global-like state array, free function) on purpose.
struct ReferenceXoshiro {
  uint64_t s[4];
  static uint64_t rotl(const uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
  uint64_t next() {
    const uint64_t result = rotl(s[1] * 5, 7) * 9;
    const uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
  }
};

TEST(Rng, MatchesReferenceXoshiro) {
  SplitMix64 sm{99};
  ReferenceXoshiro ref{{sm.next(), sm.next(), sm.next(), sm.next()}};
  Rng rng{99};
  for (int i = 0; i < 10'000; ++i) ASSERT_EQ(rng.next(), ref.next()) << "i=" << i;
}

// Change detector. Every committed regression seed depends on these streams
// staying bit-for-bit identical. If this fails, the generator changed and all
// recorded seeds are invalid.
TEST(Rng, FrozenStreams) {
  Rng a{42};
  EXPECT_EQ(a.next(), 1546998764402558742ULL);
  EXPECT_EQ(a.next(), 6990951692964543102ULL);
  Rng d = Rng::derive(42, 3);
  EXPECT_EQ(d.next(), 16341344995327714024ULL);
  EXPECT_EQ(Rng{7}.below(1000), 700u);
}

TEST(Rng, SameSeedSameSequence) {
  Rng a{5}, b{5};
  for (int i = 0; i < 1000; ++i) ASSERT_EQ(a.next(), b.next());
}

TEST(Rng, DerivedStreamsAreIndependent) {
  Rng a = Rng::derive(1, 0);
  Rng b = Rng::derive(1, 1);
  Rng c = Rng::derive(2, 0);
  int equal_ab = 0, equal_ac = 0;
  for (int i = 0; i < 1000; ++i) {
    uint64_t x = a.next(), y = b.next(), z = c.next();
    equal_ab += x == y;
    equal_ac += x == z;
  }
  EXPECT_EQ(equal_ab, 0);
  EXPECT_EQ(equal_ac, 0);

  // Drawing heavily from one stream does not move another.
  Rng net = Rng::derive(9, 1), crash1 = Rng::derive(9, 2), crash2 = Rng::derive(9, 2);
  for (int i = 0; i < 12345; ++i) net.next();
  EXPECT_EQ(crash1.next(), crash2.next());
}

// Exhaustive check of the rejection rule on 8-bit words. For every bound n and
// every possible first draw x, count which outputs are accepted without a
// retry. Unbiased means every output in [0, n) is hit equally often.
TEST(Rng, LemireRejectionIsExactlyUniformOn8BitWords) {
  for (unsigned n = 1; n <= 255; ++n) {
    std::array<int, 256> hits{};
    for (unsigned x = 0; x <= 255; ++x) {
      int calls = 0;
      auto next = [&]() -> uint8_t {
        // First call returns x. A retry gets 255, which is always accepted,
        // and marks this x as rejected.
        return ++calls == 1 ? static_cast<uint8_t>(x) : uint8_t{255};
      };
      uint8_t out = detail::lemire_bounded<uint8_t, uint16_t>(next, static_cast<uint8_t>(n));
      ASSERT_LT(out, n);
      if (calls == 1) ++hits[out];
    }
    for (unsigned v = 1; v < n; ++v) ASSERT_EQ(hits[v], hits[0]) << "n=" << n << " v=" << v;
    ASSERT_EQ(hits[0], static_cast<int>(256 / n)) << "n=" << n;
  }
}

TEST(Rng, BelowStaysInRange) {
  testing::for_each_seed(200, [](uint64_t seed) {
    Rng rng{seed};
    Rng bounds{~seed};
    for (int i = 0; i < 1000; ++i) {
      uint64_t n = bounds.next() >> bounds.below(64);
      if (n == 0) n = 1;
      ASSERT_LT(rng.below(n), n);
    }
    ASSERT_EQ(rng.below(1), 0u);
  });
}

TEST(Rng, BetweenIsInclusiveAndHandlesExtremes) {
  Rng rng{3};
  bool saw_lo = false, saw_hi = false;
  for (int i = 0; i < 1000; ++i) {
    int64_t v = rng.between(-2, 2);
    ASSERT_GE(v, -2);
    ASSERT_LE(v, 2);
    saw_lo |= v == -2;
    saw_hi |= v == 2;
  }
  EXPECT_TRUE(saw_lo && saw_hi);
  EXPECT_EQ(rng.between(7, 7), 7);
  rng.between(INT64_MIN, INT64_MAX);  // full range must not trip the n > 0 check
}

TEST(Rng, ChancePpm) {
  Rng rng{11};
  for (int i = 0; i < 1000; ++i) {
    ASSERT_FALSE(rng.chance_ppm(0));
    ASSERT_TRUE(rng.chance_ppm(Rng::kMillion));
  }
  // Fixed seed, so this is deterministic, not a flaky statistical test.
  int hits = 0;
  const int n = 1'000'000;
  for (int i = 0; i < n; ++i) hits += rng.chance_ppm(250'000);
  EXPECT_NEAR(hits, n / 4, 3000);
}

}  // namespace
}  // namespace raftdst
