// Reliable byte streams to other machines. A `Stream` is what protocols are
// written against; `Dial` produces one for an address, taking care of name
// resolution, TCP and (when asked) TLS, so that callers never see which kind
// they hold.
//
// Everything that may wait for the network is asynchronous (see
// async/task.h) and must be called from a task running on an EventLoop.

#ifndef NET_STREAM_H_
#define NET_STREAM_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "os/socket.h"

namespace net {

// The moment an operation gives up and reports DeadlineExceeded.
using Deadline = std::chrono::steady_clock::time_point;

// The deadline `timeout` from now.
inline Deadline After(std::chrono::nanoseconds timeout) {
  return std::chrono::steady_clock::now() +
         std::chrono::duration_cast<Deadline::duration>(timeout);
}

// A connected, ordered, reliable stream of bytes. Destroying it closes it.
//
// A stream belongs to the event loop of the thread that obtained it. At most
// one read and one write may be in progress at a time, and the stream must
// not be destroyed while either is.
class Stream {
 public:
  virtual ~Stream() = default;

  // Reads at least one byte into `buffer` and evaluates to how many, or to 0
  // at the end of the stream. Fails with DeadlineExceeded if nothing arrives
  // in time, after which the stream is still usable; any other failure is
  // final.
  virtual Task<absl::StatusOr<size_t>> Read(std::span<char> buffer,
                                            Deadline deadline) = 0;

  // Sends all of `data`. Any failure is final.
  virtual Task<absl::Status> Write(std::string_view data) = 0;
};

enum class Security {
  kPlaintext,
  // Encrypted, with the peer's identity verified against its host name.
  kTls,
};

// Where to connect, and whether to trust the network in between.
struct Address {
  std::string host;
  uint16_t port = 0;
  Security security = Security::kTls;
};

// Opens a stream to `address`. Fails with NotFound if the host does not
// resolve, Unavailable if it cannot be reached, Unauthenticated if it cannot
// prove it is `address.host`, and DeadlineExceeded if this takes too long.
Task<absl::StatusOr<std::unique_ptr<Stream>>> Dial(Address address,
                                                   Deadline deadline);

// Accepts plaintext streams on a loopback port chosen by the system. This is
// how tests stand in for a remote server.
class Listener {
 public:
  static absl::StatusOr<Listener> OnLoopback();

  // What to pass to `Dial` to reach this listener.
  Address address() const;

  // Waits for the next incoming stream.
  Task<absl::StatusOr<std::unique_ptr<Stream>>> Accept() const;

 private:
  Listener(os::Socket socket, uint16_t port)
      : socket_(std::move(socket)), port_(port) {}

  os::Socket socket_;
  uint16_t port_;
};

}  // namespace net

#endif  // NET_STREAM_H_
