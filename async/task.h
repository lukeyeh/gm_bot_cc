// Task<T>: the return type of an asynchronous function that produces a T.
//
// An asynchronous function is one that may wait, typically for I/O. Instead
// of blocking its thread while it waits, it suspends, and the thread runs
// other tasks until whatever it is waiting for happens. Write one like an
// ordinary function, with `co_await` wherever it calls another asynchronous
// function and `co_return` instead of `return`:
//
//   Task<int> CountBytes(TcpConnection& connection) {
//     const auto line = co_await connection.ReadUntil("\n", 100);
//     co_return line.ok() ? line->size() : 0;
//   }
//
// A task does nothing until it is awaited with `co_await`, which runs it and
// yields its result, or handed to something that runs tasks, such as
// EventLoop::Run or Spawn. Destroying a task that has not finished abandons
// it.
//
// Arguments passed to an asynchronous function by reference or by view must
// stay valid until the task finishes, not just until the call returns.
// Awaiting the task in the same expression that creates it guarantees that.
//
// This file, awaitable.h, sequence.h and task_scope.cc are the only places
// that deal with the C++ coroutine machinery directly. Everything else is
// written in terms of Task, Sequence, Awaitable, Waker and TaskScope.

#ifndef ASYNC_TASK_H_
#define ASYNC_TASK_H_

#include <coroutine>
#include <optional>
#include <utility>

template <typename T>
class Task;

namespace async_internal {

// The parts of a task's coroutine state that do not depend on its result
// type.
class TaskPromiseBase {
 public:
  std::suspend_always initial_suspend() const { return {}; }

  // When the task finishes, control passes straight to whoever awaited it.
  //
  // The noexcept here and on ResumeAwaiter is the one place the language
  // insists on it, even with exceptions disabled.
  auto final_suspend() const noexcept {
    struct ResumeAwaiter {
      std::coroutine_handle<> awaiter;
      bool await_ready() const noexcept { return false; }
      std::coroutine_handle<> await_suspend(
          std::coroutine_handle<>) const noexcept {
        return awaiter;
      }
      void await_resume() const noexcept {}
    };
    return ResumeAwaiter{
        awaiter_,
    };
  }

  void set_awaiter(std::coroutine_handle<> awaiter) { awaiter_ = awaiter; }

 private:
  // The coroutine waiting for this task's result.
  std::coroutine_handle<> awaiter_ = std::noop_coroutine();
};

template <typename T>
class TaskPromise : public TaskPromiseBase {
 public:
  Task<T> get_return_object();
  void return_value(T value) { result_.emplace(std::move(value)); }
  T TakeResult() { return std::move(*result_); }

 private:
  std::optional<T> result_;
};

template <>
class TaskPromise<void> : public TaskPromiseBase {
 public:
  Task<void> get_return_object();
  void return_void() const {}
  void TakeResult() const {}
};

}  // namespace async_internal

template <typename T = void>
class [[nodiscard]] Task {
 public:
  using promise_type = async_internal::TaskPromise<T>;

  Task(Task&& other) : coroutine_(std::exchange(other.coroutine_, {})) {}
  Task& operator=(Task&& other) {
    std::swap(coroutine_, other.coroutine_);
    return *this;
  }
  ~Task() {
    if (coroutine_) coroutine_.destroy();
  }

  // Makes `co_await task` run the task and evaluate to its result.
  auto operator co_await() const {
    struct RunAwaiter {
      std::coroutine_handle<promise_type> coroutine;
      bool await_ready() const { return false; }
      std::coroutine_handle<> await_suspend(
          std::coroutine_handle<> awaiter) const {
        coroutine.promise().set_awaiter(awaiter);
        return coroutine;
      }
      T await_resume() const { return coroutine.promise().TakeResult(); }
    };
    return RunAwaiter{
        coroutine_,
    };
  }

 private:
  friend promise_type;

  explicit Task(std::coroutine_handle<promise_type> coroutine)
      : coroutine_(coroutine) {}

  // The suspended function. Null once moved from.
  std::coroutine_handle<promise_type> coroutine_;
};

namespace async_internal {

template <typename T>
Task<T> TaskPromise<T>::get_return_object() {
  return Task<T>(std::coroutine_handle<TaskPromise<T>>::from_promise(*this));
}

inline Task<void> TaskPromise<void>::get_return_object() {
  return Task<void>(
      std::coroutine_handle<TaskPromise<void>>::from_promise(*this));
}

}  // namespace async_internal

#endif  // ASYNC_TASK_H_
