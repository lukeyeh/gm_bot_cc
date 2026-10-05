#include "discord/client.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "async/status_macros.h"
#include "async/task.h"
#include "discord/gateway.h"
#include "discord/model.h"
#include "discord/rest.h"
#include "discord/wire.h"
#include "http/client.h"
#include "websocket/websocket.h"

namespace discord {

using discord_internal::Dispatch;
using discord_internal::Gateway;
using discord_internal::Intent;
using discord_internal::Rest;

Client::Client(std::unique_ptr<http::Client> http, std::unique_ptr<Rest> rest,
               std::unique_ptr<Gateway> gateway, User self,
               ApplicationId application)
    : http_(std::move(http)),
      rest_(std::move(rest)),
      gateway_(std::move(gateway)),
      self_(std::move(self)),
      application_(application) {}
Client::Client(Client&&) = default;
Client& Client::operator=(Client&&) = default;
Client::~Client() = default;

Task<absl::StatusOr<Client>> Client::Connect(std::string token) {
  co_return co_await Connect(std::move(token), http::NewClient(),
                             &websocket::Connect);
}

Task<absl::StatusOr<Client>> Client::Connect(std::string token,
                                             std::unique_ptr<http::Client> http,
                                             websocket::Connector connect) {
  auto rest = std::make_unique<Rest>(http.get(), token);
  CO_ASSIGN_OR_RETURN(std::string url, co_await rest->GatewayUrl());

  // What the events this client reports are made from.
  const std::vector<Intent> intents = {
      Intent::kGuilds,
      Intent::kGuildMessages,
      Intent::kMessageContent,
  };
  auto gateway = std::make_unique<Gateway>(std::move(connect), std::move(url),
                                           std::move(token), intents);

  // The first dispatch is READY, which says who the bot is.
  CO_ASSIGN_OR_RETURN(const Dispatch ready, co_await gateway->Next());

  User self = discord_internal::ParseUser(ready.data["user"]);
  const ApplicationId application =
      discord_internal::ParseApplicationId(ready.data, self);

  co_return Client(std::move(http), std::move(rest), std::move(gateway),
                   std::move(self), application);
}

Task<absl::StatusOr<Event>> Client::NextEvent() {
  for (;;) {
    CO_ASSIGN_OR_RETURN(const Dispatch dispatch, co_await gateway_->Next());

    std::optional<Event> event =
        discord_internal::ParseEvent(dispatch.type, dispatch.data);
    if (event.has_value()) co_return std::move(*event);
  }
}

Task<absl::Status> Client::Send(ChannelId channel, std::string_view text) {
  co_return co_await rest_->CreateMessage(channel, text);
}

Task<absl::Status> Client::React(const Message& message,
                                 std::string_view emoji) {
  co_return co_await rest_->AddReaction(message.channel, message.id, emoji);
}

Task<absl::StatusOr<GuildId>> Client::GuildOf(ChannelId channel) {
  co_return co_await rest_->GuildOf(channel);
}

Task<absl::Status> Client::OfferCommands(GuildId guild,
                                         std::span<const Command> commands) {
  // Commands offered in one server appear there at once. Offered everywhere,
  // they take Discord up to an hour to show.
  CO_RETURN_IF_ERROR(
      co_await rest_->SetCommands(application_, guild, commands));

  // Whatever the bot once offered everywhere would otherwise still be there,
  // beside the above.
  co_return co_await rest_->SetCommands(application_, std::nullopt, {});
}

Task<absl::Status> Client::Respond(const CommandInvoked& command,
                                   std::string_view text) {
  co_return co_await rest_->Respond(command.id, command.token, text);
}

}  // namespace discord
