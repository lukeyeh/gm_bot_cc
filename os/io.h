// The kernel's asynchronous network I/O, as C++.
//
// A task that wants to accept a connection, send, receive or sleep awaits one
// of the operations at the bottom of this header:
//
//   absl::StatusOr<size_t> received = co_await os::Receive(socket, buffer);
//
// Awaiting suspends the task until the kernel has done the work, without
// blocking the thread, and evaluates to the outcome as a Status or StatusOr.
// IoDriver is what makes that happen on a thread: whoever runs the thread's
// event loop creates one and keeps asking it to wake the tasks whose
// operations have finished.
//
// There are two ways of doing this underneath, chosen when a driver is
// created, and nothing above this header can tell them apart except by speed:
//
//   io_uring  Operations are handed to the kernel in batches and the kernel
//             reports back when each is done. Fewest trips into the kernel.
//             Forbidden in many container sandboxes.
//   epoll     Operations are attempted directly; when one cannot proceed, the
//             kernel is asked to say when the socket is ready, and it is
//             tried again. Works everywhere.

#ifndef OS_IO_H_
#define OS_IO_H_

#include <linux/time_types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "async/awaitable.h"
#include "os/socket.h"

struct io_uring_sqe;

namespace os_internal {
class Backend;
class EpollBackend;
class IoUringBackend;
}  // namespace os_internal

namespace os {

using Duration = std::chrono::nanoseconds;

enum class IoBackend {
  // io_uring if the system allows it, otherwise epoll.
  kAuto,
  kIoUring,
  kEpoll,
};

// "auto", "io_uring" or "epoll".
std::string_view IoBackendName(IoBackend backend);

// These let IoBackend be the type of a command-line flag. The --io_backend
// flag, defined alongside IoDriver, sets the backend for every driver that is
// not told otherwise.
bool AbslParseFlag(absl::string_view text, IoBackend* backend,
                   std::string* error);
std::string AbslUnparseFlag(IoBackend backend);

// Settings that only matter to the io_uring backend.
struct IoUringOptions {
  // When the kernel does the work of finishing operations.
  enum class CompletionWork {
    // Only while the thread is waiting in WakeFinished. The thread is never
    // interrupted while running, which is fastest for an event loop.
    kWhileWaiting,
    // As soon as possible, interrupting the thread if necessary.
    kImmediately,
  };

  // How many operations can be queued before they are handed to the kernel.
  // Queueing more than this is fine; it only causes an earlier handover.
  uint32_t queue_depth = 1024;

  CompletionWork completion_work = CompletionWork::kWhileWaiting;
};

struct IoOptions {
  // Which backend to use. Unset means whatever the --io_backend flag says,
  // which is kAuto unless the program was started with something else.
  std::optional<IoBackend> backend;

  IoUringOptions io_uring;
};

// One thread's means of doing I/O. Operations awaited on a thread go to the
// driver attached to it.
class IoDriver {
 public:
  // Fails if the requested backend is not available: io_uring where the
  // kernel lacks it or a sandbox forbids it. kAuto does not fail for that
  // reason; it falls back to epoll and logs a warning.
  static absl::StatusOr<IoDriver> Create(const IoOptions& options = {});

  IoDriver(IoDriver&&);
  IoDriver& operator=(IoDriver&&);
  ~IoDriver();

  // The backend in use; never kAuto.
  IoBackend backend() const { return backend_; }

  // Makes this the driver that operations awaited on the calling thread go
  // to. A driver can be created on one thread and attached on another, but
  // only ever attached once, and a thread can have one attached at a time.
  //
  // Sockets belong to the driver they are first used with; do not use one
  // socket with two drivers.
  void Attach();

  // Undoes Attach. Call CancelAll first if operations may be unfinished.
  void Detach();

  // Blocks until at least one operation has finished, then wakes the task
  // waiting on each finished operation, one after another. Those tasks run
  // inside this call, until each next has to wait.
  //
  // `stop` is checked before waking each task. Once it is true the remaining
  // finished operations are dropped: their tasks are never woken. This lets a
  // task end the loop that is driving it.
  void WakeFinished(const bool& stop);

  // Cancels every unfinished operation. The tasks waiting on them are not
  // woken. Afterwards the kernel holds no reference to any task's memory, so
  // abandoned tasks can be destroyed safely.
  void CancelAll();

 private:
  IoDriver(IoBackend backend,
           std::unique_ptr<os_internal::Backend> implementation);

  IoBackend backend_;
  std::unique_ptr<os_internal::Backend> implementation_;
};

}  // namespace os

namespace os_internal {

// What every operation has in common, and the meeting point between a task
// waiting for an operation and the backend that carries it out.
//
//   1. The task awaits the operation. Ready gives the backend a chance to
//      finish it on the spot; if it cannot, the task suspends and Start hands
//      the operation to the backend along with the task's waker.
//   2. When the operation is done the backend calls Complete with the
//      kernel's result, which wakes the task.
//   3. The task reads the outcome, converted to a Status by the operation's
//      Finish.
//
// An operation describes itself to both backends: Describe says what to ask
// io_uring for, Attempt does the work directly for epoll.
class Operation {
 public:
  // The Awaitable hooks every operation shares. See async/awaitable.h.
  bool Ready();
  void Start(Waker waker);

 protected:
  // What the operation is waiting for when it cannot proceed.
  enum class Needs {
    // The socket to have something to read.
    kReadable,
    // The socket to have a connection waiting to be accepted. Unlike
    // kReadable, several threads may be waiting on the same socket, and only
    // one of them should be woken for each connection.
    kIncomingConnection,
    // The socket to have room to write.
    kWritable,
    // Nothing but the passage of its time limit, which is then not a failure.
    kTime,
  };

  // `descriptor` is the socket operated on, or -1 for kTime. If `time_limit`
  // passes before the operation finishes, the operation is abandoned and
  // reports DeadlineExceeded.
  Operation(int descriptor, Needs needs,
            std::optional<os::Duration> time_limit);

  // Step 3. The kernel's result: a count or handle if non-negative.
  int result() const { return result_; }

  // Step 3. OK if the operation succeeded; otherwise what went wrong, with
  // `what` saying what was being attempted.
  absl::Status status(std::string_view what) const;

  int descriptor() const { return descriptor_; }

  // The time limit in the kernel's format, for operations that hand it to
  // io_uring themselves.
  __kernel_timespec* kernel_time_limit() { return &kernel_time_limit_; }

 private:
  friend class EpollBackend;
  friend class IoUringBackend;

  // Fills in a submission queue entry asking io_uring to do the operation.
  virtual void Describe(io_uring_sqe* entry) = 0;

  // Does the operation now if that is possible without waiting. Returns its
  // result (non-negative on success, -errno on failure), or -EAGAIN if it
  // would have to wait for what needs() says.
  virtual int Attempt() = 0;

  // Step 2.
  void Complete(int result) {
    result_ = result;
    waker_.Wake();
  }

  int descriptor_;
  Needs needs_;
  std::optional<os::Duration> time_limit_;
  Waker waker_;
  int result_ = 0;

  // For the io_uring backend: the time limit in the kernel's format. The
  // kernel reads it after Start returns, so it has to live here.
  __kernel_timespec kernel_time_limit_ = {};

  // For the epoll backend: when the time limit runs out, and this
  // operation's neighbours in the backend's list of operations ordered by
  // that time.
  std::chrono::steady_clock::time_point deadline_;
  Operation* sooner_ = nullptr;
  Operation* later_ = nullptr;
  bool timed_ = false;
};

// One way of carrying out operations. See the two implementations.
class Backend {
 public:
  virtual ~Backend() = default;

  // Called once, on the thread that will use the backend.
  virtual void OnAttach() = 0;

  // Finishes `operation` if that can be done without waiting, and says
  // whether it did.
  virtual bool TryNow(Operation& operation) = 0;

  // Takes on an operation that TryNow could not finish. Its Complete is
  // called from a later WakeFinished.
  virtual void Start(Operation& operation) = 0;

  // As IoDriver's.
  virtual void WakeFinished(const bool& stop) = 0;
  virtual void CancelAll() = 0;
};

}  // namespace os_internal

namespace os {

// Waits for a peer to connect to `listener`, which must be listening.
//
//   absl::StatusOr<os::Socket> peer = co_await os::Accept(listener);
class [[nodiscard]] Accept : public os_internal::Operation,
                             public Awaitable<Accept> {
 public:
  explicit Accept(const Socket& listener);
  absl::StatusOr<Socket> Finish() const;

 private:
  void Describe(io_uring_sqe* entry) override;
  int Attempt() override;
};

// Connects `socket` to a peer listening at `peer`. Fails with Unavailable if
// nothing is listening there.
//
//   absl::Status connected = co_await os::Connect(socket, address);
class [[nodiscard]] Connect : public os_internal::Operation,
                              public Awaitable<Connect> {
 public:
  Connect(const Socket& socket, const SocketAddress& peer,
          std::optional<Duration> time_limit = std::nullopt);
  absl::Status Finish() const;

 private:
  void Describe(io_uring_sqe* entry) override;
  int Attempt() override;

  SocketAddress peer_;
  sockaddr_in kernel_peer_;
  // Whether Attempt has already asked the kernel to start connecting.
  bool begun_ = false;
};

// Receives whatever bytes are available on `socket` into `buffer`, waiting if
// there are none yet. Evaluates to how many were received, at most
// buffer.size(). Zero means the peer has closed the connection.
//
//   absl::StatusOr<size_t> received = co_await os::Receive(socket, buffer);
class [[nodiscard]] Receive : public os_internal::Operation,
                              public Awaitable<Receive> {
 public:
  Receive(const Socket& socket, std::span<char> buffer,
          std::optional<Duration> time_limit = std::nullopt);
  absl::StatusOr<size_t> Finish() const;

 private:
  void Describe(io_uring_sqe* entry) override;
  int Attempt() override;

  std::span<char> buffer_;
};

// Sends `first` followed by `second` on `socket` without copying either.
// Evaluates to how many bytes were sent, which may be fewer than all of them:
// send the rest with another Send. Sending to a peer that has gone away is an
// ordinary failure; it does not raise a signal.
//
//   absl::StatusOr<size_t> sent = co_await os::Send(socket, head, body);
class [[nodiscard]] Send : public os_internal::Operation,
                           public Awaitable<Send> {
 public:
  Send(const Socket& socket, std::string_view first, std::string_view second,
       std::optional<Duration> time_limit = std::nullopt);
  absl::StatusOr<size_t> Finish() const;

 private:
  void Describe(io_uring_sqe* entry) override;
  int Attempt() override;

  // The two pieces, and the message that refers to them, in the kernel's
  // format.
  iovec pieces_[2];
  msghdr message_;
};

// Does nothing for `duration`.
//
//   co_await os::Sleep(std::chrono::milliseconds(100));
class [[nodiscard]] Sleep : public os_internal::Operation,
                            public Awaitable<Sleep> {
 public:
  explicit Sleep(Duration duration);
  void Finish() const {}

 private:
  void Describe(io_uring_sqe* entry) override;
  int Attempt() override;
};

}  // namespace os

#endif  // OS_IO_H_
