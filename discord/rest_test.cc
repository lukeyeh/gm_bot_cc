// Discord's HTTP API by example: what each call sends, and what Discord's
// answers turn into.

#include "discord/rest.h"

#include <benchmark/benchmark.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "discord/model.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "http/client.h"
#include "http/fake_client.h"
#include "http/head.h"
#include "net/event_loop.h"

namespace {

using absl_testing::IsOkAndHolds;
using absl_testing::StatusIs;
using discord_internal::Rest;
using testing::HasSubstr;

void RunOnEventLoop(Task<> test) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(std::move(test));
}

constexpr discord::ChannelId kChannel{
    .value = 22,
};
constexpr discord::MessageId kMessage{
    .value = 11,
};

http::Response Answer(int status, std::string body) {
  return http::Response{
      .status = status,
      .body = std::move(body),
  };
}

// Posting a message: where it goes, how the bot proves who it is, and that
// the text is sent as JSON whatever it contains.
TEST(RestTest, CreateMessagePostsJsonToTheChannel) {
  RunOnEventLoop([]() -> Task<> {
    http::FakeClient http;
    http.Answer(Answer(200, R"({"id": "33"})"));
    Rest rest(&http, "secret-token");

    ABSL_EXPECT_OK(co_await rest.CreateMessage(kChannel, "gm \"all\" 🌅"));

    const http::Request& request = http.requests().front();
    EXPECT_EQ(request.method, http::Method::kPost);
    EXPECT_EQ(request.url, "https://discord.com/api/v10/channels/22/messages");
    EXPECT_EQ(request.body, R"({"content":"gm \"all\" 🌅"})");
    EXPECT_EQ(http::FindHeader(request.headers, "Authorization"),
              "Bot secret-token");
    EXPECT_EQ(http::FindHeader(request.headers, "Content-Type"),
              "application/json");
    EXPECT_THAT(http::FindHeader(request.headers, "User-Agent"),
                HasSubstr("DiscordBot"));
  }());
}

// A reaction is a PUT with no body; the emoji travels in the path, encoded.
TEST(RestTest, AddReactionPutsTheEncodedEmoji) {
  RunOnEventLoop([]() -> Task<> {
    http::FakeClient http;
    Rest rest(&http, "secret-token");

    ABSL_EXPECT_OK(co_await rest.AddReaction(kChannel, kMessage, "🌅"));

    const http::Request& request = http.requests().front();
    EXPECT_EQ(request.method, http::Method::kPut);
    EXPECT_EQ(request.url,
              "https://discord.com/api/v10/channels/22/messages/11/reactions/"
              "%F0%9F%8C%85/@me");
    EXPECT_EQ(request.body, "");
  }());
}

TEST(RestTest, GatewayUrlAsksDiscordWhereToConnect) {
  RunOnEventLoop([]() -> Task<> {
    http::FakeClient http;
    http.Answer(
        Answer(200, R"({"url": "wss://gateway.discord.gg", "shards": 1})"));
    Rest rest(&http, "secret-token");

    EXPECT_THAT(co_await rest.GatewayUrl(),
                IsOkAndHolds("wss://gateway.discord.gg"));
    EXPECT_EQ(http.requests().front().url,
              "https://discord.com/api/v10/gateway/bot");
  }());
}

// Registering commands replaces the whole set the bot offers in one server,
// in one request.
TEST(RestTest, SetCommandsPutsTheWholeSetForAGuild) {
  RunOnEventLoop([]() -> Task<> {
    http::FakeClient http;
    Rest rest(&http, "secret-token");
    const std::vector<discord::Command> commands = {
        discord::Command{
            .name = "streak",
            .description = "Check your streak",
        },
    };

    ABSL_EXPECT_OK(co_await rest.SetCommands(
        discord::ApplicationId{
            .value = 77,
        },
        discord::GuildId{
            .value = 33,
        },
        commands));

    const http::Request& request = http.requests().front();
    EXPECT_EQ(request.method, http::Method::kPut);
    EXPECT_EQ(request.url,
              "https://discord.com/api/v10/applications/77/guilds/33/commands");
    EXPECT_EQ(request.body,
              R"([{"type":1,"name":"streak","description":"Check your streak",)"
              R"("options":[]}])");
  }());
}

// Without a guild it is the set offered everywhere, which is how to clear
// that set: by putting nothing.
TEST(RestTest, SetCommandsWithoutAGuildSetsTheGlobalOnes) {
  RunOnEventLoop([]() -> Task<> {
    http::FakeClient http;
    Rest rest(&http, "secret-token");

    ABSL_EXPECT_OK(co_await rest.SetCommands(
        discord::ApplicationId{
            .value = 77,
        },
        std::nullopt, {}));

    const http::Request& request = http.requests().front();
    EXPECT_EQ(request.url,
              "https://discord.com/api/v10/applications/77/commands");
    EXPECT_EQ(request.body, "[]");
  }());
}

// A channel knows which server it is in.
TEST(RestTest, GuildOfAsksAboutTheChannel) {
  RunOnEventLoop([]() -> Task<> {
    http::FakeClient http;
    http.Answer(Answer(200, R"({"id": "22", "type": 0, "guild_id": "33"})"));
    Rest rest(&http, "secret-token");

    const absl::StatusOr<discord::GuildId> guild =
        co_await rest.GuildOf(kChannel);

    ABSL_EXPECT_OK(guild);
    if (guild.ok()) EXPECT_EQ(guild->value, 33);
    EXPECT_EQ(http.requests().front().url,
              "https://discord.com/api/v10/channels/22");
  }());
}

// A direct message is a channel too, but in no server. And a channel the bot
// cannot see is reported as Discord reports it.
TEST(RestTest, GuildOfFailsForChannelsOutsideAnyServerOrOutOfSight) {
  RunOnEventLoop([]() -> Task<> {
    http::FakeClient http;
    Rest rest(&http, "secret-token");

    http.Answer(Answer(200, R"({"id": "22", "type": 1})"));
    EXPECT_THAT(co_await rest.GuildOf(kChannel),
                StatusIs(absl::StatusCode::kFailedPrecondition));

    http.Answer(Answer(404, R"({"message": "Unknown Channel"})"));
    EXPECT_THAT(co_await rest.GuildOf(kChannel),
                StatusIs(absl::StatusCode::kNotFound));
  }());
}

// Answering a command posts a message to the interaction's own address, with
// mentions switched off so that a reply naming people pings none of them.
TEST(RestTest, RespondAnswersTheInteractionWithoutNotifyingAnyone) {
  RunOnEventLoop([]() -> Task<> {
    http::FakeClient http;
    Rest rest(&http, "secret-token");

    ABSL_EXPECT_OK(co_await rest.Respond(
        discord::InteractionId{
            .value = 55,
        },
        "aW50ZXJhY3Rpb24", "🥇 **luke**"));

    const http::Request& request = http.requests().front();
    EXPECT_EQ(request.method, http::Method::kPost);
    EXPECT_EQ(request.url,
              "https://discord.com/api/v10/interactions/55/aW50ZXJhY3Rpb24/"
              "callback");
    EXPECT_EQ(request.body, R"({"type":4,"data":{"content":"🥇 **luke**",)"
                            R"("allowed_mentions":{"parse":[]}}})");
  }());
}

// Each way Discord says no has its own code, and carries Discord's words.
TEST(RestTest, RefusalsBecomeSpecificStatuses) {
  RunOnEventLoop([]() -> Task<> {
    http::FakeClient http;
    Rest rest(&http, "secret-token");

    http.Answer(Answer(401, R"({"message": "401: Unauthorized", "code": 0})"));
    EXPECT_THAT(co_await rest.CreateMessage(kChannel, "gm"),
                StatusIs(absl::StatusCode::kUnauthenticated,
                         HasSubstr("401: Unauthorized")));

    http.Answer(Answer(403, R"({"message": "Missing Permissions"})"));
    EXPECT_THAT(co_await rest.CreateMessage(kChannel, "gm"),
                StatusIs(absl::StatusCode::kPermissionDenied));

    http.Answer(Answer(404, R"({"message": "Unknown Channel"})"));
    EXPECT_THAT(co_await rest.CreateMessage(kChannel, "gm"),
                StatusIs(absl::StatusCode::kNotFound));

    http.Answer(Answer(400, R"({"message": "Cannot send an empty message"})"));
    EXPECT_THAT(co_await rest.CreateMessage(kChannel, ""),
                StatusIs(absl::StatusCode::kInvalidArgument));

    // An error page from a proxy rather than from Discord's API.
    http.Answer(Answer(502, "<html>Bad Gateway</html>"));
    EXPECT_THAT(co_await rest.CreateMessage(kChannel, "gm"),
                StatusIs(absl::StatusCode::kUnavailable));
  }());
}

// Not getting through at all is Unavailable, whatever the cause below.
TEST(RestTest, NetworkFailuresAreUnavailable) {
  RunOnEventLoop([]() -> Task<> {
    http::FakeClient http;
    http.Answer(absl::DeadlineExceededError("timed out"));
    Rest rest(&http, "secret-token");

    EXPECT_THAT(co_await rest.CreateMessage(kChannel, "gm"),
                StatusIs(absl::StatusCode::kUnavailable));
  }());
}

// A rate limit is waited out and the call made again; the caller sees only
// that it succeeded.
TEST(RestTest, WaitsOutRateLimits) {
  RunOnEventLoop([]() -> Task<> {
    http::FakeClient http;
    http.Answer(Answer(429, R"({"message": "You are being rate limited.",
                                "retry_after": 0.01, "global": false})"));
    http.Answer(Answer(200, R"({"id": "33"})"));
    Rest rest(&http, "secret-token");

    ABSL_EXPECT_OK(co_await rest.CreateMessage(kChannel, "gm"));

    EXPECT_EQ(http.requests().size(), 2);
  }());
}

// But not for ever: a limit that keeps being hit is reported.
TEST(RestTest, GivesUpOnARateLimitThatDoesNotLift) {
  RunOnEventLoop([]() -> Task<> {
    http::FakeClient http;
    for (int i = 0; i < 10; ++i) {
      http.Answer(Answer(429, R"({"message": "slow down", "retry_after": 0})"));
    }
    Rest rest(&http, "secret-token");

    EXPECT_THAT(co_await rest.CreateMessage(kChannel, "gm"),
                StatusIs(absl::StatusCode::kResourceExhausted));
    EXPECT_EQ(http.requests().size(), 4);
  }());
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //discord:rest_test -- --benchmark_filter=all
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
//   -----------------------------------------------------------
//   Benchmark                 Time             CPU   Iterations
//   -----------------------------------------------------------
//   BM_CreateMessage        575 ns          575 ns       694626
//   BM_AddReaction          507 ns          507 ns       840561
// End of results.
// clang-format on

// A client that answers everything with 204 and keeps nothing, so that a
// benchmark can call it without limit.
class IndifferentClient final : public http::Client {
 public:
  Task<absl::StatusOr<http::Response>> Send(const http::Request&) override {
    co_return http::Response{
        .status = 204,
    };
  }
};

Task<> CreateMessages(benchmark::State& state) {
  IndifferentClient http;
  Rest rest(&http, "secret-token");
  for (auto _ : state) {
    benchmark::DoNotOptimize(co_await rest.CreateMessage(
        kChannel,
        "Good morning <@400000000000000004>! Your streak has "
        "started! ☀️"));
  }
}

// Building one API request and reading its answer, without the network.
void BM_CreateMessage(benchmark::State& state) {
  EventLoop::Create()->Run(CreateMessages(state));
}
BENCHMARK(BM_CreateMessage);

Task<> AddReactions(benchmark::State& state) {
  IndifferentClient http;
  Rest rest(&http, "secret-token");
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        co_await rest.AddReaction(kChannel, kMessage, "🌅"));
  }
}

void BM_AddReaction(benchmark::State& state) {
  EventLoop::Create()->Run(AddReactions(state));
}
BENCHMARK(BM_AddReaction);

}  // namespace
