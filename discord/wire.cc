#include "discord/wire.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "discord/model.h"
#include "json/json.h"

namespace discord_internal {
namespace {

// Discord's numbers for the kinds of thing this file reads and writes.
constexpr int64_t kSlashCommand = 1;        // An application command's type.
constexpr int64_t kCommandInteraction = 2;  // An interaction's type.
constexpr int64_t kTextOption = 3;          // A command option's type.
constexpr int64_t kIntegerOption = 4;
// The permission that marks a server's administrators, as Discord writes
// permissions: a number in a string.
constexpr std::string_view kAdministratorPermission = "8";
constexpr int64_t kOrdinaryMessage = 0;  // A message's type.
constexpr int64_t kReplyMessage = 19;

// Ids are written as strings of digits, since JSON numbers cannot be trusted
// with 64 bits. Anything else reads as 0, which is never a real id.
template <typename Kind>
discord::Id<Kind> ParseId(const json::Value& value) {
  uint64_t id = 0;
  if (!absl::SimpleAtoi(value.AsString(), &id)) id = 0;
  return discord::Id<Kind>{
      .value = id,
  };
}

// The first of `candidates` that is not empty.
std::string FirstNonEmpty(std::initializer_list<std::string_view> candidates) {
  for (const std::string_view candidate : candidates) {
    if (!candidate.empty()) return std::string(candidate);
  }
  return "";
}

}  // namespace

discord::User ParseUser(const json::Value& user, const json::Value& member) {
  return discord::User{
      .id = ParseId<discord::UserKind>(user["id"]),
      .name = FirstNonEmpty({
          member["nick"].AsString(),
          user["global_name"].AsString(),
          user["username"].AsString(),
      }),
      .bot = user["bot"].AsBool(),
  };
}

discord::ApplicationId ParseApplicationId(const json::Value& ready,
                                          const discord::User& self) {
  const discord::ApplicationId stated =
      ParseId<discord::ApplicationKind>(ready["application"]["id"]);
  if (stated.value != 0) return stated;

  // A bot's application and its user have had the same id for years, so the
  // user's will do if Discord ever leaves the application's out.
  return discord::ApplicationId{
      .value = self.id.value,
  };
}

discord::GuildId ParseGuildOfChannel(const json::Value& channel) {
  return ParseId<discord::GuildKind>(channel["guild_id"]);
}

absl::StatusOr<discord::Message> ParseMessage(const json::Value& message) {
  discord::Message parsed{
      .id = ParseId<discord::MessageKind>(message["id"]),
      .channel = ParseId<discord::ChannelKind>(message["channel_id"]),
      .guild = ParseId<discord::GuildKind>(message["guild_id"]),
      .author = ParseUser(message["author"], message["member"]),
      .content = message["content"].AsString(),
  };

  if (parsed.id.value == 0 || parsed.channel.value == 0 ||
      parsed.author.id.value == 0) {
    return absl::InvalidArgumentError(
        "message without an id, a channel or an author");
  }

  return parsed;
}

json::Value FormatCommands(std::span<const discord::Command> commands) {
  json::Value list = json::Value::Array();
  for (const discord::Command& command : commands) {
    json::Value options = json::Value::Array();
    for (const discord::Option& option : command.options) {
      options.Append(
          json::Value()
              .Set("name", option.name)
              .Set("description", option.description)
              .Set("type", option.type == discord::OptionType::kInteger
                               ? kIntegerOption
                               : kTextOption)
              .Set("required", option.required));
    }

    json::Value described = json::Value()
                                .Set("type", kSlashCommand)
                                .Set("name", command.name)
                                .Set("description", command.description)
                                .Set("options", std::move(options));
    if (command.administrators_only) {
      // Who may use a command by default is given as the permissions they
      // must hold.
      described.Set("default_member_permissions", kAdministratorPermission);
    }

    list.Append(std::move(described));
  }

  return list;
}

namespace {

std::optional<discord::Event> ParseMessageCreated(const json::Value& data) {
  // Discord also announces joins, pins, boosts and so on as messages, with
  // whoever caused them as the author. Those are not something anyone said.
  const int64_t type = data["type"].AsInt();
  if (type != kOrdinaryMessage && type != kReplyMessage) return std::nullopt;

  absl::StatusOr<discord::Message> message = ParseMessage(data);
  if (!message.ok()) {
    LOG(WARNING) << "ignoring a malformed message: "
                 << message.status().message();
    return std::nullopt;
  }

  return discord::MessageCreated{
      .message = std::move(*message),
  };
}

std::optional<discord::Event> ParseInteraction(const json::Value& data) {
  // Buttons, menus and the like also arrive as interactions.
  if (data["type"].AsInt() != kCommandInteraction) return std::nullopt;

  // In a server the user comes wrapped in their membership of it; in a
  // direct message they come bare.
  const json::Value& member = data["member"];
  discord::CommandInvoked invoked{
      .name = data["data"]["name"].AsString(),
      .user = member.is_null() ? ParseUser(data["user"])
                               : ParseUser(member["user"], member),
      .channel = ParseId<discord::ChannelKind>(data["channel_id"]),
      .guild = ParseId<discord::GuildKind>(data["guild_id"]),
      .id = ParseId<discord::InteractionKind>(data["id"]),
      .token = data["token"].AsString(),
  };
  if (invoked.name.empty() || invoked.id.value == 0 || invoked.token.empty()) {
    LOG(WARNING) << "ignoring a malformed command interaction";
    return std::nullopt;
  }

  for (const json::Value& option : data["data"]["options"].items()) {
    const int64_t type = option["type"].AsInt();
    if (type == kIntegerOption) {
      invoked.options.push_back(discord::OptionValue{
          .name = option["name"].AsString(),
          .value = option["value"].AsInt(),
      });
    } else if (type == kTextOption) {
      invoked.options.push_back(discord::OptionValue{
          .name = option["name"].AsString(),
          .value = option["value"].AsString(),
      });
    }
  }

  return invoked;
}

}  // namespace

std::optional<discord::Event> ParseEvent(std::string_view type,
                                         const json::Value& data) {
  if (type == "MESSAGE_CREATE") return ParseMessageCreated(data);
  if (type == "INTERACTION_CREATE") return ParseInteraction(data);

  return std::nullopt;
}

}  // namespace discord_internal
