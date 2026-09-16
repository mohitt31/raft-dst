#pragma once

#include <compare>
#include <cstdint>
#include <limits>

namespace raftdst {

// Term and Index are both plain 64-bit counters in the paper. Passing one
// where the other belongs is an easy Raft bug to write and a hard one to
// spot, so each gets its own type with only the operations Raft needs.

class Term {
 public:
  constexpr Term() = default;
  constexpr explicit Term(uint64_t v) : v_(v) {}

  [[nodiscard]] constexpr uint64_t get() const { return v_; }
  [[nodiscard]] constexpr Term next() const { return Term{v_ + 1}; }

  friend constexpr bool operator==(Term, Term) = default;
  friend constexpr auto operator<=>(Term, Term) = default;

 private:
  uint64_t v_ = 0;
};

// Log index. Index 0 is the empty-log sentinel; real entries start at 1.
// Arithmetic wraps like uint64_t; callers check bounds where it matters.
class Index {
 public:
  constexpr Index() = default;
  constexpr explicit Index(uint64_t v) : v_(v) {}

  [[nodiscard]] constexpr uint64_t get() const { return v_; }
  [[nodiscard]] constexpr Index next() const { return Index{v_ + 1}; }
  [[nodiscard]] constexpr Index prev() const { return Index{v_ - 1}; }

  friend constexpr Index operator+(Index i, uint64_t n) { return Index{i.v_ + n}; }
  friend constexpr Index operator-(Index i, uint64_t n) { return Index{i.v_ - n}; }
  // Number of indices in [lo, hi). Requires lo <= hi.
  friend constexpr uint64_t distance(Index lo, Index hi) { return hi.v_ - lo.v_; }

  friend constexpr bool operator==(Index, Index) = default;
  friend constexpr auto operator<=>(Index, Index) = default;

 private:
  uint64_t v_ = 0;
};

// Static clusters of at most kMaxNodes, ids 0..kMaxNodes-1.
inline constexpr int kMaxNodes = 5;

class NodeId {
 public:
  constexpr NodeId() = default;
  constexpr explicit NodeId(uint8_t v) : v_(v) {}

  [[nodiscard]] static constexpr NodeId none() { return NodeId{}; }
  [[nodiscard]] constexpr bool valid() const { return v_ < kMaxNodes; }
  [[nodiscard]] constexpr uint8_t get() const { return v_; }

  friend constexpr bool operator==(NodeId, NodeId) = default;
  friend constexpr auto operator<=>(NodeId, NodeId) = default;

 private:
  uint8_t v_ = kNone;
  static constexpr uint8_t kNone = 0xFF;
};

// Signed nanoseconds. Virtual time in the simulator, CLOCK_MONOTONIC on real
// nodes. int64 nanoseconds covers about 292 years.
class Duration {
 public:
  constexpr Duration() = default;

  [[nodiscard]] static constexpr Duration ns(int64_t v) { return Duration{v}; }
  [[nodiscard]] static constexpr Duration us(int64_t v) { return Duration{v * 1'000}; }
  [[nodiscard]] static constexpr Duration ms(int64_t v) { return Duration{v * 1'000'000}; }
  [[nodiscard]] static constexpr Duration sec(int64_t v) { return Duration{v * 1'000'000'000}; }

  [[nodiscard]] constexpr int64_t count_ns() const { return v_; }

  friend constexpr Duration operator+(Duration a, Duration b) { return Duration{a.v_ + b.v_}; }
  friend constexpr Duration operator-(Duration a, Duration b) { return Duration{a.v_ - b.v_}; }
  friend constexpr Duration operator*(Duration a, int64_t k) { return Duration{a.v_ * k}; }

  friend constexpr bool operator==(Duration, Duration) = default;
  friend constexpr auto operator<=>(Duration, Duration) = default;

 private:
  constexpr explicit Duration(int64_t v) : v_(v) {}
  int64_t v_ = 0;
};

class Time {
 public:
  constexpr Time() = default;

  [[nodiscard]] static constexpr Time from_ns(int64_t v) { return Time{v}; }
  [[nodiscard]] static constexpr Time zero() { return Time{0}; }
  [[nodiscard]] static constexpr Time max() { return Time{std::numeric_limits<int64_t>::max()}; }

  [[nodiscard]] constexpr int64_t ns_since_epoch() const { return v_; }

  friend constexpr Time operator+(Time t, Duration d) { return Time{t.v_ + d.count_ns()}; }
  friend constexpr Time operator-(Time t, Duration d) { return Time{t.v_ - d.count_ns()}; }
  friend constexpr Duration operator-(Time a, Time b) { return Duration::ns(a.v_ - b.v_); }

  friend constexpr bool operator==(Time, Time) = default;
  friend constexpr auto operator<=>(Time, Time) = default;

 private:
  constexpr explicit Time(int64_t v) : v_(v) {}
  int64_t v_ = 0;
};

}  // namespace raftdst
