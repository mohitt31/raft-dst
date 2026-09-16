#include "common/types.h"

#include <gtest/gtest.h>

#include <concepts>
#include <type_traits>

namespace raftdst {
namespace {

template <class A, class B>
concept Addable = requires(A a, B b) { a + b; };

// The point of the strong types is what does NOT compile.
static_assert(!std::is_convertible_v<Term, Index>);
static_assert(!std::is_convertible_v<Index, Term>);
static_assert(!std::is_convertible_v<uint64_t, Term>);
static_assert(!std::is_convertible_v<uint64_t, Index>);
static_assert(!std::equality_comparable_with<Term, Index>);
static_assert(!Addable<Term, Index>);
static_assert(!Addable<Term, uint64_t>);
static_assert(!Addable<Time, Time>);
static_assert(!std::is_constructible_v<Time, int64_t>);
static_assert(!std::is_constructible_v<Duration, int64_t>);

// They must stay as cheap as the raw integers they wrap.
static_assert(std::is_trivially_copyable_v<Term> && sizeof(Term) == 8);
static_assert(std::is_trivially_copyable_v<Index> && sizeof(Index) == 8);
static_assert(std::is_trivially_copyable_v<Time> && sizeof(Time) == 8);
static_assert(std::is_trivially_copyable_v<NodeId> && sizeof(NodeId) == 1);

TEST(Types, TermOrdering) {
  Term t{3};
  EXPECT_EQ(t.next(), Term{4});
  EXPECT_LT(Term{2}, Term{3});
  EXPECT_EQ(Term{}.get(), 0u);
}

TEST(Types, IndexArithmetic) {
  Index i{10};
  EXPECT_EQ(i + 5, Index{15});
  EXPECT_EQ(i - 10, Index{0});
  EXPECT_EQ(i.next(), Index{11});
  EXPECT_EQ(i.prev(), Index{9});
  EXPECT_EQ(distance(Index{4}, Index{10}), 6u);
}

TEST(Types, NodeIdDefaultsToNone) {
  EXPECT_EQ(NodeId{}, NodeId::none());
  EXPECT_FALSE(NodeId::none().valid());
  EXPECT_TRUE(NodeId{0}.valid());
  EXPECT_TRUE(NodeId{kMaxNodes - 1}.valid());
  EXPECT_FALSE(NodeId{kMaxNodes}.valid());
}

TEST(Types, TimeAndDuration) {
  Time t0 = Time::zero();
  Time t1 = t0 + Duration::ms(150);
  EXPECT_EQ(t1 - t0, Duration::us(150'000));
  EXPECT_EQ((t1 - Duration::ms(50)).ns_since_epoch(), 100'000'000);
  EXPECT_EQ(Duration::sec(2), Duration::ms(1000) * 2);
  EXPECT_LT(t1, Time::max());
  EXPECT_LT(t0 - t1, Duration{});
}

}  // namespace
}  // namespace raftdst
