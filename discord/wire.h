// Internal to //discord: how Discord's objects are written in JSON. The only
// place that knows their field names.

#ifndef DISCORD_WIRE_H_
#define DISCORD_WIRE_H_

#include <optional>
#include <span>
#include <string_view>

#include "absl/status/statusor.h"
#include "discord/model.h"
#include "json/json.h"

namespace discord_internal {

// A user object, and, where the user is seen as a member of a server, the
// member object that goes with it.
discord::User ParseUser(const json::Value& user,
                        const json::Value& member = {});

// Which application the bot is, from the data of the READY dispatch that
// starts a session, in which `self` is the bot's user.
discord::ApplicationId ParseApplicationId(const json::Value& ready,
                                          const discord::User& self);

// Which server a channel object says the channel is in, or the id 0 if it
// does not say, as the channel of a direct message does not.
discord::GuildId ParseGuildOfChannel(const json::Value& channel);

// A message object. Fails with InvalidArgument if it lacks what identifies a
// message: its id, its channel, its author.
absl::StatusOr<discord::Message> ParseMessage(const json::Value& message);

// The commands as Discord wants them described when they are registered.
json::Value FormatCommands(std::span<const discord::Command> commands);

// The event that a gateway dispatch of this `type` with this `data` reports,
// or nothing if it is of a kind the client does not report or is malformed.
std::optional<discord::Event> ParseEvent(std::string_view type,
                                         const json::Value& data);

}  // namespace discord_internal

#endif  // DISCORD_WIRE_H_
