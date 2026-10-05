// A Discord bot's connection to Discord.
//
//   CO_ASSIGN_OR_RETURN(discord::Client client,
//                       co_await discord::Client::Connect(token));
//   for (;;) {
//     CO_ASSIGN_OR_RETURN(const discord::Event event,
//                         co_await client.NextEvent());
//
//     if (const auto* created = std::get_if<discord::MessageCreated>(&event)) {
//       CO_RETURN_IF_ERROR(co_await client.React(created->message, "👋"));
//     }
//   }
//
// That is the whole interface: connect, hear what happens, act. (Slash
// commands are the same three steps: OfferCommands, a CommandInvoked event,
// Respond.) The two
// protocols Discord splits this across (a WebSocket gateway for events, an
// HTTP API for actions), and everything needed to keep them working, such as
// heartbeats, reconnecting, resuming missed events and waiting out rate
// limits, are the client's business and do not appear here.
//
// Everything is asynchronous (see async/task.h) and must be called from a
// task running on an EventLoop. A client is used by one task at a time.

#ifndef DISCORD_CLIENT_H_
#define DISCORD_CLIENT_H_

#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "discord/model.h"
#include "http/client.h"
#include "websocket/websocket.h"

namespace discord_internal {
class Gateway;
class Rest;
}  // namespace discord_internal

namespace discord {

class Client {
 public:
  // Logs in as the bot whose `token` this is (from the Discord Developer
  // Portal) and evaluates to a client once Discord is ready to send events.
  //
  // The bot will be told about messages in the servers it has been invited
  // to, including their text, which requires the Message Content intent to
  // be enabled for it in the portal.
  //
  // Fails with Unauthenticated if Discord does not accept the token,
  // PermissionDenied if the intent has not been enabled, and Unavailable if
  // Discord cannot be reached.
  static Task<absl::StatusOr<Client>> Connect(std::string token);

  // As above, but reaching Discord through `http` and `connect` instead of
  // the network: the way to test a bot against a scripted Discord.
  static Task<absl::StatusOr<Client>> Connect(
      std::string token, std::unique_ptr<http::Client> http,
      websocket::Connector connect);

  Client(Client&&);
  Client& operator=(Client&&);
  ~Client();

  // The bot's own user. Its messages are reported like anyone else's, so
  // this is how to recognise them.
  const User& self() const { return self_; }

  // Waits for the next thing to happen.
  //
  // Losing the connection to Discord is not a failure: the client reconnects
  // and carries on, without dropping events where Discord allows. NextEvent
  // fails only if Discord turns the bot away for good, as Connect would.
  //
  // Call it again promptly after handling each event, since it is also what
  // keeps the connection alive.
  Task<absl::StatusOr<Event>> NextEvent();

  // Posts `text` in `channel`. Fails with PermissionDenied if the bot may
  // not post there, NotFound if there is no such channel, and Unavailable if
  // Discord cannot be reached.
  Task<absl::Status> Send(ChannelId channel, std::string_view text);

  // Adds `emoji`, a Unicode emoji such as "🌅", to `message` as a reaction
  // from the bot. Fails as Send does.
  Task<absl::Status> React(const Message& message, std::string_view emoji);

  // Which server `channel` is in. Fails with NotFound or PermissionDenied if
  // the bot cannot see the channel, and FailedPrecondition if it is not a
  // channel of a server.
  Task<absl::StatusOr<GuildId>> GuildOf(ChannelId channel);

  // Makes `commands` the slash commands this bot offers in `guild`, in place
  // of whatever it offered there before, and withdraws any it once offered
  // in every server at once. They can be used straight away. Each use of one
  // arrives from NextEvent as a CommandInvoked.
  //
  // Fails with InvalidArgument if Discord finds fault with a command's name
  // or description.
  Task<absl::Status> OfferCommands(GuildId guild,
                                   std::span<const Command> commands);

  // Answers `command` with a message saying `text`, which everyone in the
  // channel can see. Mentions in it appear as usual but notify nobody.
  //
  // Discord waits three seconds for this. Fails with NotFound if it comes
  // later than that, or twice.
  Task<absl::Status> Respond(const CommandInvoked& command,
                             std::string_view text);

 private:
  Client(std::unique_ptr<http::Client> http,
         std::unique_ptr<discord_internal::Rest> rest,
         std::unique_ptr<discord_internal::Gateway> gateway, User self,
         ApplicationId application);

  // On the heap so that the client can be moved while they refer to each
  // other.
  std::unique_ptr<http::Client> http_;
  std::unique_ptr<discord_internal::Rest> rest_;
  std::unique_ptr<discord_internal::Gateway> gateway_;
  User self_;
  // What Discord calls the bot when it comes to its commands.
  ApplicationId application_;
};

}  // namespace discord

#endif  // DISCORD_CLIENT_H_
