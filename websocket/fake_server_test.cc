// websocket::FakeServer by example: how a test scripts it, and what the
// client then experiences.

#include "websocket/fake_server.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "net/event_loop.h"
#include "websocket/websocket.h"

namespace {

using absl_testing::StatusIs;
using testing::ElementsAre;
using websocket::Connection;
using websocket::Incoming;

void RunOnEventLoop(Task<> test) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(std::move(test));
}

// Fake connections never wait, so any deadline will do.
websocket::Deadline Whenever() { return std::chrono::steady_clock::now(); }

// The next message on `connection`, or "" if what arrives is not a message.
Task<std::string> ReceiveText(Connection& connection) {
  const absl::StatusOr<Incoming> incoming =
      co_await connection.Receive(Whenever());
  ABSL_EXPECT_OK(incoming);
  if (!incoming.ok() || !std::holds_alternative<std::string>(*incoming)) {
    co_return "";
  }
  co_return std::get<std::string>(*incoming);
}

// Each Receive plays the next step of the script.
TEST(FakeServerTest, PlaysTheScriptInOrder) {
  RunOnEventLoop([]() -> Task<> {
    websocket::FakeServer server;
    server.Sends("hello");
    server.StaysQuiet();
    server.Closes(4004);
    const websocket::Connector connect = server.connector();

    const absl::StatusOr<std::unique_ptr<Connection>> connection =
        co_await connect("wss://example.test/");
    ABSL_EXPECT_OK(connection);
    if (!connection.ok()) co_return;

    EXPECT_EQ(co_await ReceiveText(**connection), "hello");
    EXPECT_THAT(co_await (*connection)->Receive(Whenever()),
                StatusIs(absl::StatusCode::kDeadlineExceeded));
    const absl::StatusOr<Incoming> last =
        co_await (*connection)->Receive(Whenever());
    ABSL_EXPECT_OK(last);
    if (last.ok()) EXPECT_EQ(std::get<websocket::Close>(*last).code, 4004);
  }());
}

// The script carries on across connections, which is how reconnection is
// tested: a drop, then whatever the server says to the new connection.
TEST(FakeServerTest, OneScriptSpansReconnections) {
  RunOnEventLoop([]() -> Task<> {
    websocket::FakeServer server;
    server.Drops();
    server.RefusesToConnect();
    server.Sends("welcome back");
    const websocket::Connector connect = server.connector();

    const absl::StatusOr<std::unique_ptr<Connection>> first =
        co_await connect("wss://a/");
    ABSL_EXPECT_OK(first);
    if (!first.ok()) co_return;
    EXPECT_THAT(co_await (*first)->Receive(Whenever()),
                StatusIs(absl::StatusCode::kUnavailable));
    // A finished connection stays finished.
    EXPECT_THAT(co_await (*first)->Send("anyone?"),
                StatusIs(absl::StatusCode::kUnavailable));

    EXPECT_THAT(co_await connect("wss://b/"),
                StatusIs(absl::StatusCode::kUnavailable));

    const absl::StatusOr<std::unique_ptr<Connection>> second =
        co_await connect("wss://c/");
    ABSL_EXPECT_OK(second);
    if (!second.ok()) co_return;
    EXPECT_EQ(co_await ReceiveText(**second), "welcome back");

    EXPECT_THAT(server.urls(), ElementsAre("wss://a/", "wss://b/", "wss://c/"));
  }());
}

// What the client sends is kept for the test to check.
TEST(FakeServerTest, RecordsWhatClientsSend) {
  RunOnEventLoop([]() -> Task<> {
    websocket::FakeServer server;
    const websocket::Connector connect = server.connector();
    const absl::StatusOr<std::unique_ptr<Connection>> connection =
        co_await connect("wss://example.test/");
    ABSL_EXPECT_OK(connection);
    if (!connection.ok()) co_return;

    ABSL_EXPECT_OK(co_await (*connection)->Send("one"));
    ABSL_EXPECT_OK(co_await (*connection)->Send("two"));

    EXPECT_THAT(server.received(), ElementsAre("one", "two"));
  }());
}

}  // namespace
