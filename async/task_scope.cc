#include "async/task_scope.h"

#include <coroutine>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "async/task.h"

namespace {

// A coroutine that owns itself: it starts immediately and frees its state
// when it finishes, since nothing awaits it. While it lives, its address is
// in the set it was started with.
struct SelfOwned {
  struct promise_type {
    // Receives the arguments of the coroutine function, RunToCompletion.
    promise_type(absl::flat_hash_set<void*>& unfinished, Task<>&)
        : unfinished_(unfinished) {
      unfinished_.insert(Address());
    }
    ~promise_type() { unfinished_.erase(Address()); }

    SelfOwned get_return_object() const { return {}; }
    std::suspend_never initial_suspend() const { return {}; }
    // noexcept is required by the language here.
    std::suspend_never final_suspend() const noexcept { return {}; }
    void return_void() const {}

    void* Address() {
      return std::coroutine_handle<promise_type>::from_promise(*this).address();
    }

    absl::flat_hash_set<void*>& unfinished_;
  };
};

SelfOwned RunToCompletion(absl::flat_hash_set<void*>& unfinished, Task<> task) {
  co_await task;
}

}  // namespace

TaskScope::~TaskScope() {
  // Copied because destroying a task removes it from the set.
  const std::vector<void*> abandoned(unfinished_.begin(), unfinished_.end());
  for (void* const task : abandoned) {
    std::coroutine_handle<>::from_address(task).destroy();
  }
}

void TaskScope::Spawn(Task<> task) {
  RunToCompletion(unfinished_, std::move(task));
}
