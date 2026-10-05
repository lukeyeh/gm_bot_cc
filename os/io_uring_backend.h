// Internal to //os: the io_uring way of carrying out operations.
//
// io_uring is a pair of queues shared with the kernel. A thread writes
// descriptions of operations into the submission queue, and the kernel writes
// each one's outcome into the completion queue. Many operations can be in
// progress at once, and one trip into the kernel hands over and collects any
// number of them.

#ifndef OS_IO_URING_BACKEND_H_
#define OS_IO_URING_BACKEND_H_

#include <memory>

#include "absl/status/statusor.h"
#include "os/io.h"

namespace os_internal {

// Fails if the kernel does not provide io_uring or forbids its use, as some
// container sandboxes do.
absl::StatusOr<std::unique_ptr<Backend>> NewIoUringBackend(
    const os::IoUringOptions& options);

}  // namespace os_internal

#endif  // OS_IO_URING_BACKEND_H_
