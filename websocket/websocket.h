// WebSocket connections (RFC 6455): a long-lived, two-way channel of whole
// messages.
//
// A `Connection` deals in messages only. The opening handshake, framing,
// masking, reassembly of fragmented messages, answering pings and the closing
// handshake all happen inside it.
//
// Everything that may wait is asynchronous (see async/task.h) and must be
// called from a task running on an EventLoop.

#ifndef WEBSOCKET_WEBSOCKET_H_
#define WEBSOCKET_WEBSOCKET_H_

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "net/stream.h"

namespace websocket {

// The moment a wait gives up and reports DeadlineExceeded.
using Deadline = std::chrono::steady_clock::time_point;

// The peer ending the connection on purpose, with its stated reason.
struct Close {
  // From RFC 6455, or from the range left to applications (4000 and up).
  // 1005 if the peer gave none.
  int code = 1005;
  std::string reason;
};

// What can arrive: the payload of a message, or the peer's goodbye.
using Incoming = std::variant<std::string, Close>;

// One end of an open WebSocket. Destroying it drops the connection without a
// goodbye, which the peer sees as the connection breaking.
//
// One task may be receiving while another sends, but no more than one of
// each at a time.
class Connection {
 public:
  virtual ~Connection() = default;

  // Waits for the next message. Fails with DeadlineExceeded if none arrives
  // in time, which leaves the connection as it was: nothing is lost, and a
  // later call continues. After a `Close` or any other failure the connection
  // is finished and should be destroyed.
  virtual Task<absl::StatusOr<Incoming>> Receive(Deadline deadline) = 0;

  // Sends `text` as one message. A failure means the connection is finished.
  virtual Task<absl::Status> Send(std::string_view text) = 0;
};

// Connects to a ws:// or wss:// URL and performs the opening handshake.
// Fails as net::Dial does when the server cannot be reached or verified, and
// with FailedPrecondition when it is reached but declines to speak WebSocket.
Task<absl::StatusOr<std::unique_ptr<Connection>>> Connect(std::string url);

// How code that needs connections obtains them: `Connect`, or a stand-in.
using Connector =
    std::function<Task<absl::StatusOr<std::unique_ptr<Connection>>>(
        std::string)>;

// The server's side: performs the opening handshake with a client that has
// just connected over `stream`.
Task<absl::StatusOr<std::unique_ptr<Connection>>> Accept(
    std::unique_ptr<net::Stream> stream);

}  // namespace websocket

#endif  // WEBSOCKET_WEBSOCKET_H_
