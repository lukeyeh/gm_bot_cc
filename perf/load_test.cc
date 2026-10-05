// The load tests, run small: they are measuring instruments, and this checks
// that they measure what they say. For numbers, run //perf:bench.

#include "perf/load.h"

#include <chrono>

#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "gtest/gtest.h"

namespace {

// Every event sent is received, and each has a latency.
TEST(WebSocketLoadTest, ReceivesEveryEvent) {
  const absl::StatusOr<LoadResult> result = RunWebSocketLoad(LoadOptions{
      .events = 300,
  });

  ABSL_ASSERT_OK(result);
  EXPECT_EQ(result->handled, 300);
  EXPECT_EQ(result->latency.count(), 300);
  EXPECT_GT(result->events_per_second(), 0);
}

// Pacing is honoured: events sent a millisecond apart take at least that
// long in all.
TEST(WebSocketLoadTest, PacesEventsWhenAsked) {
  const absl::StatusOr<LoadResult> result = RunWebSocketLoad(LoadOptions{
      .events = 20,
      .interval = std::chrono::milliseconds(1),
  });

  ABSL_ASSERT_OK(result);
  EXPECT_EQ(result->handled, 20);
  EXPECT_GE(result->elapsed, std::chrono::milliseconds(19));
}

// The bot answers every event of the morning: a reaction to each message,
// a reply to each command.
TEST(BotLoadTest, AnswersEveryEvent) {
  const absl::StatusOr<LoadResult> result = RunBotLoad(LoadOptions{
      .events = 120,
      .members = 20,
      .history_days = 2,
  });

  ABSL_ASSERT_OK(result);
  EXPECT_EQ(result->handled, 120);
  EXPECT_EQ(result->latency.count(), 120);
}

}  // namespace
