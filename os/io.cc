#include "os/io.h"

#include <liburing.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "absl/flags/flag.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "async/awaitable.h"
#include "os/epoll_backend.h"
#include "os/io_uring_backend.h"
#include "os/socket.h"

ABSL_FLAG(os::IoBackend, io_backend, os::IoBackend::kAuto,
          "How to do network I/O: io_uring, epoll, or auto, which uses "
          "io_uring where the system allows it and epoll otherwise");

namespace {

using os_internal::Backend;

// The backend of the driver attached to the calling thread, if any.
thread_local Backend* attached_backend = nullptr;

Backend& AttachedBackend() {
  QCHECK(attached_backend != nullptr)
      << "I/O operation awaited on a thread with no IoDriver attached";
  return *attached_backend;
}

__kernel_timespec ToKernelTime(os::Duration duration) {
  constexpr long long kNanosecondsPerSecond = 1'000'000'000;
  return {
      .tv_sec = duration.count() / kNanosecondsPerSecond,
      .tv_nsec = duration.count() % kNanosecondsPerSecond,
  };
}

// Calls `call`, a system call wrapper that returns -1 and sets errno on
// failure, again whenever it is interrupted by a signal. Returns its result,
// or -errno.
template <typename Call>
int Retrying(Call call) {
  for (;;) {
    const auto result = call();
    if (result >= 0) return static_cast<int>(result);
    if (errno != EINTR) return -errno;
  }
}

}  // namespace

namespace os_internal {

Operation::Operation(int descriptor, Needs needs,
                     std::optional<os::Duration> time_limit)
    : descriptor_(descriptor),
      needs_(needs),
      time_limit_(time_limit),
      kernel_time_limit_(
          ToKernelTime(time_limit.value_or(os::Duration::zero()))) {}

bool Operation::Ready() { return AttachedBackend().TryNow(*this); }

void Operation::Start(Waker waker) {
  waker_ = waker;
  AttachedBackend().Start(*this);
}

absl::Status Operation::status(std::string_view what) const {
  if (result_ >= 0) return absl::OkStatus();
  // The kernel reports failure as a negated error number. Abseil knows which
  // status code each one corresponds to; a time limit running out arrives
  // here as ETIMEDOUT and so becomes DeadlineExceeded.
  return absl::ErrnoToStatus(-result_, what);
}

}  // namespace os_internal

namespace os {

std::string_view IoBackendName(IoBackend backend) {
  switch (backend) {
    case IoBackend::kAuto: return "auto";
    case IoBackend::kIoUring: return "io_uring";
    case IoBackend::kEpoll: return "epoll";
  }
  return "unknown";
}

bool AbslParseFlag(absl::string_view text, IoBackend* backend,
                   std::string* error) {
  for (const IoBackend candidate : {
           IoBackend::kAuto,
           IoBackend::kIoUring,
           IoBackend::kEpoll,
       }) {
    if (text == IoBackendName(candidate)) {
      *backend = candidate;
      return true;
    }
  }
  *error = "must be auto, io_uring or epoll";
  return false;
}

std::string AbslUnparseFlag(IoBackend backend) {
  return std::string(IoBackendName(backend));
}

absl::StatusOr<IoDriver> IoDriver::Create(const IoOptions& options) {
  const IoBackend wanted =
      options.backend.value_or(absl::GetFlag(FLAGS_io_backend));

  if (wanted != IoBackend::kEpoll) {
    absl::StatusOr<std::unique_ptr<Backend>> io_uring =
        os_internal::NewIoUringBackend(options.io_uring);
    if (io_uring.ok()) {
      return IoDriver(IoBackend::kIoUring, std::move(*io_uring));
    }
    if (wanted == IoBackend::kIoUring) return io_uring.status();
    LOG_FIRST_N(WARNING, 1)
        << io_uring.status().message() << "; using epoll instead";
  }

  ABSL_ASSIGN_OR_RETURN(std::unique_ptr<Backend> epoll,
                        os_internal::NewEpollBackend());
  return IoDriver(IoBackend::kEpoll, std::move(epoll));
}

IoDriver::IoDriver(IoBackend backend, std::unique_ptr<Backend> implementation)
    : backend_(backend), implementation_(std::move(implementation)) {}
IoDriver::IoDriver(IoDriver&&) = default;
IoDriver& IoDriver::operator=(IoDriver&&) = default;
IoDriver::~IoDriver() = default;

void IoDriver::Attach() {
  QCHECK(attached_backend == nullptr)
      << "an IoDriver is already attached to this thread";
  implementation_->OnAttach();
  attached_backend = implementation_.get();
}

void IoDriver::Detach() {
  QCHECK(attached_backend == implementation_.get())
      << "this IoDriver is not attached to this thread";
  attached_backend = nullptr;
}

void IoDriver::WakeFinished(const bool& stop) {
  implementation_->WakeFinished(stop);
}

void IoDriver::CancelAll() { implementation_->CancelAll(); }

Accept::Accept(const Socket& listener)
    : Operation(os_internal::DescriptorOf(listener), Needs::kIncomingConnection,
                std::nullopt) {}

void Accept::Describe(io_uring_sqe* entry) {
  io_uring_prep_accept(entry, descriptor(), nullptr, nullptr, 0);
}

int Accept::Attempt() {
  // Without this, accepting when no connection is waiting would block the
  // thread. Another thread may take the connection that woke this one, so
  // that can happen even after being told the socket is ready.
  int enable = 1;
  ioctl(descriptor(), FIONBIO, &enable);
  return Retrying([this] { return accept(descriptor(), nullptr, nullptr); });
}

absl::StatusOr<Socket> Accept::Finish() const {
  if (result() < 0) return status("cannot accept connection");
  return os_internal::SocketFromDescriptor(result());
}

Connect::Connect(const Socket& socket, const SocketAddress& peer,
                 std::optional<Duration> time_limit)
    : Operation(os_internal::DescriptorOf(socket), Needs::kWritable,
                time_limit),
      peer_(peer) {
  os_internal::ToKernelAddress(peer, &kernel_peer_);
}

void Connect::Describe(io_uring_sqe* entry) {
  io_uring_prep_connect(entry, descriptor(),
                        reinterpret_cast<const sockaddr*>(&kernel_peer_),
                        sizeof(kernel_peer_));
}

int Connect::Attempt() {
  if (!begun_) {
    begun_ = true;
    // Makes connect return at once and carry on in the background, to be
    // checked on when the socket becomes writable.
    int enable = 1;
    ioctl(descriptor(), FIONBIO, &enable);
    const int result = Retrying([this] {
      return connect(descriptor(),
                     reinterpret_cast<const sockaddr*>(&kernel_peer_),
                     sizeof(kernel_peer_));
    });
    return result == -EINPROGRESS ? -EAGAIN : result;
  }

  // How the attempt begun above turned out.
  int error = 0;
  socklen_t size = sizeof(error);
  if (getsockopt(descriptor(), SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
    return -errno;
  }
  if (error != 0) return -error;
  // No error yet does not mean connected: check that there is a peer.
  sockaddr_in connected_to;
  size = sizeof(connected_to);
  if (getpeername(descriptor(), reinterpret_cast<sockaddr*>(&connected_to),
                  &size) < 0) {
    return errno == ENOTCONN ? -EAGAIN : -errno;
  }
  return 0;
}

absl::Status Connect::Finish() const {
  if (result() < 0) {
    return status(absl::StrCat("cannot connect to ", peer_.ToString()));
  }
  return absl::OkStatus();
}

Receive::Receive(const Socket& socket, std::span<char> buffer,
                 std::optional<Duration> time_limit)
    : Operation(os_internal::DescriptorOf(socket), Needs::kReadable,
                time_limit),
      buffer_(buffer) {}

void Receive::Describe(io_uring_sqe* entry) {
  io_uring_prep_recv(entry, descriptor(), buffer_.data(), buffer_.size(), 0);
}

int Receive::Attempt() {
  return Retrying([this] {
    return recv(descriptor(), buffer_.data(), buffer_.size(), MSG_DONTWAIT);
  });
}

absl::StatusOr<size_t> Receive::Finish() const {
  if (result() < 0) return status("cannot receive");
  return static_cast<size_t>(result());
}

Send::Send(const Socket& socket, std::string_view first,
           std::string_view second, std::optional<Duration> time_limit)
    : Operation(os_internal::DescriptorOf(socket), Needs::kWritable,
                time_limit),
      pieces_{
          {
              .iov_base = const_cast<char*>(first.data()),
              .iov_len = first.size(),
          },
          {
              .iov_base = const_cast<char*>(second.data()),
              .iov_len = second.size(),
          },
      },
      message_{} {}

// MSG_NOSIGNAL, in both of the following: by default, writing to a peer that
// has hung up kills the whole process with SIGPIPE.

void Send::Describe(io_uring_sqe* entry) {
  message_.msg_iov = pieces_;
  message_.msg_iovlen = 2;
  // MSG_WAITALL: have the kernel keep going after a partial send instead of
  // reporting it.
  io_uring_prep_sendmsg(entry, descriptor(), &message_,
                        MSG_NOSIGNAL | MSG_WAITALL);
}

int Send::Attempt() {
  message_.msg_iov = pieces_;
  message_.msg_iovlen = 2;
  return Retrying([this] {
    return sendmsg(descriptor(), &message_, MSG_NOSIGNAL | MSG_DONTWAIT);
  });
}

absl::StatusOr<size_t> Send::Finish() const {
  if (result() < 0) return status("cannot send");
  return static_cast<size_t>(result());
}

Sleep::Sleep(Duration duration) : Operation(-1, Needs::kTime, duration) {}

void Sleep::Describe(io_uring_sqe* entry) {
  // A timer finishes by "failing" with ETIME, which Finish ignores.
  io_uring_prep_timeout(entry, kernel_time_limit(), 0, 0);
}

int Sleep::Attempt() { return -EAGAIN; }

}  // namespace os
