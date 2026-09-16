#include "common/bytes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "common/rng.h"
#include "support/property.h"

namespace raftdst {
namespace {

TEST(Bytes, LittleEndianLayout) {
  std::vector<uint8_t> buf;
  ByteWriter w{buf};
  w.u16(0x0102);
  w.u32(0x03040506);
  w.u64(0x0708090a0b0c0d0eULL);
  const std::vector<uint8_t> want{0x02, 0x01, 0x06, 0x05, 0x04, 0x03, 0x0e,
                                  0x0d, 0x0c, 0x0b, 0x0a, 0x09, 0x08, 0x07};
  EXPECT_EQ(buf, want);
}

TEST(Bytes, PatchU32) {
  std::vector<uint8_t> buf;
  ByteWriter w{buf};
  w.u32(0);
  w.u8(9);
  w.patch_u32(0, 0xdeadbeef);
  ByteReader r{buf};
  EXPECT_EQ(r.u32(), 0xdeadbeefu);
  EXPECT_EQ(r.u8(), 9);
  EXPECT_TRUE(r.ok() && r.at_end());
}

// A random script of typed writes. Used for round-trip and truncation tests.
struct Op {
  uint8_t kind;  // 0..3 = u8,u16,u32,u64, 4 = bytes
  uint64_t value;
  std::vector<uint8_t> blob;
};

std::vector<Op> random_script(Rng& rng) {
  std::vector<Op> ops(rng.below(40) + 1);
  for (auto& op : ops) {
    op.kind = static_cast<uint8_t>(rng.below(5));
    op.value = rng.next();
    if (op.kind == 4) {
      op.blob.resize(rng.below(20));
      for (auto& b : op.blob) b = static_cast<uint8_t>(rng.next());
    }
  }
  return ops;
}

void write_script(const std::vector<Op>& ops, std::vector<uint8_t>& buf) {
  ByteWriter w{buf};
  for (const auto& op : ops) {
    switch (op.kind) {
      case 0:
        w.u8(static_cast<uint8_t>(op.value));
        break;
      case 1:
        w.u16(static_cast<uint16_t>(op.value));
        break;
      case 2:
        w.u32(static_cast<uint32_t>(op.value));
        break;
      case 3:
        w.u64(op.value);
        break;
      default:
        w.u32(static_cast<uint32_t>(op.blob.size()));
        w.bytes(op.blob);
        break;
    }
  }
}

// Reads the script back. Returns how many ops matched before the reader
// reported an error.
size_t read_script(const std::vector<Op>& ops, ByteReader& r) {
  size_t good = 0;
  for (const auto& op : ops) {
    bool match = false;
    switch (op.kind) {
      case 0:
        match = r.u8() == static_cast<uint8_t>(op.value);
        break;
      case 1:
        match = r.u16() == static_cast<uint16_t>(op.value);
        break;
      case 2:
        match = r.u32() == static_cast<uint32_t>(op.value);
        break;
      case 3:
        match = r.u64() == op.value;
        break;
      default: {
        uint32_t n = r.u32();
        auto s = r.bytes(n);
        match = n == op.blob.size() && std::equal(s.begin(), s.end(), op.blob.begin());
        break;
      }
    }
    if (!r.ok()) break;
    EXPECT_TRUE(match);
    ++good;
  }
  return good;
}

TEST(Bytes, RoundTripProperty) {
  testing::for_each_seed(1000, [](uint64_t seed) {
    Rng rng{seed};
    auto ops = random_script(rng);
    std::vector<uint8_t> buf;
    write_script(ops, buf);
    ByteReader r{buf};
    ASSERT_EQ(read_script(ops, r), ops.size());
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(r.at_end());
  });
}

// Every truncated prefix must fail cleanly: no out-of-bounds read (ASan
// checks that), and the error flag must be set.
TEST(Bytes, TruncatedInputFailsCleanly) {
  testing::for_each_seed(200, [](uint64_t seed) {
    Rng rng{seed};
    auto ops = random_script(rng);
    std::vector<uint8_t> buf;
    write_script(ops, buf);
    for (size_t len = 0; len < buf.size(); ++len) {
      // Copy into an exactly-sized buffer so ASan sees any overread.
      std::vector<uint8_t> prefix(buf.begin(), buf.begin() + static_cast<long>(len));
      ByteReader r{prefix};
      size_t good = read_script(ops, r);
      ASSERT_FALSE(r.ok()) << "len=" << len;
      ASSERT_LT(good, ops.size());
      // Once failed, further reads return zero and stay failed.
      ASSERT_EQ(r.u64(), 0u);
      ASSERT_TRUE(r.bytes(1).empty());
      ASSERT_FALSE(r.ok());
    }
  });
}

}  // namespace
}  // namespace raftdst
