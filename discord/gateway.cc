#include "discord/gateway.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/log/log.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/strip.h"
#include "async/sequence.h"
#include "async/status_macros.h"
#include "async/task.h"
#include "json/json.h"
#include "net/event_loop.h"
#include "websocket/websocket.h"

namespace discord_internal {
namespace {

// The kinds of gateway message, as numbered by Discord.
constexpr int64_t kDispatch = 0;
constexpr int64_t kHeartbeat = 1;
constexpr int64_t kIdentify = 2;
constexpr int64_t kResume = 6;
constexpr int64_t kReconnect = 7;
constexpr int64_t kInvalidSession = 9;
constexpr int64_t kHello = 10;
constexpr int64_t kHeartbeatAck = 11;

// How long Discord has to greet a new connection.
constexpr std::chrono::seconds kHelloTimeout(30);

// The longest wait between attempts to reconnect.
constexpr std::chrono::milliseconds kMaxBackoff = std::chrono::minutes(1);

// How long to wait before attempt number `attempts` + 1: one second, doubling.
std::chrono::milliseconds Backoff(int attempts) {
  const int doublings = std::min(attempts - 1, 6);
  return std::min<std::chrono::milliseconds>(
      std::chrono::seconds(1) * (1 << doublings), kMaxBackoff);
}

// Why Discord closing the connection with this code means there is no point
// in connecting again, or OK if there is.
absl::Status VerdictOf(const websocket::Close& close) {
  switch (close.code) {
    case 4004:
      return absl::UnauthenticatedError("Discord rejected the bot's token");
    case 4010:
    case 4011:
      return absl::FailedPreconditionError(
          "Discord requires this bot to be sharded");
    case 4012:
      return absl::FailedPreconditionError(
          "Discord no longer supports this gateway version");
    case 4013:
    case 4014:
      return absl::PermissionDeniedError(
          "Discord will not grant the intents this bot asks for; enable the "
          "Message Content intent for it in the Discord Developer Portal");
    default: return absl::OkStatus();
  }
}

// Whether the session survives a close with this code, to be resumed on a
// new connection.
bool SessionSurvives(const websocket::Close& close) {
  switch (close.code) {
    case 1000:  // Normal closure,
    case 1001:  // and going away: Discord takes both as "the bot is done".
    case 4007:  // The sequence number we resumed with was wrong.
    case 4009:  // The session timed out.
      return false;
    default: return true;
  }
}

}  // namespace

Task<> Gateway::RealPause(std::chrono::milliseconds duration) {
  co_await Sleep(duration);
}

Gateway::Gateway(websocket::Connector connect, std::string url,
                 std::string token, const std::vector<Intent>& intents,
                 Pause pause)
    : connect_(std::move(connect)),
      url_(std::move(url)),
      token_(std::move(token)),
      pause_(std::move(pause)),
      dispatches_(Dispatches()) {
  for (const Intent intent : intents) {
    intents_ |= static_cast<uint32_t>(intent);
  }
}

Task<absl::StatusOr<Dispatch>> Gateway::Next() {
  std::optional<absl::StatusOr<Dispatch>> next = co_await dispatches_.Next();

  // The sequence ends only after yielding the reason it cannot go on, which
  // is the answer from then on.
  if (!next.has_value()) co_return ended_;
  if (!next->ok()) ended_ = next->status();

  co_return std::move(*next);
}

Sequence<absl::StatusOr<Dispatch>> Gateway::Dispatches() {
  // Each turn of this loop is one connection.
  for (;;) {
    // Straight away the first time; then more and more patiently, until a
    // dispatch shows that a connection is working again.
    if (attempts_ > 0) co_await pause_(Backoff(attempts_));
    ++attempts_;

    // Why this connection came to an end, once it has.
    absl::Status ending;
    absl::StatusOr<Link> link = co_await Connect();
    if (!link.ok()) ending = link.status();

    // Each turn of this one is a message from Discord, or a heartbeat when
    // one is due.
    while (ending.ok()) {
      // Checked here as well as on a quiet connection, because a busy one
      // never leaves Receive waiting long enough to time out.
      if (Clock::now() >= link->next_heartbeat) {
        ending = co_await Beat(*link);
        continue;
      }

      const absl::StatusOr<websocket::Incoming> incoming =
          co_await link->connection->Receive(link->next_heartbeat);
      if (absl::IsDeadlineExceeded(incoming.status())) {
        ending = co_await Beat(*link);
        continue;
      }
      if (!incoming.ok()) {
        ending = incoming.status();
        continue;
      }

      if (const auto* const close = std::get_if<websocket::Close>(&*incoming)) {
        // Some closes mean "do not come back".
        if (const absl::Status verdict = VerdictOf(*close); !verdict.ok()) {
          co_yield verdict;
          co_return;
        }

        if (!SessionSurvives(*close)) ForgetSession();
        ending = absl::UnavailableError(
            absl::StrCat("gateway closed the connection, code ", close->code));
        continue;
      }

      absl::StatusOr<json::Value> payload =
          json::Parse(std::get<std::string>(*incoming));
      if (!payload.ok()) {
        LOG(WARNING) << "ignoring a gateway message that is not JSON";
        continue;
      }

      switch ((*payload)["op"].AsInt()) {
        case kDispatch: {
          // The data is most of the payload: handed on, not copied.
          Dispatch dispatch{
              .type = (*payload)["t"].AsString(),
              .data = payload->Take("d"),
          };
          sequence_ = (*payload)["s"].AsInt();
          attempts_ = 0;

          if (dispatch.type == "READY") {
            session_ = Session{
                .id = dispatch.data["session_id"].AsString(),
                .resume_url = dispatch.data["resume_gateway_url"].AsString(),
            };
            ever_ready_ = true;
          }

          co_yield std::move(dispatch);
          break;
        }

        case kHeartbeat:
          // Discord may ask for a heartbeat ahead of schedule.
          ending = co_await SendHeartbeat(*link);
          break;

        case kHeartbeatAck: link->acknowledged = true; break;

        case kReconnect:
          // Discord is about to restart this server and wants us elsewhere.
          ending = absl::UnavailableError("gateway asked us to reconnect");
          break;

        case kInvalidSession:
          // The data says whether the session can still be resumed.
          if (!(*payload)["d"].AsBool()) ForgetSession();
          ending = absl::UnavailableError("gateway rejected the session");
          break;

        default: break;
      }
    }

    // The connection is gone. Before the gateway has ever been ready, that
    // more likely means a mistake than a blip, and is the caller's to know.
    // After, it is the gateway's to put right, by going round again.
    if (!ever_ready_) {
      co_yield absl::Status(
          ending.code(),
          absl::StrCat("cannot start a Discord session: ", ending.message()));
      co_return;
    }
    LOG(WARNING) << "lost the Discord gateway (" << ending.message()
                 << "); reconnecting";
  }
}

Task<absl::StatusOr<Gateway::Link>> Gateway::Connect() {
  const std::string_view base = absl::StripSuffix(
      session_.has_value() ? session_->resume_url : url_, "/");
  CO_ASSIGN_OR_RETURN(
      std::unique_ptr<websocket::Connection> connection,
      co_await connect_(absl::StrCat(base, "/?v=10&encoding=json")));

  // Discord speaks first, saying how often it wants to hear a heartbeat.
  CO_ASSIGN_OR_RETURN(
      const websocket::Incoming greeting,
      co_await connection->Receive(Clock::now() + kHelloTimeout));
  if (const auto* const close = std::get_if<websocket::Close>(&greeting)) {
    CO_RETURN_IF_ERROR(VerdictOf(*close));
    co_return absl::UnavailableError(
        absl::StrCat("gateway closed on connecting, code ", close->code));
  }

  const absl::StatusOr<json::Value> hello =
      json::Parse(std::get<std::string>(greeting));
  const int64_t interval =
      hello.ok() ? (*hello)["d"]["heartbeat_interval"].AsInt() : 0;
  if (!hello.ok() || (*hello)["op"].AsInt() != kHello || interval <= 0) {
    co_return absl::UnavailableError("gateway did not greet the connection");
  }

  // Then the bot says who it is: picking up its session if it has one,
  // starting one if not.
  json::Value introduction;
  if (session_.has_value()) {
    introduction.Set("op", kResume)
        .Set("d",
             json::Value()
                 .Set("token", token_)
                 .Set("session_id", session_->id)
                 .Set("seq", sequence_.has_value() ? json::Value(*sequence_)
                                                   : json::Value()));
  } else {
    introduction.Set("op", kIdentify)
        .Set("d", json::Value()
                      .Set("token", token_)
                      .Set("intents", intents_)
                      .Set("properties", json::Value()
                                             .Set("os", "linux")
                                             .Set("browser", "gm_bot")
                                             .Set("device", "gm_bot")));
  }
  CO_RETURN_IF_ERROR(co_await connection->Send(json::Serialize(introduction)));

  // The first heartbeat comes after a random part of the interval, so that
  // bots which all reconnected at once do not all beat at once.
  const std::chrono::milliseconds heartbeat_interval(interval);
  co_return Link{
      .connection = std::move(connection),
      .heartbeat_interval = heartbeat_interval,
      .next_heartbeat =
          Clock::now() +
          std::chrono::duration_cast<Clock::duration>(
              heartbeat_interval * absl::Uniform<double>(random_, 0.0, 1.0)),
  };
}

Task<absl::Status> Gateway::SendHeartbeat(Link& link) {
  const json::Value heartbeat =
      json::Value()
          .Set("op", kHeartbeat)
          .Set("d",
               sequence_.has_value() ? json::Value(*sequence_) : json::Value());
  CO_RETURN_IF_ERROR(
      co_await link.connection->Send(json::Serialize(heartbeat)));

  link.acknowledged = false;
  link.next_heartbeat = Clock::now() + link.heartbeat_interval;
  co_return absl::OkStatus();
}

Task<absl::Status> Gateway::Beat(Link& link) {
  if (!link.acknowledged) {
    // A connection can die without either end being told. Silence where an
    // acknowledgement should be is how that shows.
    co_return absl::UnavailableError("heartbeat went unanswered");
  }

  co_return co_await SendHeartbeat(link);
}

void Gateway::ForgetSession() {
  session_.reset();
  sequence_.reset();
}

}  // namespace discord_internal
