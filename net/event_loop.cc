#include "net/event_loop.h"

#include <chrono>
#include <memory>
#include <string_view>
#include <utility>

#include "absl/log/check.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "async/task_scope.h"
#include "os/io.h"

namespace {

// Where Spawn puts tasks on the calling thread. Null when no EventLoop is
// running on the thread.
thread_local TaskScope* spawned_tasks = nullptr;

Task<> RunThenSetFlag(Task<> task, bool& finished) {
  co_await task;
  finished = true;
}

}  // namespace

absl::StatusOr<EventLoop> EventLoop::Create() {
  ABSL_ASSIGN_OR_RETURN(os::IoDriver driver, os::IoDriver::Create());
  return EventLoop(std::make_unique<os::IoDriver>(std::move(driver)));
}

EventLoop::EventLoop(std::unique_ptr<os::IoDriver> driver)
    : driver_(std::move(driver)) {}
EventLoop::EventLoop(EventLoop&&) = default;
EventLoop& EventLoop::operator=(EventLoop&&) = default;
EventLoop::~EventLoop() = default;

std::string_view EventLoop::io_backend() const {
  return os::IoBackendName(driver_->backend());
}

void EventLoop::Run(Task<> main) {
  QCHECK(spawned_tasks == nullptr)
      << "an EventLoop is already running on this thread";
  // Destroyed on return, which frees whatever tasks are then unfinished.
  TaskScope scope;
  spawned_tasks = &scope;
  driver_->Attach();

  // `main` starts here and runs until it first waits for I/O. From then on
  // the loop is: sleep until some I/O finishes, wake whoever was waiting for
  // it, repeat.
  bool finished = false;
  scope.Spawn(RunThenSetFlag(std::move(main), finished));
  // NOLINTNEXTLINE(bugprone-infinite-loop): set by a task woken in the call.
  while (!finished) driver_->WakeFinished(finished);

  // Spawned tasks may be suspended in the middle of I/O that refers to their
  // memory, so stop the I/O before the scope frees them.
  driver_->CancelAll();
  driver_->Detach();
  spawned_tasks = nullptr;
}

Task<> Sleep(std::chrono::nanoseconds duration) {
  co_await os::Sleep(duration);
}

void Spawn(Task<> task) {
  QCHECK(spawned_tasks != nullptr)
      << "Spawn called outside a running EventLoop";
  spawned_tasks->Spawn(std::move(task));
}
