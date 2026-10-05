// Internal to //os: the epoll way of carrying out operations.
//
// Here the thread does each operation itself, with an ordinary system call
// that is told not to wait. Usually that succeeds at once. When it cannot,
// because there is nothing to read yet or no room to write, the operation is
// parked, and epoll is how the thread learns that a parked operation's socket
// is worth trying again.
//
// It costs more trips into the kernel per operation than io_uring, and works
// on every Linux system, including container sandboxes that forbid io_uring.

#ifndef OS_EPOLL_BACKEND_H_
#define OS_EPOLL_BACKEND_H_

#include <memory>

#include "absl/status/statusor.h"
#include "os/io.h"

namespace os_internal {

absl::StatusOr<std::unique_ptr<Backend>> NewEpollBackend();

}  // namespace os_internal

#endif  // OS_EPOLL_BACKEND_H_
