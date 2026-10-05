// A scripted WebSocket server for tests. The test says, in order, what the
// server does (send a message, go quiet, drop the connection, close it), and
// the client under test, given `connector()`, experiences exactly that, with
// no network and no waiting. What the client sent is kept for inspection.
//
// One script spans every connection the client makes, so a test of
// reconnecting simply lists what happens before and after the break.

#ifndef WEBSOCKET_FAKE_SERVER_H_
#define WEBSOCKET_FAKE_SERVER_H_

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "websocket/websocket.h"

namespace websocket {

class FakeServer {
 public:
  // The next step of the script: the server sends `text`.
  void Sends(std::string text) { script_.emplace_back(std::move(text)); }

  // The server sends nothing until the client's deadline passes.
  void StaysQuiet() { script_.emplace_back(Quiet{}); }

  // The connection breaks without a goodbye.
  void Drops() { script_.emplace_back(Dropped{}); }

  // The server closes the connection with `code`.
  void Closes(int code) {
    script_.emplace_back(Close{
        .code = code,
        .reason = "",
    });
  }

  // The next attempt to connect fails.
  void RefusesToConnect() { script_.emplace_back(Refused{}); }

  // Pass this to the code under test in place of websocket::Connect. The
  // server must outlive every connection made through it.
  Connector connector() {
    return [this](std::string url) { return Connect(std::move(url)); };
  }

  // The URL of every connection attempt, oldest first.
  const std::vector<std::string>& urls() const { return urls_; }

  // Every message clients have sent, oldest first, across all connections.
  const std::vector<std::string>& received() const { return received_; }

 private:
  struct Quiet {};
  struct Dropped {};
  struct Refused {};
  using Step = std::variant<std::string, Quiet, Dropped, Close, Refused>;

  class FakeConnection final : public Connection {
   public:
    explicit FakeConnection(FakeServer* server) : server_(server) {}

    Task<absl::StatusOr<Incoming>> Receive(Deadline) override {
      if (finished_) co_return absl::UnavailableError("connection is finished");

      // A test whose script runs out has a client doing more than the test
      // described; say so rather than leave it waiting for ever.
      LOG_IF(FATAL, server_->script_.empty())
          << "FakeServer: the client is receiving but the script is over";
      const Step step = std::move(server_->script_.front());
      server_->script_.pop_front();

      if (const std::string* const text = std::get_if<std::string>(&step)) {
        co_return *text;
      }
      if (std::holds_alternative<Quiet>(step)) {
        co_return absl::DeadlineExceededError("timed out");
      }

      // Every other step ends the connection.
      finished_ = true;
      if (const Close* const close = std::get_if<Close>(&step)) {
        co_return *close;
      }
      co_return absl::UnavailableError("connection dropped");
    }

    Task<absl::Status> Send(std::string_view text) override {
      if (finished_) co_return absl::UnavailableError("connection is finished");

      server_->received_.emplace_back(text);
      co_return absl::OkStatus();
    }

   private:
    FakeServer* server_;
    bool finished_ = false;
  };

  Task<absl::StatusOr<std::unique_ptr<Connection>>> Connect(std::string url) {
    urls_.push_back(std::move(url));
    if (!script_.empty() && std::holds_alternative<Refused>(script_.front())) {
      script_.pop_front();
      co_return absl::UnavailableError("connection refused");
    }

    co_return std::make_unique<FakeConnection>(this);
  }

  std::deque<Step> script_;
  std::vector<std::string> urls_;
  std::vector<std::string> received_;
};

}  // namespace websocket

#endif  // WEBSOCKET_FAKE_SERVER_H_
