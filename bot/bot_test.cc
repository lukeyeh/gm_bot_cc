// The bot by example: what it does, seen from Discord's side, when people
// say GM and when they say other things.

#include "bot/bot.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "async/task.h"
#include "bot/config.h"
#include "discord/client.h"
#include "discord/model.h"
#include "gm/ledger.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "http/client.h"
#include "http/fake_client.h"
#include "net/event_loop.h"
#include "websocket/fake_server.h"

namespace {

using absl_testing::StatusIs;
using testing::AllOf;
using testing::Contains;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::Not;

void RunOnEventLoop(Task<> test) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(std::move(test));
}

constexpr char kApi[] = "https://discord.com/api/v10";

// A message as the gateway reports it. Ids are small numbers for legibility:
// the message's own id is its sequence number.
struct Said {
  int sequence = 0;
  int channel = 22;
  int guild = 33;
  int author = 44;
  std::string_view name = "luke";
  bool bot = false;
  std::string_view content;
};

std::string Dispatch(const Said& said) {
  return absl::StrCat(R"({"op":0,"t":"MESSAGE_CREATE","s":)", said.sequence,
                      R"(,"d":{"id":")", said.sequence, R"(","channel_id":")",
                      said.channel, R"(","guild_id":")", said.guild,
                      R"(","content":")", said.content, R"(","author":{"id":")",
                      said.author, R"(","username":")", said.name,
                      R"(","bot":)", said.bot ? "true" : "false", "}}}");
}

// What the bot did, as Discord's API saw it: "PUT <path>" for each reaction,
// "POST <path> <body>" for each message.
std::vector<std::string> Actions(const http::FakeClient& http) {
  std::vector<std::string> actions;
  // The first request is the client asking where the gateway is.
  for (size_t i = 1; i < http.requests().size(); ++i) {
    const http::Request& request = http.requests()[i];
    const std::string_view path =
        std::string_view(request.url).substr(std::string_view(kApi).size());
    // Registering the commands, and the question about each channel that
    // goes with it, have tests of their own.
    if (path.starts_with("/applications/") ||
        request.method == http::Method::kGet) {
      continue;
    }

    actions.push_back(request.method == http::Method::kPut
                          ? absl::StrCat("PUT ", path)
                          : absl::StrCat("POST ", path, " ", request.body));
  }
  return actions;
}

// Someone using a slash command, as the gateway reports it. `options` is the
// JSON of the options they filled in.
struct Used {
  int sequence = 0;
  // The server it was used in, or 0 for a direct message.
  int guild = 33;
  int author = 44;
  std::string_view name = "luke";
  std::string_view command;
  std::string_view options = "[]";
};

std::string Dispatch(const Used& used) {
  return absl::StrCat(
      R"({"op":0,"t":"INTERACTION_CREATE","s":)", used.sequence,
      R"(,"d":{"type":2,"id":")", used.sequence, R"(","token":"token-)",
      used.sequence, R"(","channel_id":"22","guild_id":)",
      used.guild == 0 ? "null" : absl::StrCat("\"", used.guild, "\""),
      R"(,"data":{"name":")", used.command, R"(","options":)", used.options,
      R"(},"member":{"user":{"id":")", used.author, R"(","username":")",
      used.name, R"("}}}})");
}

// The bot's answer to the command numbered `sequence`, as an action.
std::string Reply(int sequence, std::string_view content) {
  return absl::StrCat("POST /interactions/", sequence, "/token-", sequence,
                      R"(/callback {"type":4,"data":{"content":")", content,
                      R"(","allowed_mentions":{"parse":[]}}})");
}

constexpr char kReactionTo3[] =
    "PUT /channels/22/messages/3/reactions/%F0%9F%8C%85/@me";
constexpr char kReactionTo2[] =
    "PUT /channels/22/messages/2/reactions/%F0%9F%8C%85/@me";

// Runs the bot against a Discord that sends `dispatches` and then revokes the
// bot's token, which is what ends Serve. Returns what the bot did.
struct Outcome {
  absl::Status stopped;
  std::vector<std::string> actions;
};

Task<Outcome> ServeEvents(std::vector<std::string> dispatches,
                          gm::Ledger& ledger, bot::Config config,
                          std::vector<http::Response> answers = {}) {
  auto http = std::make_unique<http::FakeClient>();
  const http::FakeClient& requests = *http;
  http->Answer(http::Response{
      .status = 200,
      .body = R"({"url":"wss://gateway.test"})",
  });
  // On starting, the bot asks which server its channel is in.
  http->Answer(http::Response{
      .status = 200,
      .body = R"({"id":"22","guild_id":"33"})",
  });
  for (http::Response& answer : answers) http->Answer(std::move(answer));

  websocket::FakeServer gateway;
  gateway.Sends(R"({"op":10,"d":{"heartbeat_interval":3600000}})");
  gateway.Sends(R"({"op":0,"s":1,"t":"READY","d":{"session_id":"abc",
      "resume_gateway_url":"wss://resume.test",
      "user":{"id":"99","username":"gm_bot","bot":true}}})");
  for (std::string& dispatch : dispatches) gateway.Sends(std::move(dispatch));
  gateway.Closes(4004);

  absl::StatusOr<discord::Client> client = co_await discord::Client::Connect(
      "secret", std::move(http), gateway.connector());
  ABSL_EXPECT_OK(client);
  if (!client.ok()) co_return Outcome{};

  const absl::Status stopped = co_await bot::Serve(*client, ledger, config);
  co_return Outcome{
      .stopped = stopped,
      .actions = Actions(requests),
  };
}

gm::Ledger NewLedger() {
  absl::StatusOr<gm::Ledger> ledger = gm::Ledger::InMemory();
  ABSL_EXPECT_OK(ledger);
  return std::move(*ledger);
}

// The GM channel is 22, in server 33, which is where Said and Used put things
// unless told not to.
bot::Config InChannel22() {
  return bot::Config{
      .token = "secret",
      .channels =
          {
              discord::ChannelId{
                  .value = 22,
              },
          },
      .time_zone = absl::UTCTimeZone(),
  };
}

// The whole point: someone says GM, the bot marks it with a sunrise and
// welcomes the new streak, and the ledger has it.
TEST(BotTest, AcknowledgesAndRecordsAGm) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();

    const Outcome outcome = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "gm everyone",
            }),
        },
        ledger, InChannel22());

    EXPECT_THAT(
        outcome.actions,
        ElementsAre(
            kReactionTo2,
            "POST /channels/22/messages "
            R"({"content":"Good morning <@44>! Your streak has started! ☀️"})"));

    const absl::StatusOr<gm::Standing> standing =
        ledger.community(33).StandingOf(
            44, absl::ToCivilDay(absl::Now(), absl::UTCTimeZone()));
    ABSL_EXPECT_OK(standing);
    if (!standing.ok()) co_return;
    EXPECT_EQ(standing->streak, 1);
    EXPECT_EQ(standing->member.name, "luke");
  }());
}

// A second GM the same day is acknowledged, and its author told it did not
// count again.
TEST(BotTest, TellsSomeoneWhoHasAlreadySaidGmToday) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();

    const Outcome outcome = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "gm",
            }),
            Dispatch(Said{
                .sequence = 3,
                .content = "GM!",
            }),
        },
        ledger, InChannel22());

    EXPECT_THAT(
        outcome.actions,
        ElementsAre(kReactionTo2, HasSubstr("Your streak has started"),
                    "PUT /channels/22/messages/3/reactions/%F0%9F%8C%85/@me",
                    HasSubstr("you already said GM today")));
  }());
}

// The GM channel is for GMs. Anything else there gets a thumbs down, and its
// author loses the streak they had and is told what to say instead.
TEST(BotTest, AnythingElseInTheGmChannelForfeitsTheStreak) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();

    const Outcome outcome = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "gm",
            }),
            Dispatch(Said{
                .sequence = 3,
                .content = "did anyone watch the game",
            }),
        },
        ledger, InChannel22());

    EXPECT_THAT(
        outcome.actions,
        ElementsAre(
            kReactionTo2, HasSubstr("Your streak has started"),
            "PUT /channels/22/messages/3/reactions/%F0%9F%91%8E/@me",
            AllOf(HasSubstr("<@44>, only GMs belong in this channel!"),
                  HasSubstr("Your 1 day streak has been reset to 0."))));

    const absl::StatusOr<gm::Standing> standing =
        ledger.community(33).StandingOf(
            44, absl::ToCivilDay(absl::Now(), absl::UTCTimeZone()));
    ABSL_EXPECT_OK(standing);
    if (!standing.ok()) co_return;
    EXPECT_EQ(standing->streak, 0);
    EXPECT_EQ(standing->best, 1);
  }());
}

// Someone with no streak is still told off, just not told of a loss. And a
// GM after a forfeit starts a new streak, the same day if they like.
TEST(BotTest, AStreakCanBeStartedAgainAfterAForfeit) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();

    const Outcome outcome = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "hello?",
            }),
            Dispatch(Said{
                .sequence = 3,
                .content = "gm",
            }),
        },
        ledger, InChannel22());

    EXPECT_THAT(
        outcome.actions,
        ElementsAre("PUT /channels/22/messages/2/reactions/%F0%9F%91%8E/@me",
                    AllOf(HasSubstr("only GMs belong in this channel!"),
                          Not(HasSubstr("has been reset"))),
                    kReactionTo3, HasSubstr("Your streak has started")));
  }());
}

// What happens in other channels, whether a GM or not, and what other bots
// say (this one's own replies among them) is none of the bot's business.
TEST(BotTest, IgnoresOtherChannelsAndOtherBots) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();

    const Outcome outcome = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .channel = 23,
                .content = "gm",
            }),
            Dispatch(Said{
                .sequence = 3,
                .channel = 23,
                .content = "did anyone watch the game",
            }),
            Dispatch(Said{
                .sequence = 4,
                .author = 99,
                .name = "gm_bot",
                .bot = true,
                .content = "👎 <@44>, only GMs belong in this channel!",
            }),
            Dispatch(Said{
                .sequence = 5,
                .author = 55,
                .name = "another bot",
                .bot = true,
                .content = "gm",
            }),
        },
        ledger, InChannel22());

    EXPECT_THAT(outcome.actions, ElementsAre());
  }());
}

// Discord refusing one action (here, the reaction) does not stop the bot:
// the GM is still recorded and the next one is handled as usual.
TEST(BotTest, CarriesOnWhenAnActionIsRefused) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();

    const Outcome outcome = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "gm",
            }),
            Dispatch(Said{
                .sequence = 3,
                .author = 45,
                .name = "ada",
                .content = "gm",
            }),
        },
        ledger, InChannel22(),
        {
            // To registering the commands, which comes first and takes two
            // requests.
            http::Response{
                .status = 204,
            },
            http::Response{
                .status = 204,
            },
            http::Response{
                .status = 403,
                .body = R"({"message":"Missing Permissions"})",
            },
        });

    EXPECT_THAT(
        outcome.actions,
        ElementsAre(kReactionTo2,  // Refused.
                    "PUT /channels/22/messages/3/reactions/%F0%9F%8C%85/@me",
                    HasSubstr("Good morning <@45>!")));

    const absl::StatusOr<gm::Standing> standing =
        ledger.community(33).StandingOf(
            44, absl::ToCivilDay(absl::Now(), absl::UTCTimeZone()));
    ABSL_EXPECT_OK(standing);
    if (standing.ok()) EXPECT_EQ(standing->streak, 1);
  }());
}

// On starting, the bot tells Discord which commands it answers, in the server
// its channel is in.
TEST(BotTest, RegistersItsCommandsInTheGmChannelsServer) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();
    auto http = std::make_unique<http::FakeClient>();
    const http::FakeClient& requests = *http;
    http->Answer(http::Response{
        .status = 200,
        .body = R"({"url":"wss://gateway.test"})",
    });
    http->Answer(http::Response{
        .status = 200,
        .body = R"({"id":"22","guild_id":"33"})",
    });
    websocket::FakeServer gateway;
    gateway.Sends(R"({"op":10,"d":{"heartbeat_interval":3600000}})");
    gateway.Sends(R"({"op":0,"s":1,"t":"READY","d":{"session_id":"abc",
        "resume_gateway_url":"wss://resume.test",
        "user":{"id":"99","username":"gm_bot","bot":true}}})");
    gateway.Closes(4004);
    absl::StatusOr<discord::Client> client = co_await discord::Client::Connect(
        "secret", std::move(http), gateway.connector());
    ABSL_EXPECT_OK(client);
    if (!client.ok()) co_return;

    (co_await bot::Serve(*client, ledger, InChannel22())).IgnoreError();

    EXPECT_EQ(requests.requests()[1].url, absl::StrCat(kApi, "/channels/22"));
    const http::Request& registration = requests.requests()[2];
    EXPECT_EQ(registration.method, http::Method::kPut);
    EXPECT_EQ(registration.url,
              absl::StrCat(kApi, "/applications/99/guilds/33/commands"));
    for (const char* const command :
         {"leaderboard", "streak", "gmlist", "gmadd", "gmremove"}) {
      EXPECT_THAT(registration.body,
                  HasSubstr(absl::StrCat(R"("name":")", command, "\"")));
    }
    // Changing the phrases is for administrators.
    EXPECT_THAT(registration.body,
                HasSubstr(R"("default_member_permissions":"8")"));
  }());
}

// A GM channel the bot cannot see is a mistake in its configuration, found
// at once rather than by the bot silently never hearing a GM.
TEST(BotTest, StopsIfItCannotSeeTheGmChannel) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();
    auto http = std::make_unique<http::FakeClient>();
    http->Answer(http::Response{
        .status = 200,
        .body = R"({"url":"wss://gateway.test"})",
    });
    http->Answer(http::Response{
        .status = 404,
        .body = R"({"message":"Unknown Channel"})",
    });
    websocket::FakeServer gateway;
    gateway.Sends(R"({"op":10,"d":{"heartbeat_interval":3600000}})");
    gateway.Sends(R"({"op":0,"s":1,"t":"READY","d":{"session_id":"abc",
        "resume_gateway_url":"wss://resume.test",
        "user":{"id":"99","username":"gm_bot","bot":true}}})");
    absl::StatusOr<discord::Client> client = co_await discord::Client::Connect(
        "secret", std::move(http), gateway.connector());
    ABSL_EXPECT_OK(client);
    if (!client.ok()) co_return;

    EXPECT_THAT(
        co_await bot::Serve(*client, ledger, InChannel22()),
        StatusIs(absl::StatusCode::kNotFound, HasSubstr("Unknown Channel")));
  }());
}

// If Discord will not take the commands, the bot says so rather than run
// without them.
TEST(BotTest, StopsIfItCannotRegisterItsCommands) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();

    const Outcome outcome = co_await ServeEvents(
        {}, ledger, InChannel22(),
        {
            http::Response{
                .status = 400,
                .body = R"({"message":"Invalid Form Body"})",
            },
        });

    EXPECT_THAT(outcome.stopped,
                StatusIs(absl::StatusCode::kInvalidArgument,
                         HasSubstr("registering the bot's commands")));
  }());
}

// /leaderboard answers with the standings: here Ada, who said GM today, and
// Luke, who has too.
TEST(BotTest, AnswersLeaderboard) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();

    const Outcome outcome = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "gm",
            }),
            Dispatch(Said{
                .sequence = 3,
                .author = 45,
                .name = "ada",
                .content = "gm",
            }),
            Dispatch(Used{
                .sequence = 4,
                .command = "leaderboard",
            }),
        },
        ledger, InChannel22());

    EXPECT_THAT(outcome.actions,
                Contains(Reply(4,
                               "🌅 **GM Streak Leaderboard**\\n\\n"
                               "🥇 **ada** — 1 day\\n"
                               "🥇 **luke** — 1 day")));
  }());
}

// Its one option says how many people to show.
TEST(BotTest, LeaderboardHonoursItsLimit) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();

    const Outcome outcome = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "gm",
            }),
            Dispatch(Said{
                .sequence = 3,
                .author = 45,
                .name = "ada",
                .content = "gm",
            }),
            Dispatch(Used{
                .sequence = 4,
                .command = "leaderboard",
                .options = R"([{"name":"limit","type":4,"value":1}])",
            }),
        },
        ledger, InChannel22());

    EXPECT_THAT(outcome.actions,
                Contains(Reply(4,
                               "🌅 **GM Streak Leaderboard**\\n\\n"
                               "🥇 **ada** — 1 day")));
  }());
}

// /streak tells whoever asks about their own streak, before and after they
// have one.
TEST(BotTest, AnswersStreak) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();

    const Outcome outcome = co_await ServeEvents(
        {
            Dispatch(Used{
                .sequence = 2,
                .command = "streak",
            }),
            Dispatch(Said{
                .sequence = 3,
                .content = "gm",
            }),
            Dispatch(Used{
                .sequence = 4,
                .command = "streak",
            }),
        },
        ledger, InChannel22());

    EXPECT_THAT(
        outcome.actions,
        ElementsAre(Reply(2,
                          "<@44>, you don't have an active streak. Say GM "
                          "to start one! 🌅"),
                    kReactionTo3, HasSubstr("Your streak has started"),
                    Reply(4, "🔥 <@44>, your current streak is **1 day**!")));
  }());
}

// /gmlist shows what counts.
TEST(BotTest, AnswersGmList) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();

    const Outcome outcome = co_await ServeEvents(
        {
            Dispatch(Used{
                .sequence = 2,
                .command = "gmlist",
            }),
        },
        ledger, InChannel22());

    EXPECT_THAT(outcome.actions,
                ElementsAre(Reply(2,
                                  "✅ **GM phrases**\\n\\n"
                                  "• `gm`\\n• `good morning`\\n• `morning`"
                                  "\\n\\nCapitalisation does not matter.")));
  }());
}

// /gmadd makes a new phrase count, from the very next message.
TEST(BotTest, AnAddedPhraseCountsAsAGm) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();

    const Outcome outcome = co_await ServeEvents(
        {
            Dispatch(Used{
                .sequence = 2,
                .command = "gmadd",
                .options = R"([{"name":"phrase","type":3,"value":" Yo "}])",
            }),
            Dispatch(Said{
                .sequence = 3,
                .content = "yo everyone",
            }),
        },
        ledger, InChannel22());

    EXPECT_THAT(
        outcome.actions,
        ElementsAre(Reply(2, "✅ `yo` now counts as a GM."), kReactionTo3,
                    HasSubstr("Your streak has started")));
  }());
}

// /gmremove takes a phrase off the list, after which saying it in the GM
// channel is like saying anything else there.
TEST(BotTest, ARemovedPhraseNoLongerCounts) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();

    const Outcome outcome = co_await ServeEvents(
        {
            Dispatch(Used{
                .sequence = 2,
                .command = "gmremove",
                .options = R"([{"name":"phrase","type":3,"value":"Morning"}])",
            }),
            Dispatch(Said{
                .sequence = 3,
                .content = "morning",
            }),
        },
        ledger, InChannel22());

    EXPECT_THAT(
        outcome.actions,
        ElementsAre(
            Reply(2, "✅ `morning` no longer counts as a GM."),
            "PUT /channels/22/messages/3/reactions/%F0%9F%91%8E/@me",
            HasSubstr("Say `gm` or `good morning` to start a new one.")));
  }());
}

// Whoever uses the commands is told when nothing changed, and why a phrase
// was not accepted.
TEST(BotTest, ExplainsPhraseChangesThatDidNotHappen) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();

    const Outcome outcome = co_await ServeEvents(
        {
            Dispatch(Used{
                .sequence = 2,
                .command = "gmadd",
                .options = R"([{"name":"phrase","type":3,"value":"GM"}])",
            }),
            Dispatch(Used{
                .sequence = 3,
                .command = "gmadd",
                .options = R"([{"name":"phrase","type":3,"value":"   "}])",
            }),
            Dispatch(Used{
                .sequence = 4,
                .command = "gmremove",
                .options = R"([{"name":"phrase","type":3,"value":"yo"}])",
            }),
        },
        ledger, InChannel22());

    EXPECT_THAT(outcome.actions,
                ElementsAre(Reply(2, "`gm` already counts as a GM."),
                            Reply(3, "❌ A phrase cannot be empty."),
                            Reply(4, "❌ `yo` is not a GM phrase.")));
  }());
}

// One bot, two servers, each with its GM channel. The same person keeps a
// separate streak in each, each has its own leaderboard and phrases, and the
// commands are offered in both.
TEST(BotTest, ServesSeveralServersSeparately) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();
    bot::Config config = InChannel22();
    config.channels.push_back(discord::ChannelId{
        .value = 24,
    });

    const Outcome outcome = co_await ServeEvents(
        {
            // Luke says GM in server 33, and adds a phrase there.
            Dispatch(Said{
                .sequence = 2,
                .content = "gm",
            }),
            Dispatch(Used{
                .sequence = 3,
                .command = "gmadd",
                .options = R"([{"name":"phrase","type":3,"value":"yo"}])",
            }),
            // In server 35 that phrase means nothing, and Luke has no streak.
            Dispatch(Said{
                .sequence = 4,
                .channel = 24,
                .guild = 35,
                .content = "yo",
            }),
            Dispatch(Used{
                .sequence = 5,
                .guild = 35,
                .command = "leaderboard",
            }),
        },
        ledger, config,
        {
            // Registering in server 33 takes two requests; then the bot asks
            // which server channel 24 is in.
            http::Response{
                .status = 204,
            },
            http::Response{
                .status = 204,
            },
            http::Response{
                .status = 200,
                .body = R"({"id":"24","guild_id":"35"})",
            },
        });

    EXPECT_THAT(
        outcome.actions,
        ElementsAre(kReactionTo2, HasSubstr("Your streak has started"),
                    Reply(3, "✅ `yo` now counts as a GM."),
                    "PUT /channels/24/messages/4/reactions/%F0%9F%91%8E/@me",
                    AllOf(HasSubstr("only GMs belong in this channel!"),
                          Not(HasSubstr("has been reset"))),
                    Reply(5,
                          "🌅 **GM Streak Leaderboard**\\n\\n"
                          "No one has said GM yet. Be the first!")));

    const absl::StatusOr<gm::Standing> here = ledger.community(33).StandingOf(
        44, absl::ToCivilDay(absl::Now(), absl::UTCTimeZone()));
    ABSL_EXPECT_OK(here);
    if (here.ok()) EXPECT_EQ(here->streak, 1);
  }());
}

// The commands belong to servers. Used in a direct message, where there are
// no streaks to report, they say so.
TEST(BotTest, DeclinesCommandsOutsideAServer) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();

    const Outcome outcome = co_await ServeEvents(
        {
            Dispatch(Used{
                .sequence = 2,
                .guild = 0,
                .command = "streak",
            }),
        },
        ledger, InChannel22());

    EXPECT_THAT(outcome.actions,
                ElementsAre(Reply(2, "I only work in a server, sorry.")));
  }());
}

// Discord can go on offering a command the bot has since dropped. Whoever
// tries it gets an answer rather than a spinner.
TEST(BotTest, AnswersCommandsItNoLongerHas) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();

    const Outcome outcome = co_await ServeEvents(
        {
            Dispatch(Used{
                .sequence = 2,
                .command = "gmhelp",
            }),
        },
        ledger, InChannel22());

    EXPECT_THAT(outcome.actions,
                ElementsAre(Reply(2, "I don't know that command any more.")));
  }());
}

// The bot stops only when Discord will not have it back, and says why.
TEST(BotTest, StopsWhenDiscordTurnsItAway) {
  RunOnEventLoop([]() -> Task<> {
    gm::Ledger ledger = NewLedger();

    const Outcome outcome = co_await ServeEvents({}, ledger, InChannel22());

    EXPECT_THAT(outcome.stopped, StatusIs(absl::StatusCode::kUnauthenticated));
  }());
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //bot:bot_test -- --benchmark_filter=all
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
//   ----------------------------------------------------------------------
//   Benchmark            Time             CPU   Iterations UserCounters...
//   ----------------------------------------------------------------------
//   BM_ServeGms       6.05 ms         6.05 ms           67 items_per_second=42.3337k/s
// End of results.
// clang-format on

Task<> ServeGms(benchmark::State& state) {
  constexpr int kMessages = 256;
  for (auto _ : state) {
    gm::Ledger ledger = NewLedger();
    std::vector<std::string> dispatches;
    dispatches.reserve(kMessages);
    for (int i = 0; i < kMessages; ++i) {
      dispatches.push_back(Dispatch(Said{
          .sequence = i + 2,
          .author = 1000 + i,
          .content = "gm everyone",
      }));
    }
    benchmark::DoNotOptimize(
        co_await ServeEvents(std::move(dispatches), ledger, InChannel22()));
  }
  state.SetItemsProcessed(state.iterations() * kMessages);
}

// The whole bot handling a morning's GMs, everything but the network:
// parsing each event, the ledger (in memory), and composing both replies.
void BM_ServeGms(benchmark::State& state) {
  EventLoop::Create()->Run(ServeGms(state));
}
BENCHMARK(BM_ServeGms)->Unit(benchmark::kMillisecond);

}  // namespace
