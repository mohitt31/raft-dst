#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "common/crc32c.h"
#include "common/hash.h"
#include "common/rng.h"
#include "support/property.h"

namespace raftdst {
namespace {

std::span<const uint8_t> as_bytes(std::string_view s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

TEST(Fnv1a64, PublishedVectors) {
  auto h = [](std::string_view s) {
    Fnv1a64 f;
    f.add_bytes(as_bytes(s));
    return f.value();
  };
  EXPECT_EQ(h(""), 0xcbf29ce484222325ULL);
  EXPECT_EQ(h("a"), 0xaf63dc4c8601ec8cULL);
  EXPECT_EQ(h("foobar"), 0x85944171f73967e8ULL);
}

TEST(Fnv1a64, IntegersAreLittleEndianBytes) {
  Fnv1a64 a, b;
  a.add_u64(0x0102030405060708ULL);
  a.add_u8(0xAB);
  const std::array<uint8_t, 9> le{8, 7, 6, 5, 4, 3, 2, 1, 0xAB};
  b.add_bytes(le);
  EXPECT_EQ(a.value(), b.value());
}

TEST(Crc32c, CheckValue) { EXPECT_EQ(crc32c(as_bytes("123456789")), 0xe3069283u); }

TEST(Crc32c, Rfc3720Vectors) {
  std::array<uint8_t, 32> buf{};
  EXPECT_EQ(crc32c(buf), 0x8a9136aau);
  buf.fill(0xff);
  EXPECT_EQ(crc32c(buf), 0x62a8ab43u);
  for (int i = 0; i < 32; ++i) buf[i] = static_cast<uint8_t>(i);
  EXPECT_EQ(crc32c(buf), 0x46dd794eu);
  for (int i = 0; i < 32; ++i) buf[i] = static_cast<uint8_t>(31 - i);
  EXPECT_EQ(crc32c(buf), 0x113fdb5cu);
}

TEST(Crc32c, ExtendMatchesOneShot) {
  testing::for_each_seed(500, [](uint64_t seed) {
    Rng rng{seed};
    std::vector<uint8_t> data(rng.below(512));
    for (auto& b : data) b = static_cast<uint8_t>(rng.next());
    const size_t split = data.empty() ? 0 : rng.below(data.size() + 1);
    std::span<const uint8_t> all{data};
    uint32_t part = crc32c_extend(crc32c(all.first(split)), all.subspan(split));
    ASSERT_EQ(part, crc32c(all));
  });
}

TEST(Crc32c, DetectsEverySingleBitFlip) {
  std::vector<uint8_t> data(64);
  Rng rng{77};
  for (auto& b : data) b = static_cast<uint8_t>(rng.next());
  const uint32_t good = crc32c(data);
  for (size_t i = 0; i < data.size() * 8; ++i) {
    data[i / 8] ^= static_cast<uint8_t>(1u << (i % 8));
    ASSERT_NE(crc32c(data), good) << "bit " << i;
    data[i / 8] ^= static_cast<uint8_t>(1u << (i % 8));
  }
}

}  // namespace
}  // namespace raftdst
