#include "raft/codec.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "common/bytes.h"
#include "common/entry_log.h"
#include "common/rng.h"
#include "support/property.h"

namespace raftdst::raft {
namespace {

constexpr uint8_t kN = 5;

// Random valid message. AppendEntries entries are a slice of `log`, which
// must outlive the message.
Message random_message(Rng& rng, EntryLog& log) {
  Message m;
  m.from = NodeId{static_cast<uint8_t>(rng.below(kN))};
  m.to = NodeId{static_cast<uint8_t>((m.from.get() + 1 + rng.below(kN - 1)) % kN)};

  log = EntryLog{};
  const uint64_t len = rng.below(12);
  Term t{1};
  for (uint64_t i = 0; i < len; ++i) {
    t = Term{t.get() + rng.below(2)};
    std::vector<uint8_t> p(rng.below(3) == 0 ? 0 : rng.below(40));
    for (auto& b : p) b = static_cast<uint8_t>(rng.next());
    log.append(t, p);
  }
  const Term top{log.last_term().get() + rng.below(3) + 1};
  m.term = top;
  const Index pos{rng.below(log.last_index().get() + 1)};

  switch (rng.below(4)) {
    case 0:
      m.type = MsgType::kRequestVote;
      m.last_log_index = pos;
      m.last_log_term = log.term_at(pos);
      break;
    case 1:
      m.type = MsgType::kRequestVoteResp;
      m.ok = rng.below(2) == 1;
      break;
    case 2: {
      m.type = MsgType::kAppendEntries;
      m.prev_index = pos;
      m.prev_term = log.term_at(pos);
      m.commit = Index{rng.below(100)};
      m.entries = EntriesView::from_log(log.slice(pos.next(), rng.below(8), SIZE_MAX));
      break;
    }
    default:
      m.type = MsgType::kAppendEntriesResp;
      m.ok = rng.below(2) == 1;
      if (m.ok) {
        m.index = Index{rng.below(1000)};
      } else {
        m.index = Index{1 + rng.below(1000)};
        m.hint_index = Index{1 + rng.below(1000)};
        m.hint_term = Term{rng.below(top.get() + 1)};
      }
      break;
  }
  return m;
}

// Field-by-field equality, entries compared by content.
void expect_same(const Message& a, const Message& b) {
  EXPECT_EQ(a.type, b.type);
  EXPECT_EQ(a.from, b.from);
  EXPECT_EQ(a.to, b.to);
  EXPECT_EQ(a.term, b.term);
  EXPECT_EQ(a.last_log_index, b.last_log_index);
  EXPECT_EQ(a.last_log_term, b.last_log_term);
  EXPECT_EQ(a.prev_index, b.prev_index);
  EXPECT_EQ(a.prev_term, b.prev_term);
  EXPECT_EQ(a.commit, b.commit);
  EXPECT_EQ(a.ok, b.ok);
  EXPECT_EQ(a.index, b.index);
  EXPECT_EQ(a.hint_index, b.hint_index);
  EXPECT_EQ(a.hint_term, b.hint_term);
  ASSERT_EQ(a.entries.size(), b.entries.size());
  std::vector<std::vector<uint8_t>> pa, pb;
  a.entries.for_each_from(
      0, [&](Term, std::span<const uint8_t> p) { pa.emplace_back(p.begin(), p.end()); });
  b.entries.for_each_from(
      0, [&](Term, std::span<const uint8_t> p) { pb.emplace_back(p.begin(), p.end()); });
  EXPECT_EQ(pa, pb);
  for (uint64_t i = 0; i < a.entries.size(); ++i) {
    EXPECT_EQ(a.entries.term(i), b.entries.term(i));
    EXPECT_EQ(a.entries.payload_size(i), b.entries.payload_size(i));
  }
}

// Differential: the log-backed and wire-backed EntriesView are two
// implementations of the same interface and must agree on every accessor.
TEST(Codec, RoundTripIsExactAndCanonical) {
  uint64_t by_type[5] = {};
  testing::for_each_seed(3000, [&](uint64_t seed) {
    Rng rng{seed};
    EntryLog log;
    const Message m = random_message(rng, log);
    ASSERT_EQ(validate(m, kN), DecodeError::kOk);
    ++by_type[static_cast<int>(m.type)];

    std::vector<uint8_t> bytes;
    encode(m, bytes);
    Message d;
    ASSERT_EQ(decode(bytes, kN, d), DecodeError::kOk);
    expect_same(m, d);

    std::vector<uint8_t> again;
    encode(d, again);
    ASSERT_EQ(bytes, again);
    if (m.type == MsgType::kAppendEntries) {
      for (uint64_t k = 0; k <= m.entries.size(); ++k) {
        size_t n_orig = 0, n_wire = 0;
        m.entries.for_each_from(k, [&](Term, std::span<const uint8_t> p) { n_orig += p.size(); });
        d.entries.for_each_from(k, [&](Term, std::span<const uint8_t> p) { n_wire += p.size(); });
        ASSERT_EQ(n_orig, n_wire);
      }
    }
  });
  for (int t = 1; t <= 4; ++t) EXPECT_GT(by_type[t], 500u) << "type " << t;
}

TEST(Codec, EveryTruncatedPrefixIsRejected) {
  testing::for_each_seed(300, [](uint64_t seed) {
    Rng rng{seed};
    EntryLog log;
    std::vector<uint8_t> bytes;
    encode(random_message(rng, log), bytes);
    for (size_t len = 0; len < bytes.size(); ++len) {
      std::vector<uint8_t> prefix(bytes.begin(), bytes.begin() + static_cast<long>(len));
      Message d;
      ASSERT_EQ(decode(prefix, kN, d), DecodeError::kTruncated) << "len=" << len;
      ASSERT_EQ(probe_frame(prefix).size, 0u);
    }
  });
}

// Frames that follow each type's field layout but carry small random values
// in every field, sometimes with a wrong type byte or a byte too many or too
// few. Nothing may crash or read out of bounds (ASan/UBSan builds), and
// anything accepted must re-encode to the same bytes.
TEST(Codec, RandomFramesNeverCrashAndAcceptedOnesAreCanonical) {
  uint64_t accepted = 0, total = 0;
  testing::for_each_seed(20000, [&](uint64_t seed) {
    Rng rng{seed};
    auto small = [&]() -> uint64_t { return rng.below(8) == 0 ? rng.next() : rng.below(4); };
    std::vector<uint8_t> f;
    ByteWriter w{f};
    w.u32(0);
    w.u8(rng.below(16) == 0 ? static_cast<uint8_t>(small()) : kWireVersion);
    const auto type = static_cast<uint8_t>(rng.below(6));
    w.u8(type);
    w.u8(static_cast<uint8_t>(small()));
    w.u8(static_cast<uint8_t>(small()));
    w.u64(small());
    switch (type) {
      case 1:
        w.u64(small());
        w.u64(small());
        break;
      case 2:
        w.u8(static_cast<uint8_t>(small()));
        break;
      case 3: {
        w.u64(small());
        w.u64(small());
        w.u64(small());
        const uint32_t count = static_cast<uint32_t>(rng.below(4));
        w.u32(count);
        uint64_t blob = 0;
        for (uint32_t i = 0; i < count; ++i) {
          const uint32_t len = static_cast<uint32_t>(rng.below(5));
          w.u64(small());
          w.u32(len);
          blob += len;
        }
        for (uint64_t i = 0; i < blob; ++i) w.u8(static_cast<uint8_t>(rng.next()));
        break;
      }
      case 4:
        w.u8(static_cast<uint8_t>(small()));
        w.u64(small());
        w.u64(small());
        w.u64(small());
        break;
      default:
        for (uint64_t i = rng.below(20); i > 0; --i) w.u8(static_cast<uint8_t>(rng.next()));
    }
    if (rng.below(10) == 0) f.push_back(0);
    if (rng.below(10) == 0 && f.size() > 16) f.pop_back();
    w.patch_u32(0, static_cast<uint32_t>(f.size() - 4));

    ++total;
    Message d;
    if (decode(f, kN, d) != DecodeError::kOk) return;
    ++accepted;
    std::vector<uint8_t> again;
    encode(d, again);
    ASSERT_EQ(f, again);
  });
  // The property must not hold vacuously, and must not accept almost everything.
  EXPECT_GT(accepted, total / 50);
  EXPECT_LT(accepted, total / 2);
}

TEST(Codec, MutatedValidFramesNeverCrashAndAcceptedOnesAreCanonical) {
  uint64_t rejected = 0, accepted = 0;
  testing::for_each_seed(5000, [&](uint64_t seed) {
    Rng rng{seed};
    EntryLog log;
    std::vector<uint8_t> bytes;
    encode(random_message(rng, log), bytes);
    const uint64_t flips = 1 + rng.below(3);
    for (uint64_t i = 0; i < flips; ++i) {
      bytes[rng.below(bytes.size())] ^= static_cast<uint8_t>(1u << rng.below(8));
    }
    Message d;
    if (decode(bytes, kN, d) != DecodeError::kOk) {
      ++rejected;
      return;
    }
    ++accepted;
    std::vector<uint8_t> again;
    encode(d, again);
    ASSERT_EQ(bytes, again);
  });
  EXPECT_GT(rejected, 1000u);
  EXPECT_GT(accepted, 100u);
}

// Byte offsets in a frame, for building specific invalid inputs.
constexpr size_t kOffType = 5, kOffFrom = 6, kOffTo = 7, kOffTerm = 8;
constexpr size_t kOffAePrevIndex = 16, kOffAePrevTerm = 24, kOffAeCount = 40, kOffAeHeaders = 44;

void put_u64(std::vector<uint8_t>& b, size_t at, uint64_t v) {
  for (size_t i = 0; i < 8; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

std::vector<uint8_t> append_entries_frame(EntryLog& log) {
  log = EntryLog{};
  log.append(Term{2}, std::vector<uint8_t>{1, 2, 3});
  log.append(Term{2}, std::vector<uint8_t>{4});
  log.append(Term{3}, std::vector<uint8_t>{});
  Message m;
  m.type = MsgType::kAppendEntries;
  m.from = NodeId{0};
  m.to = NodeId{1};
  m.term = Term{4};
  m.prev_index = Index{1};
  m.prev_term = Term{2};
  m.commit = Index{1};
  m.entries = EntriesView::from_log(log.slice(Index{2}, 10, SIZE_MAX));
  std::vector<uint8_t> out;
  encode(m, out);
  return out;
}

DecodeError decode_bytes(const std::vector<uint8_t>& b, uint8_t n = 3) {
  Message d;
  return decode(b, n, d);
}

TEST(Codec, RejectsSpecificInvalidFrames) {
  EntryLog log;
  const auto good = append_entries_frame(log);
  ASSERT_EQ(decode_bytes(good), DecodeError::kOk);

  auto b = good;
  b[4] = 2;
  EXPECT_EQ(decode_bytes(b), DecodeError::kBadVersion);

  b = good;
  b[kOffType] = 9;
  EXPECT_EQ(decode_bytes(b), DecodeError::kBadType);

  b = good;
  b[kOffTo] = b[kOffFrom];
  EXPECT_EQ(decode_bytes(b), DecodeError::kBadNode);

  b = good;
  b[kOffTo] = 3;  // cluster of 3 has ids 0..2
  EXPECT_EQ(decode_bytes(b), DecodeError::kBadNode);
  EXPECT_EQ(decode_bytes(b, 4), DecodeError::kOk);

  b = good;
  put_u64(b, kOffTerm, 0);
  EXPECT_EQ(decode_bytes(b), DecodeError::kBadTerm);

  b = good;
  put_u64(b, kOffAePrevTerm, 0);  // index 1 with term 0
  EXPECT_EQ(decode_bytes(b), DecodeError::kBadLogPosition);

  b = good;
  put_u64(b, kOffAePrevIndex, 0);  // index 0 with term 2
  EXPECT_EQ(decode_bytes(b), DecodeError::kBadLogPosition);

  b = good;
  put_u64(b, kOffAeHeaders + 12, 1);  // second entry term drops 2 -> 1
  EXPECT_EQ(decode_bytes(b), DecodeError::kBadEntries);

  b = good;
  put_u64(b, kOffAeHeaders, 5);  // entry term above the message term
  EXPECT_EQ(decode_bytes(b), DecodeError::kBadEntries);

  b = good;
  b[kOffAeCount] = 200;  // claims far more headers than the frame holds
  EXPECT_EQ(decode_bytes(b), DecodeError::kTruncated);

  b = good;
  b.push_back(0);
  EXPECT_EQ(decode_bytes(b), DecodeError::kTrailingBytes);

  b = good;
  b[0] = 0xff;
  b[1] = 0xff;
  b[2] = 0xff;
  b[3] = 0x7f;
  EXPECT_EQ(decode_bytes(b), DecodeError::kTooLarge);
  EXPECT_EQ(probe_frame(b).error, DecodeError::kTooLarge);
}

TEST(Codec, RejectsNonCanonicalBooleansAndHints) {
  Message m;
  m.type = MsgType::kAppendEntriesResp;
  m.from = NodeId{1};
  m.to = NodeId{0};
  m.term = Term{3};
  m.ok = true;
  m.index = Index{7};
  std::vector<uint8_t> b;
  encode(m, b);
  ASSERT_EQ(decode_bytes(b), DecodeError::kOk);

  auto bad = b;
  bad[16] = 2;
  EXPECT_EQ(decode_bytes(bad), DecodeError::kBadBool);

  bad = b;
  put_u64(bad, 25, 4);  // success with a hint index
  EXPECT_EQ(decode_bytes(bad), DecodeError::kBadResponse);

  bad = b;
  bad[16] = 0;  // rejection needs index >= 1 and a hint index >= 1
  EXPECT_EQ(decode_bytes(bad), DecodeError::kBadResponse);
}

// Frames concatenated into one byte stream and fed in random chunk sizes, as
// TCP would deliver them.
TEST(Codec, StreamFramingSurvivesArbitraryChunking) {
  testing::for_each_seed(200, [](uint64_t seed) {
    Rng rng{seed};
    std::vector<EntryLog> logs(1 + rng.below(20));
    std::vector<Message> sent;
    std::vector<uint8_t> stream;
    for (auto& log : logs) {
      sent.push_back(random_message(rng, log));
      encode(sent.back(), stream);
    }

    std::vector<uint8_t> buf;
    std::vector<std::vector<uint8_t>> frames;
    size_t pos = 0;
    while (pos < stream.size()) {
      const size_t chunk = std::min<size_t>(1 + rng.below(40), stream.size() - pos);
      buf.insert(buf.end(), stream.begin() + static_cast<long>(pos),
                 stream.begin() + static_cast<long>(pos + chunk));
      pos += chunk;
      for (;;) {
        FrameProbe p = probe_frame(buf);
        ASSERT_EQ(p.error, DecodeError::kOk);
        if (p.size == 0) break;
        frames.emplace_back(buf.begin(), buf.begin() + static_cast<long>(p.size));
        buf.erase(buf.begin(), buf.begin() + static_cast<long>(p.size));
      }
    }
    ASSERT_TRUE(buf.empty());
    ASSERT_EQ(frames.size(), sent.size());
    for (size_t i = 0; i < frames.size(); ++i) {
      Message d;
      ASSERT_EQ(decode(frames[i], kN, d), DecodeError::kOk);
      expect_same(sent[i], d);
    }
  });
}

}  // namespace
}  // namespace raftdst::raft
