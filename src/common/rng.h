#pragma once

#include <cstdint>

#include "common/check.h"

namespace raftdst {

// All randomness in the simulator comes from these generators. Nothing here
// depends on the standard library's distributions, whose output differs
// between libc++ and libstdc++.

constexpr uint64_t rotl64(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

// SplitMix64 (Steele, Lea, Flood). Used to expand a seed into xoshiro state
// and to derive independent streams.
class SplitMix64 {
 public:
  constexpr explicit SplitMix64(uint64_t seed) : x_(seed) {}

  constexpr uint64_t next() {
    uint64_t z = (x_ += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }

 private:
  uint64_t x_;
};

namespace detail {

__extension__ using u128 = unsigned __int128;

// Lemire's nearly divisionless bounded draw, written over a word type so the
// tests can check the rejection rule exhaustively on 8-bit words.
template <class U, class W, class Next>
constexpr U lemire_bounded(Next&& next, U n) {
  const int bits = sizeof(U) * 8;
  U x = next();
  W m = static_cast<W>(x) * static_cast<W>(n);
  U low = static_cast<U>(m);
  if (low < n) {
    const U threshold = static_cast<U>(static_cast<U>(U{0} - n) % n);
    while (low < threshold) {
      x = next();
      m = static_cast<W>(x) * static_cast<W>(n);
      low = static_cast<U>(m);
    }
  }
  return static_cast<U>(m >> bits);
}

}  // namespace detail

// xoshiro256** (Blackman, Vigna). Small state, fast, good statistical quality.
class Rng {
 public:
  constexpr explicit Rng(uint64_t seed) {
    SplitMix64 sm{seed};
    for (auto& w : s_) w = sm.next();
  }

  // Independent stream `stream` of master seed `seed`. Changing how often one
  // stream is used never shifts another stream's values.
  static constexpr Rng derive(uint64_t seed, uint64_t stream) {
    SplitMix64 a{seed};
    SplitMix64 b{stream ^ 0x5851f42d4c957f2dULL};
    return Rng{a.next() ^ rotl64(b.next(), 17)};
  }

  constexpr uint64_t next() {
    const uint64_t result = rotl64(s_[1] * 5, 7) * 9;
    const uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl64(s_[3], 45);
    return result;
  }

  // Uniform in [0, n). n must be > 0.
  constexpr uint64_t below(uint64_t n) {
    RAFT_ASSERT(n > 0, "Rng::below(0)");
    return detail::lemire_bounded<uint64_t, detail::u128>([this] { return next(); }, n);
  }

  // Uniform in [lo, hi], inclusive.
  constexpr int64_t between(int64_t lo, int64_t hi) {
    RAFT_ASSERT(lo <= hi, "Rng::between with lo > hi");
    const uint64_t span = static_cast<uint64_t>(hi) - static_cast<uint64_t>(lo);
    const uint64_t off = span == UINT64_MAX ? next() : below(span + 1);
    return static_cast<int64_t>(static_cast<uint64_t>(lo) + off);
  }

  // True with probability ppm / 1'000'000. Integer only, no floating point.
  constexpr bool chance_ppm(uint32_t ppm) {
    if (ppm == 0) return false;
    if (ppm >= kMillion) return true;
    return below(kMillion) < ppm;
  }

  static constexpr uint32_t kMillion = 1'000'000;

 private:
  uint64_t s_[4]{};
};

}  // namespace raftdst
