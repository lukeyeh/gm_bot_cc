// discord::Client by example, against a scripted Discord: connecting,
// hearing about messages, and acting on them.

#include "discord/client.h"

#include <memory>
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
#include "net/event_loop.h"
#include "websocket/fake_server.h"

namespace {

using absl_testing::StatusIs;
using testing::ElementsAre;

void RunOnEventLoop(Task<> test) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(std::move(test));
}

constexpr char kHello[] = R"({"op":10,"d":{"heartbeat_interval":3600000}})";
constexpr char kReady[] = R"({"op":0,"s":1,"t":"READY","d":{
    "session_id":"abc","resume_gateway_url":"wss://resume.test",
    "user":{"id":"99","username":"gm_bot","bot":true}}})";
constexpr char kGm[] = R"({"op":0,"s":3,"t":"MESSAGE_CREATE","d":{
    "id":"11","channel_id":"22","content":"gm",
    "author":{"id":"44","username":"lukeyeh"}}})";

// A Discord made of the two fakes: `http` answers API calls and `gateway`
// plays the event stream. Starts out willing to say where its gateway is.
struct FakeDiscord {
  FakeDiscord() {
    auto owned = std::make_unique<http::FakeClient>();
    http = owned.get();
    http->Answer(http::Response{
        .status = 200,
        .body = R"({"url":"wss://gateway.test"})",
    });
    transport = std::move(owned);
  }

  Task<absl::StatusOr<discord::Client>> Connect() {
    co_return co_await discord::Client::Connect("secret", std::move(transport),
                                                gateway.connector());
  }

  // Owned by the client once connected.
  http::FakeClient* http;
  std::unique_ptr<http::Client> transport;
  websocket::FakeServer gateway;
};

// Connecting finds the gateway, logs in, and learns who the bot is.
TEST(ClientTest, ConnectsAndKnowsItself) {
  RunOnEventLoop([]() -> Task<> {
    FakeDiscord fake;
    fake.gateway.Sends(kHello);
    fake.gateway.Sends(kReady);

    const absl::StatusOr<discord::Client> client = co_await fake.Connect();

    ABSL_EXPECT_OK(client);
    if (!client.ok()) co_return;
    EXPECT_EQ(client->self().id.value, 99);
    EXPECT_EQ(client->self().name, "gm_bot");
    EXPECT_TRUE(client->self().bot);
    EXPECT_EQ(fake.http->requests().front().url,
              "https://discord.com/api/v10/gateway/bot");
    EXPECT_THAT(fake.gateway.urls(),
                ElementsAre("wss://gateway.test/?v=10&encoding=json"));
  }());
}

// A bad token is found out at Connect, not later.
TEST(ClientTest, ConnectFailsIfDiscordRejectsTheToken) {
  RunOnEventLoop([]() -> Task<> {
    FakeDiscord fake;
    fake.gateway.Sends(kHello);
    fake.gateway.Closes(4004);

    EXPECT_THAT(co_await fake.Connect(),
                StatusIs(absl::StatusCode::kUnauthenticated));
  }());
}

// NextEvent reports what the bot can act on and passes over the rest of what
// Discord sends.
TEST(ClientTest, ReportsMessagesAndSkipsOtherEvents) {
  RunOnEventLoop([]() -> Task<> {
    FakeDiscord fake;
    fake.gateway.Sends(kHello);
    fake.gateway.Sends(kReady);
    fake.gateway.Sends(R"({"op":0,"s":2,"t":"TYPING_START","d":{}})");
    fake.gateway.Sends(kGm);
    absl::StatusOr<discord::Client> client = co_await fake.Connect();
    ABSL_EXPECT_OK(client);
    if (!client.ok()) co_return;

    const absl::StatusOr<discord::Event> event = co_await client->NextEvent();

    ABSL_EXPECT_OK(event);
    if (!event.ok()) co_return;
    const discord::Message& message =
        std::get<discord::MessageCreated>(*event).message;
    EXPECT_EQ(message.content, "gm");
    EXPECT_EQ(message.author.name, "lukeyeh");
    EXPECT_EQ(message.channel.value, 22);
  }());
}

// Acting on a message: reply in its channel, react to it.
TEST(ClientTest, SendsAndReacts) {
  RunOnEventLoop([]() -> Task<> {
    FakeDiscord fake;
    fake.gateway.Sends(kHello);
    fake.gateway.Sends(kReady);
    fake.gateway.Sends(kGm);
    absl::StatusOr<discord::Client> client = co_await fake.Connect();
    ABSL_EXPECT_OK(client);
    if (!client.ok()) co_return;
    const absl::StatusOr<discord::Event> event = co_await client->NextEvent();
    ABSL_EXPECT_OK(event);
    if (!event.ok()) co_return;
    const discord::Message& message =
        std::get<discord::MessageCreated>(*event).message;

    ABSL_EXPECT_OK(co_await client->React(message, "🌅"));
    ABSL_EXPECT_OK(co_await client->Send(
        message.channel,
        "Good morning " + discord::Mention(message.author.id) + "!"));

    const http::Request& reaction = fake.http->requests()[1];
    EXPECT_EQ(reaction.method, http::Method::kPut);
    EXPECT_EQ(reaction.url,
              "https://discord.com/api/v10/channels/22/messages/11/reactions/"
              "%F0%9F%8C%85/@me");
    const http::Request& reply = fake.http->requests()[2];
    EXPECT_EQ(reply.url, "https://discord.com/api/v10/channels/22/messages");
    EXPECT_EQ(reply.body, R"({"content":"Good morning <@44>!"})");
  }());
}

// Slash commands in full: offer them, hear one used, answer it.
TEST(ClientTest, OffersCommandsAndAnswersThem) {
  RunOnEventLoop([]() -> Task<> {
    FakeDiscord fake;
    fake.gateway.Sends(kHello);
    fake.gateway.Sends(R"({"op":0,"s":1,"t":"READY","d":{
        "session_id":"abc","resume_gateway_url":"wss://resume.test",
        "user":{"id":"99","username":"gm_bot","bot":true},
        "application":{"id":"77"}}})");
    fake.gateway.Sends(R"({"op":0,"s":2,"t":"INTERACTION_CREATE","d":{
        "id":"55","type":2,"token":"reply-here","channel_id":"22",
        "data":{"name":"leaderboard",
                "options":[{"name":"limit","type":4,"value":3}]},
        "member":{"user":{"id":"44","username":"lukeyeh"}}}})");
    absl::StatusOr<discord::Client> client = co_await fake.Connect();
    ABSL_EXPECT_OK(client);
    if (!client.ok()) co_return;

    const std::vector<discord::Command> commands = {
        discord::Command{
            .name = "leaderboard",
            .description = "Show the leaderboard",
        },
    };
    // Commands are offered server by server; a channel says which it is in.
    fake.http->Answer(http::Response{
        .status = 200,
        .body = R"({"id":"22","guild_id":"33"})",
    });
    const absl::StatusOr<discord::GuildId> guild =
        co_await client->GuildOf(discord::ChannelId{
            .value = 22,
        });
    ABSL_EXPECT_OK(guild);
    if (!guild.ok()) co_return;
    ABSL_EXPECT_OK(co_await client->OfferCommands(*guild, commands));

    const absl::StatusOr<discord::Event> event = co_await client->NextEvent();
    ABSL_EXPECT_OK(event);
    if (!event.ok()) co_return;
    const auto& invoked = std::get<discord::CommandInvoked>(*event);
    EXPECT_EQ(invoked.name, "leaderboard");
    EXPECT_EQ(invoked.user.name, "lukeyeh");
    EXPECT_EQ(invoked.Integer("limit", 10), 3);

    ABSL_EXPECT_OK(co_await client->Respond(invoked, "🥇 **lukeyeh**"));

    // The commands go to that channel's server, for the application READY
    // named (not the bot's user), so that they appear there at once...
    const std::vector<http::Request>& requests = fake.http->requests();
    EXPECT_EQ(requests[1].url, "https://discord.com/api/v10/channels/22");
    EXPECT_EQ(requests[2].method, http::Method::kPut);
    EXPECT_EQ(requests[2].url,
              "https://discord.com/api/v10/applications/77/guilds/33/commands");
    // ...and whatever the bot once offered everywhere is withdrawn.
    EXPECT_EQ(requests[3].url,
              "https://discord.com/api/v10/applications/77/commands");
    EXPECT_EQ(requests[3].body, "[]");

    EXPECT_EQ(
        requests[4].url,
        "https://discord.com/api/v10/interactions/55/reply-here/callback");
    EXPECT_EQ(requests[4].body,
              R"({"type":4,"data":{"content":"🥇 **lukeyeh**",)"
              R"("allowed_mentions":{"parse":[]}}})");
  }());
}

// A channel the bot cannot see has no server it can name, which is how a
// mistyped channel id shows itself at startup.
TEST(ClientTest, GuildOfFailsForAChannelItCannotSee) {
  RunOnEventLoop([]() -> Task<> {
    FakeDiscord fake;
    fake.gateway.Sends(kHello);
    fake.gateway.Sends(kReady);
    absl::StatusOr<discord::Client> client = co_await fake.Connect();
    ABSL_EXPECT_OK(client);
    if (!client.ok()) co_return;
    fake.http->Answer(http::Response{
        .status = 404,
        .body = R"({"message":"Unknown Channel"})",
    });

    EXPECT_THAT(co_await client->GuildOf(discord::ChannelId{
                    .value = 22,
                }),
                StatusIs(absl::StatusCode::kNotFound));
  }());
}

// What Discord refuses, the caller hears about.
TEST(ClientTest, ReportsRefusedActions) {
  RunOnEventLoop([]() -> Task<> {
    FakeDiscord fake;
    fake.gateway.Sends(kHello);
    fake.gateway.Sends(kReady);
    absl::StatusOr<discord::Client> client = co_await fake.Connect();
    ABSL_EXPECT_OK(client);
    if (!client.ok()) co_return;
    fake.http->Answer(http::Response{
        .status = 403,
        .body = R"({"message":"Missing Permissions"})",
    });

    EXPECT_THAT(co_await client->Send(
                    discord::ChannelId{
                        .value = 22,
                    },
                    "gm"),
                StatusIs(absl::StatusCode::kPermissionDenied));
  }());
}

}  // namespace
