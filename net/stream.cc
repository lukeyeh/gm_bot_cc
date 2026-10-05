#include "net/stream.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "async/status_macros.h"
#include "async/task.h"
#include "net/tls.h"
#include "os/io.h"
#include "os/socket.h"

namespace net {
namespace {

// How long a peer may refuse to take our bytes before we call it stuck.
constexpr std::chrono::seconds kWriteTimeout(30);

// The time left until `deadline`, as the limit to give one I/O operation.
// Never quite nothing, so that an operation the kernel can finish at once is
// allowed to, even when the deadline has just passed.
os::Duration Remaining(Deadline deadline) {
  return std::max<os::Duration>(deadline - std::chrono::steady_clock::now(),
                                std::chrono::milliseconds(1));
}

class TcpStream final : public Stream {
 public:
  explicit TcpStream(os::Socket socket) : socket_(std::move(socket)) {
    // Protocol messages are small and each is wanted at once, so there is
    // nothing to gain from the system holding one back. Failing to set this
    // costs only speed.
    socket_.Enable(os::SocketOption::kNoDelay).IgnoreError();
  }

  Task<absl::StatusOr<size_t>> Read(std::span<char> buffer,
                                    Deadline deadline) override {
    co_return co_await os::Receive(socket_, buffer, Remaining(deadline));
  }

  Task<absl::Status> Write(std::string_view data) override {
    while (!data.empty()) {
      CO_ASSIGN_OR_RETURN(const size_t sent,
                          co_await os::Send(socket_, data, {}, kWriteTimeout));

      // Normally everything was sent. If not, go round again with the rest.
      data.remove_prefix(sent);
    }

    co_return absl::OkStatus();
  }

 private:
  os::Socket socket_;
};

}  // namespace

Task<absl::StatusOr<std::unique_ptr<Stream>>> Dial(Address address,
                                                   Deadline deadline) {
  CO_ASSIGN_OR_RETURN(const os::SocketAddress peer,
                      os::SocketAddress::Resolve(address.host, address.port));
  CO_ASSIGN_OR_RETURN(os::Socket socket, os::Socket::CreateTcp());
  CO_RETURN_IF_ERROR(co_await os::Connect(socket, peer, Remaining(deadline)));

  std::unique_ptr<Stream> tcp = std::make_unique<TcpStream>(std::move(socket));
  if (address.security == Security::kPlaintext) co_return tcp;

  co_return co_await TlsConnect(std::move(tcp), address.host, deadline);
}

absl::StatusOr<Listener> Listener::OnLoopback() {
  // Port 0 asks for any free port.
  ABSL_ASSIGN_OR_RETURN(const os::SocketAddress any_port,
                        os::SocketAddress::Parse("127.0.0.1", 0));
  ABSL_ASSIGN_OR_RETURN(os::Socket socket, os::Socket::CreateTcp());
  ABSL_RETURN_IF_ERROR(socket.Bind(any_port));
  ABSL_RETURN_IF_ERROR(socket.Listen());

  ABSL_ASSIGN_OR_RETURN(const os::SocketAddress bound, socket.LocalAddress());
  return Listener(std::move(socket), bound.port());
}

Address Listener::address() const {
  return Address{
      .host = "127.0.0.1",
      .port = port_,
      .security = Security::kPlaintext,
  };
}

Task<absl::StatusOr<std::unique_ptr<Stream>>> Listener::Accept() const {
  CO_ASSIGN_OR_RETURN(os::Socket socket, co_await os::Accept(socket_));

  co_return std::make_unique<TcpStream>(std::move(socket));
}

}  // namespace net
