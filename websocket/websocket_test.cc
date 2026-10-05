// WebSocket by example: a client and a server exchanging messages over a real
// (loopback) connection, and what each sees when the other pings, fragments,
// goes quiet, closes or vanishes.

#include "websocket/websocket.h"

#include <benchmark/benchmark.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "async/task.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "net/event_loop.h"
#include "net/reader.h"
#include "net/stream.h"

namespace {

using absl_testing::StatusIs;
using websocket::Connection;
using websocket::Incoming;

void RunOnEventLoop(Task<> test) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(std::move(test));
}

websocket::Deadline Soon() { return net::After(std::chrono::seconds(5)); }

net::Listener NewListener() {
  absl::StatusOr<net::Listener> listener = net::Listener::OnLoopback();
  ABSL_EXPECT_OK(listener);
  return std::move(*listener);
}

std::string UrlOf(const net::Listener& listener) {
  return absl::StrCat("ws://127.0.0.1:", listener.address().port,
                      "/?v=10&encoding=json");
}

// The next message on `connection`, or "" if what arrives is not a message.
Task<std::string> ReceiveText(Connection& connection) {
  const absl::StatusOr<Incoming> incoming = co_await connection.Receive(Soon());
  ABSL_EXPECT_OK(incoming);
  if (!incoming.ok() || !std::holds_alternative<std::string>(*incoming)) {
    co_return "";
  }
  co_return std::get<std::string>(*incoming);
}

// Two ends of one WebSocket.
struct Pair {
  std::unique_ptr<Connection> client;
  std::unique_ptr<Connection> server;
};

Task<Pair> Connect() {
  const net::Listener listener = NewListener();
  absl::StatusOr<std::unique_ptr<Connection>> server =
      absl::UnknownError("handshake did not finish");
  // The two handshakes need each other, so the server's runs alongside.
  Spawn([](const net::Listener& listener,
           absl::StatusOr<std::unique_ptr<Connection>>& server) -> Task<> {
    absl::StatusOr<std::unique_ptr<net::Stream>> stream =
        co_await listener.Accept();
    ABSL_EXPECT_OK(stream);
    if (stream.ok()) server = co_await websocket::Accept(std::move(*stream));
  }(listener, server));
  absl::StatusOr<std::unique_ptr<Connection>> client =
      co_await websocket::Connect(UrlOf(listener));
  ABSL_EXPECT_OK(client);
  ABSL_EXPECT_OK(server);
  co_return Pair{
      std::move(*client),
      std::move(*server),
  };
}

// A stream that passes everything to another it does not own.
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

// A client connected to a server that has accepted the WebSocket handshake
// and nothing more: `raw` is the server's end of the stream, for a test that
// wants to write frames by hand.
struct RawPair {
  std::unique_ptr<Connection> client;
  std::unique_ptr<net::Stream> raw;
};

Task<RawPair> ConnectToRawServer() {
  const net::Listener listener = NewListener();
  std::unique_ptr<net::Stream> raw;
  Spawn([](const net::Listener& listener,
           std::unique_ptr<net::Stream>& raw) -> Task<> {
    absl::StatusOr<std::unique_ptr<net::Stream>> stream =
        co_await listener.Accept();
    ABSL_EXPECT_OK(stream);
    if (!stream.ok()) co_return;
    // The Connection made for the handshake is discarded; the stream stays.
    ABSL_EXPECT_OK(
        co_await websocket::Accept(std::make_unique<View>(stream->get())));
    raw = std::move(*stream);
  }(listener, raw));
  absl::StatusOr<std::unique_ptr<Connection>> client =
      co_await websocket::Connect(UrlOf(listener));
  ABSL_EXPECT_OK(client);
  co_return RawPair{
      std::move(*client),
      std::move(raw),
  };
}

// The basic contract: whole messages, in order, both ways, at every size the
// framing treats differently.
TEST(WebSocketTest, CarriesMessagesBothWays) {
  RunOnEventLoop([]() -> Task<> {
    const Pair pair = co_await Connect();
    const std::string small = "gm 🌅";
    const std::string medium(1000, 'm');    // Needs a 16-bit length.
    const std::string large(100'000, 'L');  // Needs a 64-bit length.

    for (const std::string* const message : {&small, &medium, &large}) {
      // Sent from a task of its own: a large message does not fit in the
      // connection's buffers, so the sender has to wait for the receiver.
      Spawn([](Connection& client, const std::string& message) -> Task<> {
        ABSL_EXPECT_OK(co_await client.Send(message));
      }(*pair.client, *message));
      EXPECT_EQ(co_await ReceiveText(*pair.server), *message);

      Spawn([](Connection& server, const std::string& message) -> Task<> {
        ABSL_EXPECT_OK(co_await server.Send(message));
      }(*pair.server, *message));
      EXPECT_EQ(co_await ReceiveText(*pair.client), *message);
    }
  }());
}

// A quiet peer is not a failure, and waiting again later works.
TEST(WebSocketTest, ReceiveGivesUpAtTheDeadlineAndCanBeRepeated) {
  RunOnEventLoop([]() -> Task<> {
    const Pair pair = co_await Connect();

    EXPECT_THAT(co_await pair.client->Receive(
                    net::After(std::chrono::milliseconds(10))),
                StatusIs(absl::StatusCode::kDeadlineExceeded));

    ABSL_EXPECT_OK(co_await pair.server->Send("finally"));
    EXPECT_EQ(co_await ReceiveText(*pair.client), "finally");
  }());
}

// A deliberate close arrives as a Close carrying the peer's code and reason:
// protocols built on WebSocket use those to say why.
TEST(WebSocketTest, DeliversThePeersCloseCodeAndReason) {
  RunOnEventLoop([]() -> Task<> {
    const RawPair pair = co_await ConnectToRawServer();
    // A close frame (0x88) of 14 bytes: the code 4004, then the reason.
    ABSL_EXPECT_OK(co_await pair.raw->Write(std::string("\x88\x0E\x0F\xA4", 4) +
                                            "bad token :("));

    const absl::StatusOr<Incoming> incoming =
        co_await pair.client->Receive(Soon());

    ABSL_EXPECT_OK(incoming);
    if (!incoming.ok()) co_return;
    const websocket::Close& close = std::get<websocket::Close>(*incoming);
    EXPECT_EQ(close.code, 4004);
    EXPECT_EQ(close.reason, "bad token :(");
  }());
}

// Pings are answered and fragmented messages reassembled without the caller
// seeing either.
TEST(WebSocketTest, HidesPingsAndFragmentation) {
  RunOnEventLoop([]() -> Task<> {
    const RawPair pair = co_await ConnectToRawServer();
    ABSL_EXPECT_OK(co_await pair.raw->Write(
        absl::StrCat(std::string_view("\x89\x02hi", 4),   // Ping "hi".
                     std::string_view("\x01\x03gm ", 5),  // Text, to go on.
                     std::string_view("\x00\x04", 2), "ever",  // Goes on.
                     std::string_view("\x80\x04yone", 6))));   // Ends.

    EXPECT_EQ(co_await ReceiveText(*pair.client), "gm everyone");

    // The client's first frame back is its pong: 0x8A, the masked flag and
    // length, four mask bytes, then the payload XORed with the mask.
    net::Reader reader(pair.raw.get());
    const absl::StatusOr<std::string_view> frame =
        co_await reader.Read(8, Soon());
    ABSL_EXPECT_OK(frame);
    if (!frame.ok()) co_return;
    EXPECT_EQ((*frame)[0], '\x8A');
    std::string pong;
    for (size_t i = 0; i < 2; ++i) {
      pong.push_back(static_cast<char>((*frame)[6 + i] ^ (*frame)[2 + i]));
    }
    EXPECT_EQ(pong, "hi");
  }());
}

// A peer that vanishes mid-conversation is a failure, not a Close. Destroying
// a Connection is one way to vanish.
TEST(WebSocketTest, ReportsAVanishedPeer) {
  RunOnEventLoop([]() -> Task<> {
    Pair pair = co_await Connect();
    pair.server.reset();

    EXPECT_THAT(co_await pair.client->Receive(Soon()),
                StatusIs(absl::StatusCode::kUnavailable));
  }());
}

// A server that answers the handshake with ordinary HTTP is not a WebSocket
// server.
TEST(WebSocketTest, ConnectFailsIfTheServerDeclinesToUpgrade) {
  RunOnEventLoop([]() -> Task<> {
    const net::Listener listener = NewListener();
    Spawn([](const net::Listener& listener) -> Task<> {
      const absl::StatusOr<std::unique_ptr<net::Stream>> stream =
          co_await listener.Accept();
      ABSL_EXPECT_OK(stream);
      if (!stream.ok()) co_return;
      std::array<char, 1024> request{};
      (co_await (*stream)->Read(request, Soon())).IgnoreError();
      ABSL_EXPECT_OK(co_await (*stream)->Write(
          "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"));
    }(listener));

    EXPECT_THAT(co_await websocket::Connect(UrlOf(listener)),
                StatusIs(absl::StatusCode::kFailedPrecondition));
  }());
}

// And a client that sends ordinary HTTP is not a WebSocket client.
TEST(WebSocketTest, AcceptFailsIfTheClientDoesNotAskToUpgrade) {
  RunOnEventLoop([]() -> Task<> {
    const net::Listener listener = NewListener();
    const absl::StatusOr<std::unique_ptr<net::Stream>> browser =
        co_await net::Dial(listener.address(), Soon());
    ABSL_EXPECT_OK(browser);
    if (!browser.ok()) co_return;
    ABSL_EXPECT_OK(
        co_await (*browser)->Write("GET / HTTP/1.1\r\nHost: x\r\n\r\n"));

    absl::StatusOr<std::unique_ptr<net::Stream>> stream =
        co_await listener.Accept();
    ABSL_EXPECT_OK(stream);
    if (!stream.ok()) co_return;
    EXPECT_THAT(co_await websocket::Accept(std::move(*stream)),
                StatusIs(absl::StatusCode::kFailedPrecondition));
  }());
}

TEST(WebSocketTest, ConnectRejectsUrlsItCannotDial) {
  RunOnEventLoop([]() -> Task<> {
    EXPECT_THAT(co_await websocket::Connect("gopher://example.com/"),
                StatusIs(absl::StatusCode::kInvalidArgument));
  }());
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //websocket:websocket_test -- --benchmark_filter=all
//
// clang-format off
// Results. Written by perf/record_results.py; do not edit by hand.
//
//   Date      2026-10-05
//   CPU       Intel(R) Core(TM) i7-9700 CPU @ 3.00GHz, 8 cores, L3 12 MiB (1 instance)
//   Memory    31 GB
//   Disk      Samsung SSD 990 EVO Plus 4TB (ext4)
//   System    Linux 7.0.0-38-generic, CPU governor: powersave
//   Compiler  clang version 21.1.8
//   Build     bazel -c opt (-O2), C++20, no exceptions
//   I/O       io_uring, except where a benchmark's name says epoll
//
//   ---------------------------------------------------------------------------------
//   Benchmark                       Time             CPU   Iterations UserCounters...
//   ---------------------------------------------------------------------------------
//   BM_ReceiveMessage/256        5745 ns         5745 ns        73001 bytes_per_second=42.4988Mi/s
//   BM_ReceiveMessage/4096       6713 ns         6713 ns        64410 bytes_per_second=581.872Mi/s
//   BM_SendMessage/256           5453 ns         5453 ns        77260 bytes_per_second=44.7716Mi/s
//   BM_SendMessage/4096          8384 ns         8383 ns        52234 bytes_per_second=465.952Mi/s
// End of results.
// clang-format on

Task<> Echo(benchmark::State& state) {
  const Pair pair = co_await Connect();
  // About the size of a small gateway event.
  const std::string message(static_cast<size_t>(state.range(0)), 'x');
  for (auto _ : state) {
    (co_await pair.server->Send(message)).IgnoreError();
    benchmark::DoNotOptimize(co_await pair.client->Receive(Soon()));
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

// One message from server to client over loopback: framing, the kernel, and
// the event loop's turn.
void BM_ReceiveMessage(benchmark::State& state) {
  EventLoop::Create()->Run(Echo(state));
}
BENCHMARK(BM_ReceiveMessage)->Arg(256)->Arg(4096);

Task<> ClientSends(benchmark::State& state) {
  const Pair pair = co_await Connect();
  const std::string message(static_cast<size_t>(state.range(0)), 'x');
  for (auto _ : state) {
    (co_await pair.client->Send(message)).IgnoreError();
    benchmark::DoNotOptimize(co_await pair.server->Receive(Soon()));
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

// One message from client to server, which adds masking at one end and
// unmasking at the other to the above.
void BM_SendMessage(benchmark::State& state) {
  EventLoop::Create()->Run(ClientSends(state));
}
BENCHMARK(BM_SendMessage)->Arg(256)->Arg(4096);

}  // namespace
