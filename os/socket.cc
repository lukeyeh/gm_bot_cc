#include "os/socket.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"

namespace os_internal {

int DescriptorOf(const os::Socket& socket) { return socket.descriptor_; }

os::Socket SocketFromDescriptor(int descriptor) {
  return os::Socket(descriptor);
}

void ToKernelAddress(const os::SocketAddress& address, sockaddr_in* out) {
  *out = {};
  out->sin_family = AF_INET;
  out->sin_port = htons(address.port_);
  out->sin_addr.s_addr = htonl(address.ip_);
}

}  // namespace os_internal

namespace os {

absl::StatusOr<SocketAddress> SocketAddress::Parse(const std::string& ip,
                                                   uint16_t port) {
  in_addr parsed;
  if (inet_pton(AF_INET, ip.c_str(), &parsed) != 1) {
    return absl::InvalidArgumentError(
        absl::StrCat("not an IPv4 address: ", ip));
  }
  return SocketAddress(ntohl(parsed.s_addr), port);
}

absl::StatusOr<SocketAddress> SocketAddress::Resolve(const std::string& host,
                                                     uint16_t port) {
  const addrinfo hints = {
      .ai_family = AF_INET,
      .ai_socktype = SOCK_STREAM,
  };
  addrinfo* found = nullptr;
  const int failure = getaddrinfo(host.c_str(), nullptr, &hints, &found);
  if (failure != 0) {
    const std::string message =
        absl::StrCat("cannot resolve ", host, ": ", gai_strerror(failure));
    return failure == EAI_NONAME ? absl::NotFoundError(message)
                                 : absl::UnavailableError(message);
  }
  const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(
      found, &freeaddrinfo);
  const auto* const first =
      reinterpret_cast<const sockaddr_in*>(addresses->ai_addr);
  return SocketAddress(ntohl(first->sin_addr.s_addr), port);
}

std::string SocketAddress::ToString() const {
  return absl::StrCat(ip_ >> 24, ".", (ip_ >> 16) & 0xff, ".",
                      (ip_ >> 8) & 0xff, ".", ip_ & 0xff, ":", port_);
}

absl::StatusOr<Socket> Socket::CreateTcp() {
  const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
  if (descriptor < 0) return absl::ErrnoToStatus(errno, "cannot create socket");
  return Socket(descriptor);
}

Socket::Socket(Socket&& other)
    : descriptor_(std::exchange(other.descriptor_, -1)) {}

Socket& Socket::operator=(Socket&& other) {
  std::swap(descriptor_, other.descriptor_);
  return *this;
}

Socket::~Socket() {
  if (descriptor_ >= 0) close(descriptor_);
}

absl::Status Socket::Enable(SocketOption option) {
  const int enable = 1;
  int result = 0;
  switch (option) {
    case SocketOption::kReuseAddress:
      result = setsockopt(descriptor_, SOL_SOCKET, SO_REUSEADDR, &enable,
                          sizeof(enable));
      break;
    case SocketOption::kNoDelay:
      result = setsockopt(descriptor_, IPPROTO_TCP, TCP_NODELAY, &enable,
                          sizeof(enable));
      break;
  }
  if (result < 0) return absl::ErrnoToStatus(errno, "cannot set socket option");
  return absl::OkStatus();
}

absl::Status Socket::Bind(const SocketAddress& address) {
  sockaddr_in local;
  os_internal::ToKernelAddress(address, &local);
  if (bind(descriptor_, reinterpret_cast<const sockaddr*>(&local),
           sizeof(local)) < 0) {
    return absl::ErrnoToStatus(
        errno, absl::StrCat("cannot bind to ", address.ToString()));
  }
  return absl::OkStatus();
}

absl::Status Socket::Listen() {
  if (listen(descriptor_, SOMAXCONN) < 0) {
    return absl::ErrnoToStatus(errno, "cannot listen");
  }
  return absl::OkStatus();
}

absl::StatusOr<SocketAddress> Socket::LocalAddress() const {
  sockaddr_in local;
  socklen_t size = sizeof(local);
  if (getsockname(descriptor_, reinterpret_cast<sockaddr*>(&local), &size) <
      0) {
    return absl::ErrnoToStatus(errno, "cannot determine local address");
  }
  return SocketAddress(ntohl(local.sin_addr.s_addr), ntohs(local.sin_port));
}

}  // namespace os
