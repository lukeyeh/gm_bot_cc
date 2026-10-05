#include "bot/bot.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/time/civil_time.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "async/status_macros.h"
#include "async/task.h"
#include "bot/config.h"
#include "discord/client.h"
#include "discord/model.h"
#include "gm/greeting.h"
#include "gm/ledger.h"
#include "gm/phrase.h"
#include "gm/report.h"
#include "net/event_loop.h"

namespace bot {
namespace {

// How many people /leaderboard shows when not told, and at most.
constexpr int64_t kDefaultBoardSize = 10;
constexpr int64_t kMaxBoardSize = 25;

// Whether `message` is the bot's business: something a person wrote in a GM
// channel.
bool IsWatched(const discord::Message& message, const Config& config) {
  // Bots, this one included, do not have mornings.
  if (message.author.bot) return false;

  return std::ranges::find(config.channels, message.channel) !=
         config.channels.end();
}

// Records the GM that `message` is and answers it.
Task<absl::Status> HandleGm(discord::Client& client, gm::Community community,
                            absl::CivilDay today,
                            const discord::Message& message) {
  CO_ASSIGN_OR_RETURN(const gm::Receipt receipt,
                      community.Record(
                          gm::Member{
                              .id = message.author.id.value,
                              .name = message.author.name,
                          },
                          today));

  CO_RETURN_IF_ERROR(co_await client.React(message, gm::kAcknowledgement));

  const std::optional<std::string> announcement =
      gm::Announcement(receipt, discord::Mention(message.author.id));
  if (announcement.has_value()) {
    CO_RETURN_IF_ERROR(co_await client.Send(message.channel, *announcement));
  }

  co_return absl::OkStatus();
}

// Deals with a `message` in the GM channel that is not a GM: its author
// forfeits their streak and is told so, and reminded of the `phrases` that
// would have counted.
Task<absl::Status> HandleIntrusion(discord::Client& client,
                                   gm::Community community,
                                   absl::CivilDay today,
                                   const discord::Message& message,
                                   std::span<const std::string> phrases) {
  CO_ASSIGN_OR_RETURN(const int forfeited,
                      community.Forfeit(message.author.id.value, today));

  CO_RETURN_IF_ERROR(co_await client.React(message, gm::kDisapproval));

  co_return co_await client.Send(
      message.channel,
      gm::Rebuke(forfeited, discord::Mention(message.author.id), phrases));
}

// Handles something a person wrote in the GM channel.
Task<absl::Status> HandleMessage(discord::Client& client,
                                 gm::Community community, const Config& config,
                                 const discord::Message& message) {
  const absl::CivilDay today = absl::ToCivilDay(absl::Now(), config.time_zone);

  // Read each time, so that a phrase counts from the moment it is added.
  CO_ASSIGN_OR_RETURN(const std::vector<std::string> phrases,
                      community.Phrases());

  if (gm::IsGm(message.content, phrases)) {
    co_return co_await HandleGm(client, community, today, message);
  }

  co_return co_await HandleIntrusion(client, community, today, message,
                                     phrases);
}

// The slash commands the bot offers. HandleCommand answers each.
std::vector<discord::Command> Commands() {
  return {
      discord::Command{
          .name = "leaderboard",
          .description = "Show the GM leaderboard",
          .options =
              {
                  discord::Option{
                      .name = "limit",
                      .description =
                          "How many people to show (1-25, default 10)",
                      .type = discord::OptionType::kInteger,
                      .required = false,
                  },
              },
      },
      discord::Command{
          .name = "streak",
          .description = "Check your current GM streak",
      },
      discord::Command{
          .name = "gmlist",
          .description = "Show what phrases count as GM",
      },
      discord::Command{
          .name = "gmadd",
          .description = "Add a phrase that counts as GM (admin only)",
          .options =
              {
                  discord::Option{
                      .name = "phrase",
                      .description = "The phrase to add",
                      .type = discord::OptionType::kText,
                      .required = true,
                  },
              },
          .administrators_only = true,
      },
      discord::Command{
          .name = "gmremove",
          .description = "Remove a phrase that counts as GM (admin only)",
          .options =
              {
                  discord::Option{
                      .name = "phrase",
                      .description = "The phrase to remove",
                      .type = discord::OptionType::kText,
                      .required = true,
                  },
              },
          .administrators_only = true,
      },
  };
}

// Answers /gmadd: the phrase is added, or the reason it cannot be is given.
Task<absl::Status> HandleGmAdd(discord::Client& client, gm::Community community,
                               const discord::CommandInvoked& command) {
  const std::string proposed = command.Text("phrase");
  const absl::StatusOr<bool> added = community.AddPhrase(proposed);

  // An unacceptable phrase is the user's to hear about, not a failure.
  if (absl::IsInvalidArgument(added.status())) {
    co_return co_await client.Respond(
        command, absl::StrCat("❌ ", added.status().message()));
  }
  CO_RETURN_IF_ERROR(added.status());

  // Shown as it was stored, which is what will be matched.
  CO_ASSIGN_OR_RETURN(const std::string phrase, gm::CanonicalPhrase(proposed));
  co_return co_await client.Respond(command,
                                    gm::PhraseAddedReport(phrase, *added));
}

// Answers /gmremove.
Task<absl::Status> HandleGmRemove(discord::Client& client,
                                  gm::Community community,
                                  const discord::CommandInvoked& command) {
  const std::string proposed = command.Text("phrase");
  CO_ASSIGN_OR_RETURN(const bool removed, community.RemovePhrase(proposed));

  // In its stored form if it has one; as typed if it could never have been
  // a phrase.
  const std::string phrase = gm::CanonicalPhrase(proposed).value_or(proposed);
  co_return co_await client.Respond(command,
                                    gm::PhraseRemovedReport(phrase, removed));
}

// Answers someone's use of one of the commands above.
Task<absl::Status> HandleCommand(discord::Client& client,
                                 gm::Community community, const Config& config,
                                 const discord::CommandInvoked& command) {
  const absl::CivilDay today = absl::ToCivilDay(absl::Now(), config.time_zone);

  if (command.name == "leaderboard") {
    const int limit = static_cast<int>(std::clamp<int64_t>(
        command.Integer("limit", kDefaultBoardSize), 1, kMaxBoardSize));
    CO_ASSIGN_OR_RETURN(const std::vector<gm::Standing> board,
                        community.Leaderboard(today, limit));

    co_return co_await client.Respond(command, gm::LeaderboardReport(board));
  }

  if (command.name == "streak") {
    CO_ASSIGN_OR_RETURN(const gm::Standing standing,
                        community.StandingOf(command.user.id.value, today));

    co_return co_await client.Respond(
        command, gm::StreakReport(standing, discord::Mention(command.user.id)));
  }

  if (command.name == "gmlist") {
    CO_ASSIGN_OR_RETURN(const std::vector<std::string> phrases,
                        community.Phrases());

    co_return co_await client.Respond(command, gm::PhraseListReport(phrases));
  }

  if (command.name == "gmadd") {
    co_return co_await HandleGmAdd(client, community, command);
  }

  if (command.name == "gmremove") {
    co_return co_await HandleGmRemove(client, community, command);
  }

  // A command from an earlier version of the bot, which Discord has yet to
  // forget.
  co_return co_await client.Respond(command,
                                    "I don't know that command any more.");
}

// Gets things ready for serving: finds the server of each GM channel, which
// also shows that the bot can see the channel, and offers the commands there.
Task<absl::Status> Prepare(discord::Client& client, gm::Ledger& ledger,
                           const Config& config) {
  const std::vector<discord::Command> commands = Commands();

  for (const discord::ChannelId channel : config.channels) {
    CO_ASSIGN_OR_RETURN(
        const discord::GuildId guild, co_await client.GuildOf(channel),
        _.SetPrepend() << "GM channel " << channel.value << ": ");

    // What a ledger from before the bot served several servers holds can
    // only have come from the one it then served, which is listed first.
    // After the first time there is nothing left for this to do.
    if (channel == config.channels.front()) {
      CO_RETURN_IF_ERROR(ledger.ClaimUndivided(guild.value));
    }

    CO_RETURN_IF_ERROR(co_await client.OfferCommands(guild, commands))
        << "registering the bot's commands";
    LOG(INFO) << "watching channel " << channel.value << " in server "
              << guild.value;
  }

  co_return absl::OkStatus();
}

// Everything Run does that has to happen on an event loop.
Task<absl::Status> ConnectAndServe(const Config& config, gm::Ledger& ledger) {
  CO_ASSIGN_OR_RETURN(discord::Client client,
                      co_await discord::Client::Connect(config.token),
                      _.SetPrepend() << "connecting to Discord: ");
  LOG(INFO) << "connected to Discord as " << client.self().name;

  co_return co_await Serve(client, ledger, config);
}

}  // namespace

Task<absl::Status> Serve(discord::Client& client, gm::Ledger& ledger,
                         const Config& config) {
  CO_RETURN_IF_ERROR(co_await Prepare(client, ledger, config));

  for (;;) {
    CO_ASSIGN_OR_RETURN(const discord::Event event,
                        co_await client.NextEvent());

    // Each server is a community of its own in the ledger.
    if (const auto* const created =
            std::get_if<discord::MessageCreated>(&event)) {
      const discord::Message& message = created->message;
      if (!IsWatched(message, config)) continue;

      const absl::Status handled = co_await HandleMessage(
          client, ledger.community(message.guild.value), config, message);
      if (!handled.ok()) {
        LOG(ERROR) << "could not handle a message from " << message.author.name
                   << ": " << handled;
      }
    } else if (const auto* const invoked =
                   std::get_if<discord::CommandInvoked>(&event)) {
      const absl::Status handled =
          invoked->guild.value == 0
              ? co_await client.Respond(*invoked,
                                        "I only work in a server, sorry.")
              : co_await HandleCommand(client,
                                       ledger.community(invoked->guild.value),
                                       config, *invoked);
      if (!handled.ok()) {
        LOG(ERROR) << "could not answer /" << invoked->name << " from "
                   << invoked->user.name << ": " << handled;
      }
    }
  }
}

absl::Status Run(const Config& config) {
  ABSL_ASSIGN_OR_RETURN(gm::Ledger ledger, gm::Ledger::Open(config.database),
                        _.SetPrepend() << "opening the ledger: ");
  LOG(INFO) << "recording GMs in " << config.database;

  ABSL_ASSIGN_OR_RETURN(EventLoop loop, EventLoop::Create());
  return loop.Run(ConnectAndServe(config, ledger));
}

}  // namespace bot
