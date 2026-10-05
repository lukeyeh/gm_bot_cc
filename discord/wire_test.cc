// How Discord's JSON becomes the model, by example.

#include "discord/wire.h"

#include <benchmark/benchmark.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "discord/model.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "json/json.h"

namespace {

using absl_testing::StatusIs;
using discord_internal::ParseEvent;
using discord_internal::ParseMessage;
using discord_internal::ParseUser;

json::Value Json(std::string_view text) {
  const absl::StatusOr<json::Value> value = json::Parse(text);
  ABSL_EXPECT_OK(value);
  return value.value_or(json::Value());
}

// A message as Discord sends it, with far more fields than the model keeps.
constexpr std::string_view kMessage = R"({
  "id": "1100000000000000001",
  "channel_id": "2200000000000000002",
  "guild_id": "3300000000000000003",
  "content": "gm everyone",
  "timestamp": "2026-10-05T12:00:00.000000+00:00",
  "author": {
    "id": "4400000000000000004",
    "username": "lukeyeh",
    "global_name": "Luke",
    "avatar": null
  },
  "member": {"nick": "luke the early", "roles": []},
  "mentions": [],
  "type": 0
})";

TEST(ParseMessageTest, ReadsWhatTheModelKeeps) {
  const absl::StatusOr<discord::Message> message = ParseMessage(Json(kMessage));

  ABSL_ASSERT_OK(message);
  EXPECT_EQ(message->id.value, 1100000000000000001);
  EXPECT_EQ(message->channel.value, 2200000000000000002);
  EXPECT_EQ(message->guild.value, 3300000000000000003);
  EXPECT_EQ(message->content, "gm everyone");
  EXPECT_EQ(message->author.id.value, 4400000000000000004);
  EXPECT_EQ(message->author.name, "luke the early");
  EXPECT_FALSE(message->author.bot);
}

// What cannot be acted on is rejected rather than half-read.
TEST(ParseMessageTest, RejectsAMessageThatCannotBeIdentified) {
  EXPECT_THAT(ParseMessage(Json(R"({"content": "gm"})")),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(ParseMessage(Json(R"({"id": "1", "channel_id": "x",
                                    "author": {"id": "2"}})")),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// The name is the most specific one there is: server nickname, then display
// name, then username.
TEST(ParseUserTest, PrefersTheMostSpecificName) {
  const json::Value user =
      Json(R"({"id": "1", "username": "lukeyeh", "global_name": "Luke"})");

  EXPECT_EQ(ParseUser(user, Json(R"({"nick": "early bird"})")).name,
            "early bird");
  EXPECT_EQ(ParseUser(user, Json(R"({"nick": null})")).name, "Luke");
  EXPECT_EQ(ParseUser(user).name, "Luke");
  EXPECT_EQ(ParseUser(Json(R"({"id": "1", "username": "lukeyeh"})")).name,
            "lukeyeh");
}

TEST(ParseUserTest, KnowsBotsFromPeople) {
  EXPECT_TRUE(ParseUser(Json(R"({"id": "1", "bot": true})")).bot);
  EXPECT_FALSE(ParseUser(Json(R"({"id": "1"})")).bot);
}

// A new message is the one kind of event reported so far.
TEST(ParseEventTest, ReportsNewMessages) {
  const std::optional<discord::Event> event =
      ParseEvent("MESSAGE_CREATE", Json(kMessage));

  ASSERT_TRUE(event.has_value());
  if (event.has_value()) {
    EXPECT_EQ(std::get<discord::MessageCreated>(*event).message.content,
              "gm everyone");
  }
}

// A reply is a message like any other. A notice that someone pinned a
// message is not, though Discord sends it the same way with the pinner as
// its author.
TEST(ParseEventTest, ReportsRepliesButNotSystemNotices) {
  const auto with_type = [](int type) {
    json::Value message = Json(kMessage);
    message.Set("type", type);
    return message;
  };

  EXPECT_TRUE(ParseEvent("MESSAGE_CREATE", with_type(19)).has_value());
  EXPECT_FALSE(ParseEvent("MESSAGE_CREATE", with_type(6)).has_value());
  EXPECT_FALSE(ParseEvent("MESSAGE_CREATE", with_type(7)).has_value());
}

// Everything else Discord sends, and anything malformed, is nothing to
// report: the bot must not stop because of one odd event.
TEST(ParseEventTest, ReportsNothingForOtherOrMalformedEvents) {
  EXPECT_FALSE(ParseEvent("TYPING_START", Json(kMessage)).has_value());
  EXPECT_FALSE(ParseEvent("MESSAGE_CREATE", Json("{}")).has_value());
}

// A slash command used in a server, as the gateway reports it.
constexpr std::string_view kInteraction = R"({
  "id": "5500000000000000005",
  "application_id": "9900000000000000009",
  "type": 2,
  "token": "aW50ZXJhY3Rpb24",
  "channel_id": "2200000000000000002",
  "guild_id": "3300000000000000003",
  "data": {
    "id": "6600000000000000006",
    "name": "leaderboard",
    "type": 1,
    "options": [
      {"name": "limit", "type": 4, "value": 5},
      {"name": "title", "type": 3, "value": "this week"}
    ]
  },
  "member": {
    "nick": "luke the early",
    "user": {"id": "4400000000000000004", "username": "lukeyeh"}
  }
})";

// Someone using a command becomes a CommandInvoked: which command, who, where,
// with what filled in, and the handle needed to answer.
TEST(ParseEventTest, ReportsCommandsBeingUsed) {
  const std::optional<discord::Event> event =
      ParseEvent("INTERACTION_CREATE", Json(kInteraction));

  ASSERT_TRUE(event.has_value());
  if (!event.has_value()) return;
  const auto& invoked = std::get<discord::CommandInvoked>(*event);
  EXPECT_EQ(invoked.name, "leaderboard");
  EXPECT_EQ(invoked.user.id.value, 4400000000000000004);
  EXPECT_EQ(invoked.user.name, "luke the early");
  EXPECT_EQ(invoked.channel.value, 2200000000000000002);
  EXPECT_EQ(invoked.guild.value, 3300000000000000003);
  EXPECT_EQ(invoked.Integer("limit", 10), 5);
  ASSERT_EQ(invoked.options.size(), 2);
  EXPECT_EQ(std::get<std::string>(invoked.options[1].value), "this week");
  EXPECT_EQ(invoked.id.value, 5500000000000000005);
  EXPECT_EQ(invoked.token, "aW50ZXJhY3Rpb24");
}

// In a direct message there is no server membership: the user comes bare.
TEST(ParseEventTest, ReadsACommandUsedInADirectMessage) {
  const std::optional<discord::Event> event =
      ParseEvent("INTERACTION_CREATE",
                 Json(R"({"id": "5", "type": 2, "token": "t", "channel_id": "2",
               "data": {"name": "streak"},
               "user": {"id": "4", "username": "lukeyeh"}})"));

  ASSERT_TRUE(event.has_value());
  if (!event.has_value()) return;
  const auto& invoked = std::get<discord::CommandInvoked>(*event);
  EXPECT_EQ(invoked.name, "streak");
  EXPECT_EQ(invoked.user.name, "lukeyeh");
  EXPECT_EQ(invoked.guild.value, 0);  // No server.
  EXPECT_TRUE(invoked.options.empty());
}

// Button presses and the like are interactions too, but not commands; and a
// command that cannot be answered is not worth reporting.
TEST(ParseEventTest, ReportsNothingForOtherOrMalformedInteractions) {
  EXPECT_FALSE(ParseEvent("INTERACTION_CREATE",
                          Json(R"({"id": "5", "type": 3, "token": "t",
                                   "data": {"name": "streak"}})"))
                   .has_value());
  EXPECT_FALSE(ParseEvent("INTERACTION_CREATE", Json(R"({"id": "5", "type": 2,
                                   "data": {"name": "streak"}})"))
                   .has_value());
}

// What registering commands sends: each as a slash command, with its options
// typed by Discord's numbers.
TEST(FormatCommandsTest, DescribesCommandsAsDiscordExpects) {
  const std::vector<discord::Command> commands = {
      discord::Command{
          .name = "leaderboard",
          .description = "Show the GM leaderboard",
          .options =
              {
                  discord::Option{
                      .name = "limit",
                      .description = "How many to show",
                      .type = discord::OptionType::kInteger,
                      .required = false,
                  },
                  discord::Option{
                      .name = "title",
                      .description = "What to call it",
                      .type = discord::OptionType::kText,
                      .required = true,
                  },
              },
      },
      discord::Command{
          .name = "streak",
          .description = "Check your streak",
      },
  };

  EXPECT_EQ(
      json::Serialize(discord_internal::FormatCommands(commands)),
      R"([{"type":1,"name":"leaderboard","description":"Show the GM leaderboard",)"
      R"("options":[)"
      R"({"name":"limit","description":"How many to show","type":4,"required":false},)"
      R"({"name":"title","description":"What to call it","type":3,"required":true}]},)"
      R"({"type":1,"name":"streak","description":"Check your streak","options":[]}])");
}

TEST(ParseGuildOfChannelTest, ReadsTheServerOrNone) {
  EXPECT_EQ(discord_internal::ParseGuildOfChannel(
                Json(R"({"id": "22", "guild_id": "33"})"))
                .value,
            33);
  // A direct message's channel has none.
  EXPECT_EQ(
      discord_internal::ParseGuildOfChannel(Json(R"({"id": "22"})")).value, 0);
}

// A command for administrators says so in the permissions it asks for.
TEST(FormatCommandsTest, MarksCommandsForAdministrators) {
  const std::vector<discord::Command> commands = {
      discord::Command{
          .name = "gmadd",
          .description = "Add a phrase",
          .administrators_only = true,
      },
  };

  EXPECT_EQ(json::Serialize(discord_internal::FormatCommands(commands)),
            R"([{"type":1,"name":"gmadd","description":"Add a phrase",)"
            R"("options":[],"default_member_permissions":"8"}])");
}

// READY says which application the bot is; its user id stands in if not.
TEST(ParseApplicationIdTest, PrefersWhatReadySays) {
  const discord::User self = ParseUser(Json(R"({"id": "99"})"));

  EXPECT_EQ(discord_internal::ParseApplicationId(
                Json(R"({"application": {"id": "77", "flags": 0}})"), self)
                .value,
            77);
  EXPECT_EQ(discord_internal::ParseApplicationId(Json("{}"), self).value, 99);
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //discord:wire_test -- --benchmark_filter=all
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
//   ---------------------------------------------------------------
//   Benchmark                     Time             CPU   Iterations
//   ---------------------------------------------------------------
//   BM_ParseMessageEvent        200 ns          200 ns      2031880
//   BM_ParseCommandEvent        333 ns          333 ns      1282533
// End of results.
// clang-format on

// Turning a parsed dispatch into the event the bot sees.
void BM_ParseMessageEvent(benchmark::State& state) {
  const json::Value message = Json(kMessage);
  for (auto _ : state) {
    benchmark::DoNotOptimize(ParseEvent("MESSAGE_CREATE", message));
  }
}
BENCHMARK(BM_ParseMessageEvent);

void BM_ParseCommandEvent(benchmark::State& state) {
  const json::Value interaction = Json(kInteraction);
  for (auto _ : state) {
    benchmark::DoNotOptimize(ParseEvent("INTERACTION_CREATE", interaction));
  }
}
BENCHMARK(BM_ParseCommandEvent);

}  // namespace
