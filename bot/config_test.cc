// The bot's configuration by example: which variables it reads and what it
// makes of them.

#include "bot/config.h"

#include <benchmark/benchmark.h>

#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "discord/model.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

using absl_testing::StatusIs;
using testing::ElementsAre;
using testing::HasSubstr;

// Variables that are exactly `values`.
bot::Variables From(std::map<std::string, std::string> values) {
  return [values = std::move(values)](
             const std::string& name) -> std::optional<std::string> {
    const auto found = values.find(name);
    if (found == values.end()) return std::nullopt;

    return found->second;
  };
}

// The least that will do: a token and the GM channel. The rest has defaults.
TEST(LoadConfigTest, NeedsATokenAndAChannel) {
  const absl::StatusOr<bot::Config> config = bot::LoadConfig(From({
      {"DISCORD_TOKEN", "secret"},
      {"GM_CHANNEL_IDS", "22"},
  }));

  ABSL_ASSERT_OK(config);
  EXPECT_EQ(config->token, "secret");
  EXPECT_THAT(config->channels, ElementsAre(discord::ChannelId{
                                    .value = 22,
                                }));
  EXPECT_EQ(config->time_zone, absl::UTCTimeZone());
  EXPECT_EQ(config->database, "./gm.db");
}

TEST(LoadConfigTest, ReadsEveryVariable) {
  const absl::StatusOr<bot::Config> config = bot::LoadConfig(From({
      {"DISCORD_TOKEN", "secret"},
      {"GM_CHANNEL_IDS", "1234567890123456789"},
      {"TIMEZONE", "America/New_York"},
      {"DATA_DIR", "/data"},
  }));

  ABSL_ASSERT_OK(config);
  EXPECT_THAT(config->channels, ElementsAre(discord::ChannelId{
                                    .value = 1234567890123456789,
                                }));
  EXPECT_EQ(config->time_zone.name(), "America/New_York");
  EXPECT_EQ(config->database, "/data/gm.db");
}

// One bot can serve several servers: a GM channel in each, separated by
// commas, with or without spaces.
TEST(LoadConfigTest, ReadsSeveralChannels) {
  const absl::StatusOr<bot::Config> config = bot::LoadConfig(From({
      {"DISCORD_TOKEN", "secret"},
      {"GM_CHANNEL_IDS", "22, 24,26"},
  }));

  ABSL_ASSERT_OK(config);
  EXPECT_THAT(config->channels, ElementsAre(
                                    discord::ChannelId{
                                        .value = 22,
                                    },
                                    discord::ChannelId{
                                        .value = 24,
                                    },
                                    discord::ChannelId{
                                        .value = 26,
                                    }));
}

// The variable used to be called GM_CHANNEL_ID, when there could be only one
// channel. An environment written then still works.
TEST(LoadConfigTest, StillReadsTheOlderName) {
  const absl::StatusOr<bot::Config> config = bot::LoadConfig(From({
      {"DISCORD_TOKEN", "secret"},
      {"GM_CHANNEL_ID", "22"},
  }));

  ABSL_ASSERT_OK(config);
  EXPECT_THAT(config->channels, ElementsAre(discord::ChannelId{
                                    .value = 22,
                                }));
}

// An environment file often lists a variable with nothing after the equals
// sign. That means "not set", not "set to nothing".
TEST(LoadConfigTest, TreatsEmptyAsUnset) {
  const absl::StatusOr<bot::Config> config = bot::LoadConfig(From({
      {"DISCORD_TOKEN", "secret"},
      {"GM_CHANNEL_IDS", "22"},
      {"TIMEZONE", ""},
      {"DATA_DIR", ""},
  }));

  ABSL_ASSERT_OK(config);
  EXPECT_EQ(config->time_zone, absl::UTCTimeZone());
  EXPECT_EQ(config->database, "./gm.db");
}

// The bot will not start without being told which channels are GM channels:
// it penalises whatever else is said where it watches, so "everywhere" is not
// a default anyone wants. An empty value is the same as none.
TEST(LoadConfigTest, RequiresTheGmChannel) {
  EXPECT_THAT(bot::LoadConfig(From({
                  {"DISCORD_TOKEN", "secret"},
              })),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("GM_CHANNEL_IDS is not set")));
  EXPECT_THAT(bot::LoadConfig(From({
                  {"DISCORD_TOKEN", "secret"},
                  {"GM_CHANNEL_IDS", ""},
              })),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("GM_CHANNEL_IDS is not set")));
}

// A mistake is reported by the name of the variable it is in.
TEST(LoadConfigTest, SaysWhichVariableIsWrong) {
  EXPECT_THAT(
      bot::LoadConfig(From({})),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("DISCORD_TOKEN")));
  EXPECT_THAT(bot::LoadConfig(From({
                  {"DISCORD_TOKEN", "secret"},
                  {"GM_CHANNEL_IDS", "22, #general"},
              })),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("GM_CHANNEL_IDS")));
  EXPECT_THAT(
      bot::LoadConfig(From({
          {"DISCORD_TOKEN", "secret"},
          {"GM_CHANNEL_IDS", "22"},
          {"TIMEZONE", "Mars/Olympus_Mons"},
      })),
      StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("TIMEZONE")));
}

// By default the variables are the process's own.
TEST(EnvironmentTest, ReadsTheProcessEnvironment) {
  // Bazel sets this one for every test.
  EXPECT_EQ(bot::Environment("TEST_TMPDIR"),
            std::string(std::getenv("TEST_TMPDIR")));
  EXPECT_EQ(bot::Environment("GM_BOT_NO_SUCH_VARIABLE"), std::nullopt);
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //bot:config_test -- --benchmark_filter=all
//
// clang-format off
// Results. Written by perf/record_results.py; do not edit by hand.
//
//   Date      2026-10-05
//   CPU       Intel(R) Core(TM) i7-9700 CPU @ 3.00GHz, 8 cores, L3 12 MiB (1 instance)
//   Memory    31 GB
//   Disk      Samsung SSD 990 EVO Plus 4TB (ext4)
//   System    Linux 7.0.0-38-generic, CPU governor: powersave
//   Compiler  clang version 21.1.8
//   Build     bazel -c opt (-O2), C++20, no exceptions
//   I/O       io_uring, except where a benchmark's name says epoll
//
//   --------------------------------------------------------
//   Benchmark              Time             CPU   Iterations
//   --------------------------------------------------------
//   BM_LoadConfig        358 ns          358 ns      1202207
// End of results.
// clang-format on

void BM_LoadConfig(benchmark::State& state) {
  const bot::Variables variables = From({
      {"DISCORD_TOKEN", "secret"},
      {"GM_CHANNEL_IDS", "2000000000000000002, 2000000000000000003"},
      {"TIMEZONE", "America/New_York"},
  });
  for (auto _ : state) benchmark::DoNotOptimize(bot::LoadConfig(variables));
}
BENCHMARK(BM_LoadConfig);

}  // namespace
