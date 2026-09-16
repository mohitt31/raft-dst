#include "common/check.h"

#include <gtest/gtest.h>

namespace {

class CheckDeathTest : public ::testing::Test {
 protected:
  void SetUp() override { GTEST_FLAG_SET(death_test_style, "threadsafe"); }
};

TEST_F(CheckDeathTest, AssertAbortsWithMessage) {
  EXPECT_DEATH(RAFT_ASSERT(1 + 1 == 3, "arithmetic is broken"), "arithmetic is broken");
}

TEST(Check, AssertPassesQuietly) {
  RAFT_ASSERT(2 + 2 == 4, "unreachable");
  SUCCEED();
}

TEST(Check, AssertEvaluatesConditionOnce) {
  int calls = 0;
  RAFT_ASSERT(++calls == 1, "evaluated twice");
  EXPECT_EQ(calls, 1);
}

#ifdef RAFTDST_DCHECKS
TEST_F(CheckDeathTest, DcheckOnInCheckedBuilds) {
  EXPECT_DEATH(RAFT_DCHECK(false, "dcheck fired"), "dcheck fired");
}
#else
TEST(Check, DcheckDoesNotEvaluateInReleaseBuilds) {
  int calls = 0;
  RAFT_DCHECK(++calls == 1, "should not run");
  EXPECT_EQ(calls, 0);
}
#endif

}  // namespace
