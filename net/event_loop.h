// The event loop that runs asynchronous functions (see async/task.h) on a
// thread.
//
// Each thread that does asynchronous work owns one EventLoop. While the loop
// runs, tasks on that thread take turns: whenever one has to wait for I/O,
// another that is ready continues. Tasks never move between threads, so code
// running on one loop needs no locking against other tasks on the same loop.

#ifndef NET_EVENT_LOOP_H_
#define NET_EVENT_LOOP_H_

#include <chrono>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "absl/status/statusor.h"
#include "async/task.h"

namespace os {
class IoDriver;
}  // namespace os

class EventLoop {
 public:
  // Fails if the kind of I/O asked for with the --io_backend flag is not
  // available on this system. With the flag at its default it does not fail
  // for that reason.
  //
  // A loop may be created on one thread and run on another.
  static absl::StatusOr<EventLoop> Create();

  EventLoop(EventLoop&&);
  EventLoop& operator=(EventLoop&&);
  ~EventLoop();

  // How this loop does its I/O: "io_uring" or "epoll".
  std::string_view io_backend() const;

  // Runs `main`, and every task spawned while it runs, on the calling thread.
  // Returns `main`'s result when it finishes; spawned tasks still unfinished
  // at that point are abandoned. May be called at most once per loop.
  //
  //   const int total = loop.Run(CountBytes(connection));
  template <typename T>
  T Run(Task<T> main);
  void Run(Task<> main);

 private:
  explicit EventLoop(std::unique_ptr<os::IoDriver> driver);

  // Where this loop's I/O is queued and its outcomes collected.
  std::unique_ptr<os::IoDriver> driver_;
};

// Starts `task` running on the calling thread's event loop, independently of
// the caller: it is not awaited and nothing receives its completion. Must be
// called from a task running on an EventLoop.
void Spawn(Task<> task);

// Suspends the calling task for `duration`. Other tasks on the thread run in
// the meantime.
Task<> Sleep(std::chrono::nanoseconds duration);

namespace net_internal {

template <typename T>
Task<> StoreResult(Task<T> task, std::optional<T>& result) {
  result.emplace(co_await task);
}

}  // namespace net_internal

template <typename T>
T EventLoop::Run(Task<T> main) {
  std::optional<T> result;
  Run(net_internal::StoreResult(std::move(main), result));
  return std::move(*result);
}

#endif  // NET_EVENT_LOOP_H_
