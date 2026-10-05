#include "bot/config.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/time/time.h"
#include "discord/model.h"

namespace bot {

std::optional<std::string> Environment(const std::string& name) {
  const char* const value = std::getenv(name.c_str());
  if (value == nullptr) return std::nullopt;

  return std::string(value);
}

absl::StatusOr<Config> LoadConfig(const Variables& variables) {
  // Set but empty means the same as not set, which is what a line like
  // `GM_CHANNEL_ID=` in an environment file is taken to say.
  const auto lookup = [&variables](const std::string& name) {
    return variables(name).value_or("");
  };

  Config config;

  config.token = lookup("DISCORD_TOKEN");
  if (config.token.empty()) {
    return absl::InvalidArgumentError("DISCORD_TOKEN is not set");
  }

  // Required, because of what the bot does to anyone who says something
  // other than GM where it is watching: that must not be everywhere.
  std::string channels = lookup("GM_CHANNEL_IDS");
  if (channels.empty()) channels = lookup("GM_CHANNEL_ID");
  if (channels.empty()) {
    return absl::InvalidArgumentError("GM_CHANNEL_IDS is not set");
  }

  for (const std::string_view channel :
       absl::StrSplit(channels, ',', absl::SkipWhitespace())) {
    uint64_t id = 0;
    if (!absl::SimpleAtoi(absl::StripAsciiWhitespace(channel), &id) ||
        id == 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("GM_CHANNEL_IDS has something that is not a channel "
                       "id: ",
                       channel));
    }

    config.channels.push_back(discord::ChannelId{
        .value = id,
    });
  }

  const std::string time_zone = lookup("TIMEZONE");
  if (time_zone.empty()) {
    config.time_zone = absl::UTCTimeZone();
  } else if (!absl::LoadTimeZone(time_zone, &config.time_zone)) {
    return absl::InvalidArgumentError(
        absl::StrCat("TIMEZONE is not a time zone: ", time_zone));
  }

  const std::string directory = lookup("DATA_DIR");
  config.database =
      std::filesystem::path(directory.empty() ? "." : directory) / "gm.db";

  return config;
}

}  // namespace bot
