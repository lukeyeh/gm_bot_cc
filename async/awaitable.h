// How to make something a task can wait for.
//
// Tasks wait for other tasks with `co_await`. At the bottom of every chain of
// tasks is something that is not a task: a timer, a network operation, an
// event from another thread. Awaitable and Waker are how such a thing is
// written, in two steps with plain names:
//
//   class Alarm : public Awaitable<Alarm> {
//    public:
//     // Called when a task does `co_await alarm` and has been suspended.
//     // Keep the waker, and call Wake() on it when the wait is over.
//     void Start(Waker waker) { clock.CallLater(when_, [waker] { waker.Wake();
//     }); }
//
//     // Called when the task continues. Whatever this returns is the value
//     // of the `co_await` expression.
//     Time Finish() const { return clock.Now(); }
//   };
//
// Optionally, a `bool Ready()` function is called first. If it returns true
// the wait is already over: the task does not suspend, Start is skipped, and
// Finish is called straight away.
//
//   const Time now = co_await Alarm(...);
//
// An awaitable must stay at the same address from Start to Finish. A
// temporary inside a `co_await` expression does.

#ifndef ASYNC_AWAITABLE_H_
#define ASYNC_AWAITABLE_H_

#include <coroutine>

// The right to continue one suspended task. Cheap to copy; pass by value.
class Waker {
 public:
  // A waker for nothing. Wake must not be called on it.
  Waker() = default;

  // Continues the task from where it suspended. The task runs, inside this
  // call, until it next waits or finishes. Call at most once, and not from
  // within the Start that received this waker.
  void Wake() const { suspended_.resume(); }

 private:
  template <typename Derived>
  friend class Awaitable;

  explicit Waker(std::coroutine_handle<> suspended) : suspended_(suspended) {}

  std::coroutine_handle<> suspended_;
};

// Base class that makes `Derived` usable with `co_await`. Derived provides:
//
//   void Start(Waker waker);   begin whatever is being waited for
//   R Finish();                produce the result once woken
//   bool Ready();              optional: true if there is nothing to wait for
//
// See the example at the top of this file.
template <typename Derived>
class Awaitable {
 public:
  // The three functions below are the protocol `co_await` speaks. They are
  // written once here so that no other class has to.
  bool await_ready() {
    Derived& derived = *static_cast<Derived*>(this);
    if constexpr (requires { derived.Ready(); }) {
      return derived.Ready();
    } else {
      return false;
    }
  }
  void await_suspend(std::coroutine_handle<> suspended) {
    static_cast<Derived*>(this)->Start(Waker(suspended));
  }
  auto await_resume() { return static_cast<Derived*>(this)->Finish(); }
};

#endif  // ASYNC_AWAITABLE_H_
