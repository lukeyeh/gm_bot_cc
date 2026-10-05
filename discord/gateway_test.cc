// The Discord gateway by example: each test scripts what Discord does and
// shows what the Gateway sends back and reports.

#include "discord/gateway.h"

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "async/task.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "net/event_loop.h"
#include "websocket/fake_server.h"

namespace {

using absl_testing::StatusIs;
using discord_internal::Dispatch;
using discord_internal::Gateway;
using discord_internal::Intent;
using testing::ElementsAre;
using testing::StartsWith;

void RunOnEventLoop(Task<> test) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(std::move(test));
}

// What Discord says first on every connection. The interval is an hour so
// that no heartbeat falls due by the clock during a test.
constexpr char kHello[] = R"({"op":10,"d":{"heartbeat_interval":3600000}})";

// The first dispatch of a new session.
constexpr char kReady[] = R"({"op":0,"s":1,"t":"READY","d":{
    "session_id":"abc","resume_gateway_url":"wss://resume.test",
    "user":{"id":"99","username":"gm_bot","bot":true}}})";

// A dispatch numbered `sequence` reporting a message that says `content`.
std::string Message(int64_t sequence, std::string_view content) {
  return absl::StrCat(R"({"op":0,"s":)", sequence,
                      R"(,"t":"MESSAGE_CREATE","d":{"content":")", content,
                      R"("}})");
}

constexpr char kIdentify[] =
    R"({"op":2,"d":{"token":"secret","intents":33281,"properties":)"
    R"({"os":"linux","browser":"gm_bot","device":"gm_bot"}}})";

// A gateway talking to `server`, and the waits it has taken between attempts
// to connect, which a test gateway only records.
struct Fixture {
  explicit Fixture(websocket::FakeServer& server)
      : gateway(server.connector(), "wss://gateway.test", "secret",
                {
                    Intent::kGuilds,
                    Intent::kGuildMessages,
                    Intent::kMessageContent,
                },
                [this](std::chrono::milliseconds wait) {
                  return RecordPause(wait);
                }) {}

  Task<> RecordPause(std::chrono::milliseconds wait) {
    pauses.push_back(wait);
    co_return;
  }

  // The content of the next dispatch, which must be a message.
  Task<std::string> NextMessage() {
    const absl::StatusOr<Dispatch> dispatch = co_await gateway.Next();
    ABSL_EXPECT_OK(dispatch);
    if (!dispatch.ok()) co_return "";
    EXPECT_EQ(dispatch->type, "MESSAGE_CREATE");
    co_return dispatch->data["content"].AsString();
  }

  // Waits for READY, as every user of a gateway does first.
  Task<> AwaitReady() {
    const absl::StatusOr<Dispatch> ready = co_await gateway.Next();
    ABSL_EXPECT_OK(ready);
    if (ready.ok()) EXPECT_EQ(ready->type, "READY");
  }

  std::vector<std::chrono::milliseconds> pauses;
  Gateway gateway;
};

// The opening exchange: Discord greets, the bot says who it is and what it
// wants to hear about, and dispatches follow.
TEST(GatewayTest, IdentifiesThenDeliversDispatches) {
  RunOnEventLoop([]() -> Task<> {
    websocket::FakeServer discord;
    discord.Sends(kHello);
    discord.Sends(kReady);
    discord.Sends(Message(2, "gm"));
    Fixture fixture(discord);

    const absl::StatusOr<Dispatch> ready = co_await fixture.gateway.Next();
    ABSL_EXPECT_OK(ready);
    if (!ready.ok()) co_return;
    EXPECT_EQ(ready->type, "READY");
    EXPECT_EQ(ready->data["user"]["username"].AsString(), "gm_bot");
    EXPECT_EQ(co_await fixture.NextMessage(), "gm");

    EXPECT_THAT(discord.urls(),
                ElementsAre("wss://gateway.test/?v=10&encoding=json"));
    EXPECT_THAT(discord.received(), ElementsAre(kIdentify));
  }());
}

// When Discord has nothing to say for a heartbeat interval, the bot says it
// is still there, quoting the number of the last dispatch it saw.
TEST(GatewayTest, HeartbeatsWhenTheConnectionIsQuiet) {
  RunOnEventLoop([]() -> Task<> {
    websocket::FakeServer discord;
    discord.Sends(kHello);
    discord.Sends(kReady);
    discord.StaysQuiet();
    discord.Sends(R"({"op":11})");  // Discord acknowledges the heartbeat.
    discord.StaysQuiet();
    discord.Sends(R"({"op":11})");
    discord.Sends(Message(2, "gm"));
    Fixture fixture(discord);

    co_await fixture.AwaitReady();
    EXPECT_EQ(co_await fixture.NextMessage(), "gm");

    EXPECT_THAT(discord.received(), ElementsAre(kIdentify, R"({"op":1,"d":1})",
                                                R"({"op":1,"d":1})"));
  }());
}

// Discord can also ask for a heartbeat there and then.
TEST(GatewayTest, HeartbeatsWhenAsked) {
  RunOnEventLoop([]() -> Task<> {
    websocket::FakeServer discord;
    discord.Sends(kHello);
    discord.Sends(kReady);
    discord.Sends(R"({"op":1})");
    discord.Sends(Message(2, "gm"));
    Fixture fixture(discord);

    co_await fixture.AwaitReady();
    EXPECT_EQ(co_await fixture.NextMessage(), "gm");

    EXPECT_THAT(discord.received(),
                ElementsAre(kIdentify, R"({"op":1,"d":1})"));
  }());
}

// A broken connection is the gateway's problem, not the caller's: it
// connects to where READY said to resume, and asks for everything after the
// last dispatch it saw. The caller just gets the next dispatch.
TEST(GatewayTest, ResumesAfterTheConnectionBreaks) {
  RunOnEventLoop([]() -> Task<> {
    websocket::FakeServer discord;
    discord.Sends(kHello);
    discord.Sends(kReady);
    discord.Sends(Message(2, "before"));
    discord.Drops();
    discord.Sends(kHello);
    discord.Sends(Message(3, "missed while away"));
    Fixture fixture(discord);

    co_await fixture.AwaitReady();
    EXPECT_EQ(co_await fixture.NextMessage(), "before");
    EXPECT_EQ(co_await fixture.NextMessage(), "missed while away");

    EXPECT_THAT(discord.urls(),
                ElementsAre("wss://gateway.test/?v=10&encoding=json",
                            "wss://resume.test/?v=10&encoding=json"));
    EXPECT_EQ(discord.received().back(),
              R"({"op":6,"d":{"token":"secret","session_id":"abc","seq":2}})");
    // The first reconnection is immediate.
    EXPECT_THAT(fixture.pauses, ElementsAre());
  }());
}

// Discord restarts its servers routinely, and says so first.
TEST(GatewayTest, ResumesWhenDiscordAsksItToReconnect) {
  RunOnEventLoop([]() -> Task<> {
    websocket::FakeServer discord;
    discord.Sends(kHello);
    discord.Sends(kReady);
    discord.Sends(R"({"op":7})");
    discord.Sends(kHello);
    discord.Sends(Message(2, "after the move"));
    Fixture fixture(discord);

    co_await fixture.AwaitReady();
    EXPECT_EQ(co_await fixture.NextMessage(), "after the move");

    EXPECT_THAT(discord.received().back(), StartsWith(R"({"op":6,)"));
  }());
}

// A connection can die silently. An unacknowledged heartbeat is how the
// gateway finds out.
TEST(GatewayTest, ReconnectsWhenHeartbeatsGoUnanswered) {
  RunOnEventLoop([]() -> Task<> {
    websocket::FakeServer discord;
    discord.Sends(kHello);
    discord.Sends(kReady);
    discord.StaysQuiet();  // The gateway heartbeats...
    discord.StaysQuiet();  // ...and hears nothing back by the next one.
    discord.Sends(kHello);
    discord.Sends(Message(2, "back"));
    Fixture fixture(discord);

    co_await fixture.AwaitReady();
    EXPECT_EQ(co_await fixture.NextMessage(), "back");

    EXPECT_EQ(discord.urls().size(), 2);
    EXPECT_THAT(discord.received().back(), StartsWith(R"({"op":6,)"));
  }());
}

// When Discord says the session is gone, there is nothing to resume: the
// gateway starts a new one, after a pause as Discord asks.
TEST(GatewayTest, StartsANewSessionWhenTheOldOneIsInvalid) {
  RunOnEventLoop([]() -> Task<> {
    websocket::FakeServer discord;
    discord.Sends(kHello);
    discord.Sends(kReady);
    discord.Drops();
    discord.Sends(kHello);
    discord.Sends(R"({"op":9,"d":false})");  // In answer to the resume.
    discord.Sends(kHello);
    discord.Sends(kReady);
    Fixture fixture(discord);

    co_await fixture.AwaitReady();
    co_await fixture.AwaitReady();

    EXPECT_THAT(discord.received(),
                ElementsAre(kIdentify, StartsWith(R"({"op":6,)"), kIdentify));
    EXPECT_EQ(discord.urls().back(), "wss://gateway.test/?v=10&encoding=json");
    EXPECT_THAT(fixture.pauses, ElementsAre(std::chrono::seconds(1)));
  }());
}

// While reconnecting keeps failing, the gateway keeps trying, waiting twice
// as long each time.
TEST(GatewayTest, BacksOffWhileItCannotReconnect) {
  RunOnEventLoop([]() -> Task<> {
    websocket::FakeServer discord;
    discord.Sends(kHello);
    discord.Sends(kReady);
    discord.Drops();
    discord.RefusesToConnect();
    discord.RefusesToConnect();
    discord.RefusesToConnect();
    discord.Sends(kHello);
    discord.Sends(Message(2, "back at last"));
    Fixture fixture(discord);

    co_await fixture.AwaitReady();
    EXPECT_EQ(co_await fixture.NextMessage(), "back at last");

    EXPECT_THAT(fixture.pauses,
                ElementsAre(std::chrono::seconds(1), std::chrono::seconds(2),
                            std::chrono::seconds(4)));
  }());
}

// Some closes mean "do not come back", and those are the caller's to know
// about. Each says what to do about it.
TEST(GatewayTest, ReportsBeingTurnedAwayForGood) {
  RunOnEventLoop([]() -> Task<> {
    {
      websocket::FakeServer discord;
      discord.Sends(kHello);
      discord.Closes(4004);  // Authentication failed.
      Fixture fixture(discord);
      EXPECT_THAT(co_await fixture.gateway.Next(),
                  StatusIs(absl::StatusCode::kUnauthenticated));
    }
    {
      websocket::FakeServer discord;
      discord.Sends(kHello);
      discord.Closes(4014);  // Disallowed intents.
      Fixture fixture(discord);
      EXPECT_THAT(co_await fixture.gateway.Next(),
                  StatusIs(absl::StatusCode::kPermissionDenied,
                           testing::HasSubstr("Developer Portal")));
    }
    {
      // The same holds long after starting, should the token be revoked.
      websocket::FakeServer discord;
      discord.Sends(kHello);
      discord.Sends(kReady);
      discord.Closes(4004);
      Fixture fixture(discord);
      co_await fixture.AwaitReady();
      EXPECT_THAT(co_await fixture.gateway.Next(),
                  StatusIs(absl::StatusCode::kUnauthenticated));
    }
  }());
}

// Before the gateway has ever been ready, failing to connect is reported at
// once: at startup it more likely means a mistake than a blip.
TEST(GatewayTest, FailsFastIfItCannotConnectAtAll) {
  RunOnEventLoop([]() -> Task<> {
    websocket::FakeServer discord;
    discord.RefusesToConnect();
    Fixture fixture(discord);

    EXPECT_THAT(co_await fixture.gateway.Next(),
                StatusIs(absl::StatusCode::kUnavailable));
    EXPECT_THAT(fixture.pauses, ElementsAre());
  }());
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //discord:gateway_test -- --benchmark_filter=all
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
//   ----------------------------------------------------------
//   Benchmark                Time             CPU   Iterations
//   ----------------------------------------------------------
//   BM_NextDispatch       4075 ns         4072 ns       101529
// End of results.
// clang-format on

// A dispatch of the size Discord really sends for a message, numbered
// `sequence`.
std::string FullMessage(int64_t sequence) {
  return absl::StrCat(R"({"t":"MESSAGE_CREATE","s":)", sequence,
                      R"(,"op":0,"d":{"type":0,"tts":false,
  "timestamp":"2026-10-05T12:00:00.000000+00:00","pinned":false,
  "nonce":"2000000000000000002","mentions":[],"mention_roles":[],
  "mention_everyone":false,
  "member":{"roles":["500000000000000001","500000000000000002"],
    "premium_since":null,"pending":false,"nick":"luke the early","mute":false,
    "joined_at":"2021-12-29T20:14:27.585000+00:00","flags":0,"deaf":false,
    "communication_disabled_until":null,"banner":null,"avatar":null},
  "id":"1000000000000000001","flags":0,"embeds":[],"edited_timestamp":null,
  "content":"gm everyone, hope the coffee is strong today","components":[],
  "channel_type":0,"channel_id":"2000000000000000002",
  "author":{"username":"lukeyeh","public_flags":0,"primary_guild":null,
    "id":"400000000000000004","global_name":"Luke","discriminator":"0",
    "collectibles":null,"clan":null,"avatar_decoration_data":null,
    "avatar":"8e1f9d0c2b3a4d5e6f708192a3b4c5d6"},
  "attachments":[],"guild_id":"300000000000000003"}})");
}

Task<> Dispatches(benchmark::State& state) {
  websocket::FakeServer discord;
  discord.Sends(kHello);
  discord.Sends(kReady);
  Fixture fixture(discord);
  co_await fixture.AwaitReady();

  int64_t sequence = 2;
  int scripted = 0;
  for (auto _ : state) {
    // Scripted a batch at a time, so that the script is never empty and the
    // scripting is a small part of what is timed.
    if (scripted == 0) {
      for (scripted = 0; scripted < 512; ++scripted) {
        discord.Sends(FullMessage(sequence++));
      }
    }
    --scripted;
    benchmark::DoNotOptimize(co_await fixture.gateway.Next());
  }
}

// What the gateway itself adds to each event: parsing the payload and
// keeping the session's books. The connection is scripted, so no I/O.
void BM_NextDispatch(benchmark::State& state) {
  EventLoop::Create()->Run(Dispatches(state));
}
BENCHMARK(BM_NextDispatch);

}  // namespace
