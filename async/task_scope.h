// TaskScope: running tasks that nobody awaits.
//
// `co_await` runs a task and waits for it. Sometimes the caller should not
// wait: a server starts a task per connection and goes straight back to
// accepting. Spawn does that. The scope keeps track of the tasks it has
// started, so that the ones still unfinished when the scope ends are cleaned
// up rather than leaked.

#ifndef ASYNC_TASK_SCOPE_H_
#define ASYNC_TASK_SCOPE_H_

#include <cstddef>

#include "absl/container/flat_hash_set.h"
#include "async/task.h"

class TaskScope {
 public:
  TaskScope() = default;
  TaskScope(const TaskScope&) = delete;
  TaskScope& operator=(const TaskScope&) = delete;

  // Abandons the tasks that have not finished: they are destroyed where they
  // are suspended and never continue. Whatever they were waiting for must
  // already have been cancelled, so that nothing wakes them afterwards.
  ~TaskScope();

  // Starts `task` and returns as soon as it first waits (or finishes). From
  // then on it continues whenever what it is waiting for happens, and frees
  // itself when it finishes.
  void Spawn(Task<> task);

  // How many spawned tasks have not finished.
  size_t unfinished() const { return unfinished_.size(); }

 private:
  // The suspended state of each unfinished task, by address.
  absl::flat_hash_set<void*> unfinished_;
};

#endif  // ASYNC_TASK_SCOPE_H_
