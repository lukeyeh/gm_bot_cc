// Internal to //discord: the Discord gateway, the WebSocket over which
// Discord tells a bot what is happening.
//
// A Gateway turns that into one thing: a sequence of dispatches, each an
// event's name and its data. Everything the protocol requires to keep the
// sequence coming stays inside: identifying, heartbeats, noticing a dead
// connection, reconnecting, and resuming the session so that events sent
// while disconnected are not lost.

#ifndef DISCORD_GATEWAY_H_
#define DISCORD_GATEWAY_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "async/sequence.h"
#include "async/task.h"
#include "json/json.h"
#include "websocket/websocket.h"

namespace discord_internal {

// Kinds of event a bot can ask to be told about. The values are Discord's.
enum class Intent : uint32_t {
  // Servers the bot is in, and their channels.
  kGuilds = 1U << 0,
  // Messages in those servers' channels.
  kGuildMessages = 1U << 9,
  // The text of those messages. Without it they arrive empty. Must also be
  // enabled for the bot in the Discord Developer Portal.
  kMessageContent = 1U << 15,
};

// One event from Discord, as it arrived.
struct Dispatch {
  // Discord's name for the kind of event, such as "MESSAGE_CREATE".
  std::string type;
  json::Value data;
};

class Gateway {
 public:
  // How the gateway waits before trying again. Tests substitute something
  // that does not take real time.
  using Pause = std::function<Task<>(std::chrono::milliseconds)>;

  // `url` is where the gateway is, from Rest::GatewayUrl. Nothing happens
  // until the first call to Next.
  Gateway(websocket::Connector connect, std::string url, std::string token,
          const std::vector<Intent>& intents, Pause pause = &RealPause);

  // What Next is in the middle of refers to this object, so it stays put.
  Gateway(const Gateway&) = delete;
  Gateway& operator=(const Gateway&) = delete;

  // Waits for the next dispatch. The first one is always "READY".
  //
  // Losing the connection is not a failure: once the gateway has been ready,
  // Next reconnects for as long as it takes. It fails only when trying again
  // would not help:
  //   Unauthenticated     Discord does not accept the token.
  //   PermissionDenied    the bot asked for intents it has not been granted.
  //   FailedPrecondition  Discord will not serve this bot as configured.
  //   Unavailable         the gateway could not be reached at all, when it
  //                       has never yet been ready.
  //
  // Call it again promptly after each dispatch: heartbeats are sent from
  // inside it.
  Task<absl::StatusOr<Dispatch>> Next();

 private:
  using Clock = std::chrono::steady_clock;

  // What is needed to pick a session up where it left off.
  struct Session {
    std::string id;
    std::string resume_url;
  };

  // One connection to the gateway, from Discord's greeting until it is lost.
  struct Link {
    std::unique_ptr<websocket::Connection> connection;
    std::chrono::milliseconds heartbeat_interval{0};
    Clock::time_point next_heartbeat;
    // Whether Discord has answered the last heartbeat.
    bool acknowledged = true;
  };

  static Task<> RealPause(std::chrono::milliseconds duration);

  // The gateway's whole life, as the dispatches it produces: connect, start
  // or resume the session, hand out what arrives, and when the connection is
  // lost begin again. It ends only by yielding the reason it cannot go on.
  Sequence<absl::StatusOr<Dispatch>> Dispatches();

  // Connects, hears Discord's greeting, and starts the session or resumes
  // the one there is.
  Task<absl::StatusOr<Link>> Connect();

  // Sends a heartbeat over `link`, or fails if the last one went unanswered,
  // which means the connection has died quietly.
  Task<absl::Status> Beat(Link& link);
  Task<absl::Status> SendHeartbeat(Link& link);

  // Forgets the session, so that the next connection starts a new one.
  void ForgetSession();

  websocket::Connector connect_;
  std::string url_;
  std::string token_;
  uint32_t intents_ = 0;
  Pause pause_;
  absl::BitGen random_;

  // What outlasts any one connection.
  //
  // Set from READY until Discord says the session is over.
  std::optional<Session> session_;
  // The number of the last dispatch received in this session.
  std::optional<int64_t> sequence_;
  // Whether READY has ever arrived.
  bool ever_ready_ = false;
  // Connections attempted since a dispatch last arrived: how the gateway
  // knows to slow down when reconnecting is not getting anywhere.
  int attempts_ = 0;

  // Dispatches(), part way through, and why it ended once it has.
  Sequence<absl::StatusOr<Dispatch>> dispatches_;
  absl::Status ended_ =
      absl::InternalError("the gateway stopped without saying why");
};

}  // namespace discord_internal

#endif  // DISCORD_GATEWAY_H_
