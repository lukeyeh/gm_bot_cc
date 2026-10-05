// The things a Discord bot deals in: users, channels, messages, and the
// events that tell it something happened.

#ifndef DISCORD_MODEL_H_
#define DISCORD_MODEL_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "absl/strings/str_cat.h"

namespace discord {

// Discord identifies everything by a 64-bit number. Each kind of thing gets
// its own type here, so that a channel cannot be passed where a user is meant.
template <typename Kind>
struct Id {
  uint64_t value = 0;

  friend bool operator==(Id, Id) = default;
};

using UserId = Id<struct UserKind>;
using ChannelId = Id<struct ChannelKind>;
using MessageId = Id<struct MessageKind>;
using GuildId = Id<struct GuildKind>;  // A server.
using ApplicationId = Id<struct ApplicationKind>;
using InteractionId = Id<struct InteractionKind>;

struct User {
  UserId id;
  // What to call them where the message was sent: their nickname in that
  // server if they have one, otherwise their display name or username.
  std::string name;
  // Whether this is a bot rather than a person.
  bool bot = false;
};

struct Message {
  MessageId id;
  ChannelId channel;
  // The server the channel is in. The id 0 for a direct message, which is in
  // none.
  GuildId guild;
  User author;
  std::string content;
};

// Text that, in a message, appears as a link to `user` and notifies them.
inline std::string Mention(UserId user) {
  return absl::StrCat("<@", user.value, ">");
}

// Someone posted a message in a channel the bot can see. Discord's own
// notices in a channel (that someone joined, pinned a message, and the like)
// are not reported.
struct MessageCreated {
  Message message;
};

// What kind of value a command's option takes.
enum class OptionType { kInteger, kText };

// Something whoever uses a command can fill in.
struct Option {
  // As typed: lower case, no spaces.
  std::string name;
  // Shown beside the option as it is filled in.
  std::string description;
  OptionType type = OptionType::kText;
  // Whether the command can be sent without it.
  bool required = false;
};

// A slash command: what appears when someone types "/" where the bot is
// installed.
struct Command {
  // As typed after the slash: lower case, no spaces.
  std::string name;
  // Shown beside the command in Discord's list.
  std::string description;
  std::vector<Option> options;
  // Whether the command is for the server's administrators alone. This is
  // where a server starts from: its own settings can open the command to
  // others or close it further.
  bool administrators_only = false;
};

// An option as someone filled it in.
struct OptionValue {
  std::string name;
  std::variant<int64_t, std::string> value;
};

// Someone used one of the bot's commands. Discord shows them that the bot is
// thinking until it answers with Client::Respond, and gives up on it after
// three seconds.
struct CommandInvoked {
  // Which command: its name, without the slash.
  std::string name;
  User user;
  ChannelId channel;
  // The server it was used in. The id 0 for a direct message.
  GuildId guild;
  // The options they filled in. Those they left out are absent.
  std::vector<OptionValue> options;

  // The value given for the integer option `option`, or `fallback` if it was
  // left out.
  int64_t Integer(std::string_view option, int64_t fallback) const {
    for (const OptionValue& given : options) {
      const int64_t* const integer = std::get_if<int64_t>(&given.value);
      if (given.name == option && integer != nullptr) return *integer;
    }

    return fallback;
  }

  // The value given for the text option `option`, or nothing if it was left
  // out.
  std::string Text(std::string_view option) const {
    for (const OptionValue& given : options) {
      const std::string* const text = std::get_if<std::string>(&given.value);
      if (given.name == option && text != nullptr) return *text;
    }

    return "";
  }

  // Discord's handle on this use of the command, which is what Respond
  // answers. Of no other interest.
  InteractionId id;
  std::string token;
};

// Something that happened. More kinds will join this as the client learns to
// report them; handle the ones you care about and ignore the rest.
using Event = std::variant<MessageCreated, CommandInvoked>;

}  // namespace discord

#endif  // DISCORD_MODEL_H_
