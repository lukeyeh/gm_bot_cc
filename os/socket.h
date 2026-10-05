// Network sockets as C++ objects. Wraps the kernel's socket interface so that
// callers deal in classes, enums and absl::Status rather than file
// descriptors, option constants and error numbers.
//
// This header covers creating and configuring sockets, none of which waits.
// Sending, receiving, connecting and accepting can wait, and are in io.h.

#ifndef OS_SOCKET_H_
#define OS_SOCKET_H_

#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

struct sockaddr_in;

namespace os {
class Socket;
class SocketAddress;
}  // namespace os

namespace os_internal {
// For the rest of //os, which has to hand these to the kernel.
int DescriptorOf(const os::Socket& socket);
os::Socket SocketFromDescriptor(int descriptor);
void ToKernelAddress(const os::SocketAddress& address, sockaddr_in* out);
}  // namespace os_internal

namespace os {

// Where a socket is, or should connect to: an IPv4 address and a port.
class SocketAddress {
 public:
  // `ip` is a dotted address such as "127.0.0.1"; host names are not
  // resolved. Fails with InvalidArgument if it is anything else.
  static absl::StatusOr<SocketAddress> Parse(const std::string& ip,
                                             uint16_t port);

  // Looks up the IPv4 address of `host`, which may be a name such as
  // "discord.com" or a dotted address. Fails with NotFound if there is no
  // such host and Unavailable if the lookup itself could not be done.
  //
  // Unlike everything else in this header this can wait, for as long as the
  // system's resolver takes, and it blocks the thread while it does. That
  // suits a client resolving the few servers it talks to as it connects.
  static absl::StatusOr<SocketAddress> Resolve(const std::string& host,
                                               uint16_t port);

  uint16_t port() const { return port_; }

  // For example "127.0.0.1:8080".
  std::string ToString() const;

 private:
  friend class Socket;
  friend void os_internal::ToKernelAddress(const SocketAddress&, sockaddr_in*);

  SocketAddress(uint32_t ip, uint16_t port) : ip_(ip), port_(port) {}

  // The four bytes of the address, first byte most significant.
  uint32_t ip_;
  uint16_t port_;
};

// Behaviours of a socket that are off unless enabled.
enum class SocketOption {
  // Lets a listening socket claim a port that a previous run of the program
  // was using moments ago. Without it, restarting a server fails for a minute
  // or so.
  kReuseAddress,
  // Sends each write immediately instead of holding it back briefly in the
  // hope of combining it with the next.
  kNoDelay,
};

// One TCP socket. Closed when the object is destroyed.
class Socket {
 public:
  static absl::StatusOr<Socket> CreateTcp();

  Socket(Socket&& other);
  Socket& operator=(Socket&& other);
  ~Socket();

  absl::Status Enable(SocketOption option);

  // Claims `address` for this socket. A port of 0 asks for any free port;
  // LocalAddress reports which one was chosen. Fails with FailedPrecondition
  // if another socket already has the address.
  absl::Status Bind(const SocketAddress& address);

  // Starts accepting connections on the bound address. Peers that connect
  // wait in a queue until taken with os::Accept.
  absl::Status Listen();

  // The address this socket is bound to.
  absl::StatusOr<SocketAddress> LocalAddress() const;

 private:
  friend int os_internal::DescriptorOf(const Socket&);
  friend Socket os_internal::SocketFromDescriptor(int);

  explicit Socket(int descriptor) : descriptor_(descriptor) {}

  // The kernel's handle for the socket, or -1 once moved from.
  int descriptor_;
};

}  // namespace os

#endif  // OS_SOCKET_H_
