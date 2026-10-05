#include "perf/load.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/time/civil_time.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "async/status_macros.h"
#include "async/task.h"
#include "bot/bot.h"
#include "bot/config.h"
#include "discord/client.h"
#include "discord/model.h"
#include "gm/ledger.h"
#include "http/client.h"
#include "net/event_loop.h"
#include "net/stream.h"
#include "websocket/websocket.h"

namespace {

using Clock = std::chrono::steady_clock;

// The channel and server the bot's events happen in.
constexpr uint64_t kChannel = 22;
constexpr uint64_t kGuild = 33;

int64_t NowNanoseconds() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

// When each event was sent, by its number. Written by the server's thread
// and read by the client's.
class SendTimes {
 public:
  explicit SendTimes(size_t events)
      : times_(std::make_unique<std::atomic<int64_t>[]>(events)) {}

  void MarkSent(size_t event) {
    times_[event].store(NowNanoseconds(), std::memory_order_relaxed);
  }

  std::chrono::nanoseconds Since(size_t event) const {
    return std::chrono::nanoseconds(
        NowNanoseconds() - times_[event].load(std::memory_order_relaxed));
  }

  int64_t at(size_t event) const {
    return times_[event].load(std::memory_order_relaxed);
  }

 private:
  std::unique_ptr<std::atomic<int64_t>[]> times_;
};

// A stream that passes everything to another it does not own, so that the
// server can have websocket::Accept do the handshake and then write frames
// to the stream itself.
class View final : public net::Stream {
 public:
  explicit View(net::Stream* stream) : stream_(stream) {}

  Task<absl::StatusOr<size_t>> Read(std::span<char> buffer,
                                    net::Deadline deadline) override {
    co_return co_await stream_->Read(buffer, deadline);
  }

  Task<absl::Status> Write(std::string_view data) override {
    co_return co_await stream_->Write(data);
  }

 private:
  net::Stream* stream_;
};

// A WebSocket frame as a server sends it: unmasked, in one piece. `opcode` is
// 1 for text and 8 for close.
std::string Frame(uint8_t opcode, std::string_view payload) {
  std::string frame;
  frame.push_back(static_cast<char>(0x80 | opcode));
  if (payload.size() < 126) {
    frame.push_back(static_cast<char>(payload.size()));
  } else {
    frame.push_back(126);
    frame.push_back(static_cast<char>(payload.size() >> 8));
    frame.push_back(static_cast<char>(payload.size() & 0xFF));
  }
  frame.append(payload);
  return frame;
}

// The gateway's side of one connection: greets it, sends `events` at the
// pace asked for, noting when each goes, and then closes the connection the
// way Discord does to a bot whose token has been revoked, which is what
// makes a bot stop.
Task<> PlayGateway(const net::Listener& listener,
                   const std::vector<std::string>& preamble,
                   const std::vector<std::string>& events,
                   std::chrono::microseconds interval, SendTimes& sent) {
  const absl::StatusOr<std::unique_ptr<net::Stream>> stream =
      co_await listener.Accept();
  if (!stream.ok()) co_return;
  if (!(co_await websocket::Accept(std::make_unique<View>(stream->get())))
           .ok()) {
    co_return;
  }

  for (const std::string& message : preamble) {
    if (!(co_await (*stream)->Write(Frame(1, message))).ok()) co_return;
  }

  for (size_t i = 0; i < events.size(); ++i) {
    if (interval.count() > 0) co_await Sleep(interval);
    sent.MarkSent(i);
    if (!(co_await (*stream)->Write(Frame(1, events[i]))).ok()) co_return;
  }

  // Close code 4004, "authentication failed".
  (co_await (*stream)->Write(Frame(8, std::string_view("\x0F\xA4", 2))))
      .IgnoreError();

  // Stay until the client has hung up, so that nothing sent is cut short.
  std::string unread(4096, '\0');
  for (;;) {
    const absl::StatusOr<size_t> count =
        co_await (*stream)->Read(unread, net::After(std::chrono::seconds(30)));
    if (!count.ok() || *count == 0) co_return;
  }
}

// Runs PlayGateway on a thread with an event loop of its own, as a server
// elsewhere would be.
class GatewayThread {
 public:
  GatewayThread(const net::Listener& listener,
                std::vector<std::string> preamble,
                std::vector<std::string> events,
                std::chrono::microseconds interval, SendTimes& sent)
      : preamble_(std::move(preamble)),
        events_(std::move(events)),
        thread_([this, &listener, interval, &sent] {
          absl::StatusOr<EventLoop> loop = EventLoop::Create();
          if (!loop.ok()) return;
          loop->Run(PlayGateway(listener, preamble_, events_, interval, sent));
        }) {}

 private:
  std::vector<std::string> preamble_;
  std::vector<std::string> events_;
  std::jthread thread_;
};

std::string UrlOf(const net::Listener& listener) {
  return absl::StrCat("ws://127.0.0.1:", listener.address().port);
}

// A message event numbered `sequence`, of the size Discord really sends.
std::string MessageEvent(int64_t sequence, uint64_t author,
                         std::string_view content) {
  return absl::StrCat(
      R"({"t":"MESSAGE_CREATE","s":)", sequence,
      R"(,"op":0,"d":{"type":0,"tts":false,)"
      R"("timestamp":"2026-10-05T12:00:00.000000+00:00","pinned":false,)"
      R"("mentions":[],"mention_roles":[],"mention_everyone":false,)"
      R"("member":{"roles":["500000000000000001","500000000000000002"],)"
      R"("premium_since":null,"pending":false,"nick":null,"mute":false,)"
      R"("joined_at":"2021-12-29T20:14:27.585000+00:00","flags":0,)"
      R"("deaf":false,"communication_disabled_until":null,"banner":null,)"
      R"("avatar":null},"id":")",
      sequence,
      R"(","flags":0,"embeds":[],"edited_timestamp":null,"content":")", content,
      R"(","components":[],"channel_type":0,"channel_id":")", kChannel,
      R"(","author":{"username":"member)", author,
      R"(","public_flags":0,"primary_guild":null,"id":")", author,
      R"(","global_name":null,"discriminator":"0","collectibles":null,)"
      R"("clan":null,"avatar_decoration_data":null,)"
      R"("avatar":"8e1f9d0c2b3a4d5e6f708192a3b4c5d6"},"attachments":[],)"
      R"("guild_id":")",
      kGuild, R"("}})");
}

// Someone using the command called `name`.
std::string CommandEvent(int64_t sequence, uint64_t author,
                         std::string_view name) {
  return absl::StrCat(R"({"t":"INTERACTION_CREATE","s":)", sequence,
                      R"(,"op":0,"d":{"type":2,"id":")", sequence,
                      R"(","token":"token-)", sequence, R"(","channel_id":")",
                      kChannel, R"(","guild_id":")", kGuild,
                      R"(","data":{"name":")", name,
                      R"(","options":[]},"member":{"user":{"id":")", author,
                      R"(","username":"member)", author, R"("}}}})");
}

// A morning in a busy GM channel: nine events in ten are someone saying GM
// one way or another, and the rest are split between people saying something
// else and people asking how they are doing.
std::vector<std::string> Morning(const LoadOptions& options) {
  std::mt19937 random(20261005);  // The same morning every time.
  std::uniform_int_distribution<uint64_t> member(1, options.members);
  std::uniform_int_distribution<int> kind(0, 99);

  std::vector<std::string> events;
  events.reserve(options.events);
  for (int i = 0; i < options.events; ++i) {
    // Sequence numbers start at 2, READY being 1.
    const int64_t sequence = i + 2;
    const uint64_t who = member(random);
    const int roll = kind(random);

    if (roll < 60) {
      events.push_back(MessageEvent(sequence, who, "gm"));
    } else if (roll < 90) {
      events.push_back(
          MessageEvent(sequence, who, "Good morning everyone, coffee time"));
    } else if (roll < 95) {
      events.push_back(
          MessageEvent(sequence, who, "did anyone watch the game"));
    } else if (roll < 98) {
      events.push_back(CommandEvent(sequence, who, "streak"));
    } else {
      events.push_back(CommandEvent(sequence, who, "leaderboard"));
    }
  }
  return events;
}

// What begins every gateway session.
std::vector<std::string> SessionStart() {
  return {
      R"({"op":10,"d":{"heartbeat_interval":3600000}})",
      R"({"op":0,"s":1,"t":"READY","d":{"session_id":"load",)"
      R"("resume_gateway_url":"ws://127.0.0.1:1",)"
      R"("user":{"id":"99","username":"gm_bot","bot":true}}})",
  };
}

// Discord's HTTP API, as far as the bot uses it, answered from memory. It
// notes how long each event waited for the call that answers it.
class AnsweringApi final : public http::Client {
 public:
  AnsweringApi(std::string gateway_url, const SendTimes* sent,
               LoadResult* result)
      : gateway_url_(std::move(gateway_url)), sent_(sent), result_(result) {}

  Task<absl::StatusOr<http::Response>> Send(
      const http::Request& request) override {
    const std::string_view url = request.url;

    if (absl::EndsWith(url, "/gateway/bot")) {
      co_return http::Response{
          .status = 200,
          .body = absl::StrCat(R"({"url":")", gateway_url_, R"("})"),
      };
    }
    if (request.method == http::Method::kGet) {
      co_return http::Response{
          .status = 200,
          .body = absl::StrCat(R"({"guild_id":")", kGuild, R"("})"),
      };
    }

    // A reaction names the message in its path, and a reply the command.
    NoteAnswered(url, "/messages/");
    NoteAnswered(url, "/interactions/");
    co_return http::Response{
        .status = 204,
    };
  }

 private:
  // If `url` has `marker` followed by an event's number, counts that event
  // as dealt with.
  void NoteAnswered(std::string_view url, std::string_view marker) {
    const size_t at = url.find(marker);
    if (at == std::string_view::npos) return;

    const std::string_view rest = url.substr(at + marker.size());
    int64_t sequence = 0;
    if (!absl::SimpleAtoi(rest.substr(0, rest.find('/')), &sequence)) return;

    result_->latency.Record(sent_->Since(static_cast<size_t>(sequence - 2)));
    result_->elapsed =
        std::chrono::nanoseconds(NowNanoseconds() - sent_->at(0));
    ++result_->handled;
  }

  std::string gateway_url_;
  const SendTimes* sent_;
  LoadResult* result_;
};

Task<absl::Status> ReceiveAll(std::string url, size_t events,
                              const SendTimes& sent, LoadResult& result) {
  CO_ASSIGN_OR_RETURN(const std::unique_ptr<websocket::Connection> connection,
                      co_await websocket::Connect(std::move(url)));

  for (size_t i = 0; i < events; ++i) {
    CO_ASSIGN_OR_RETURN(
        const websocket::Incoming incoming,
        co_await connection->Receive(net::After(std::chrono::seconds(30))));
    if (!std::holds_alternative<std::string>(incoming)) {
      co_return absl::DataLossError("the server closed before sending it all");
    }

    result.latency.Record(sent.Since(i));
    ++result.handled;
  }

  result.elapsed = std::chrono::nanoseconds(NowNanoseconds() - sent.at(0));
  co_return absl::OkStatus();
}

Task<absl::Status> ServeUntilTurnedAway(std::string url, bot::Config config,
                                        gm::Ledger& ledger,
                                        const SendTimes& sent,
                                        LoadResult& result) {
  CO_ASSIGN_OR_RETURN(
      discord::Client client,
      co_await discord::Client::Connect(
          config.token, std::make_unique<AnsweringApi>(url, &sent, &result),
          &websocket::Connect));

  // The server ends the test by closing as Discord does on a bad token.
  const absl::Status stopped = co_await bot::Serve(client, ledger, config);
  if (absl::IsUnauthenticated(stopped)) co_return absl::OkStatus();
  co_return stopped;
}

// Gives each member `days` days of GMs up to yesterday, so that the ledger
// has something to look through.
absl::Status GiveHistory(gm::Ledger& ledger, int members, int days) {
  gm::Community community = ledger.community(kGuild);
  const absl::CivilDay today =
      absl::ToCivilDay(absl::Now(), absl::UTCTimeZone());

  for (int back = days; back >= 1; --back) {
    for (int member = 1; member <= members; ++member) {
      ABSL_RETURN_IF_ERROR(community
                               .Record(
                                   gm::Member{
                                       .id = static_cast<uint64_t>(member),
                                       .name = absl::StrCat("member", member),
                                   },
                                   today - back)
                               .status());
    }
  }
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<LoadResult> RunWebSocketLoad(const LoadOptions& options) {
  ABSL_ASSIGN_OR_RETURN(const net::Listener listener,
                        net::Listener::OnLoopback());
  std::vector<std::string> events;
  events.reserve(options.events);
  for (int i = 0; i < options.events; ++i) {
    events.push_back(MessageEvent(i + 2, 400000000000000004, "gm everyone"));
  }

  SendTimes sent(events.size());
  LoadResult result;
  ABSL_ASSIGN_OR_RETURN(EventLoop loop, EventLoop::Create());
  {
    const GatewayThread server(listener, {}, std::move(events),
                               options.interval, sent);
    ABSL_RETURN_IF_ERROR(loop.Run(ReceiveAll(
        UrlOf(listener), static_cast<size_t>(options.events), sent, result)));
  }
  return result;
}

absl::StatusOr<LoadResult> RunBotLoad(const LoadOptions& options) {
  // A ledger on disk, as the bot has in production.
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      absl::StrCat("gm_bot_load_", NowNanoseconds());
  std::filesystem::create_directories(directory);
  LoadResult result;
  absl::Status outcome;
  {
    ABSL_ASSIGN_OR_RETURN(gm::Ledger ledger,
                          gm::Ledger::Open(directory / "gm.db"));
    ABSL_RETURN_IF_ERROR(
        GiveHistory(ledger, options.members, options.history_days));

    ABSL_ASSIGN_OR_RETURN(const net::Listener listener,
                          net::Listener::OnLoopback());
    std::vector<std::string> events = Morning(options);
    SendTimes sent(events.size());

    ABSL_ASSIGN_OR_RETURN(EventLoop loop, EventLoop::Create());
    const GatewayThread server(listener, SessionStart(), std::move(events),
                               options.interval, sent);
    outcome =
        loop.Run(ServeUntilTurnedAway(UrlOf(listener),
                                      bot::Config{
                                          .token = "load-test",
                                          .channels =
                                              {
                                                  discord::ChannelId{
                                                      .value = kChannel,
                                                  },
                                              },
                                          .time_zone = absl::UTCTimeZone(),
                                      },
                                      ledger, sent, result));
  }
  std::filesystem::remove_all(directory);

  ABSL_RETURN_IF_ERROR(outcome);
  return result;
}
