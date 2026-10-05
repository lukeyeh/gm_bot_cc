// What the bot needs to be told before it can run, and where it is told:
// environment variables, so that the token never appears on a command line.

#ifndef BOT_CONFIG_H_
#define BOT_CONFIG_H_

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "discord/model.h"

namespace bot {

struct Config {
  // The bot's Discord token.
  std::string token;

  // The GM channels: the channels the bot watches, where a GM counts and
  // anything else costs its author their streak. At least one. Each server
  // they are in keeps its own streaks and phrases; the usual arrangement is
  // one channel per server.
  std::vector<discord::ChannelId> channels;

  // Whose midnight divides one day's GM from the next.
  absl::TimeZone time_zone;

  // The file the GMs are recorded in.
  std::filesystem::path database;
};

// Looks up a variable by name; nothing if it is not set.
using Variables =
    std::function<std::optional<std::string>(const std::string& name)>;

// The variables of the process's environment.
std::optional<std::string> Environment(const std::string& name);

// Reads the configuration from `variables`:
//
//   DISCORD_TOKEN   required
//   GM_CHANNEL_IDS  required; the ids of the GM channels, as Discord's "Copy
//                   Channel ID" gives them, separated by commas. The older
//                   name GM_CHANNEL_ID is read if this one is not set
//   TIMEZONE        optional; a name such as "America/New_York". UTC if unset
//   DATA_DIR        optional; the directory to keep gm.db in. The current
//                   directory if unset
//
// Fails with InvalidArgument, naming the variable, if one is missing or
// makes no sense.
absl::StatusOr<Config> LoadConfig(const Variables& variables = &Environment);

}  // namespace bot

#endif  // BOT_CONFIG_H_
