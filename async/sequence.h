// Sequence<T>: the return type of an asynchronous function that produces
// many Ts, one at a time.
//
// A Task runs to its one result. Some work is better written as a loop that
// hands out a value each time round and carries on from there when asked for
// the next: lines from a connection, events from a server. Written as a
// Task, such a loop has to return at each value and be called again, keeping
// what it was in the middle of somewhere outside itself. A Sequence keeps
// its place. Write one like an asynchronous function, with `co_yield`
// wherever it has a value to hand out:
//
//   Sequence<std::string> Lines(Reader& reader) {
//     for (;;) {
//       const auto line = co_await reader.ReadUntil("\n", 100, deadline);
//       if (!line.ok()) co_return;        // The sequence ends here.
//       co_yield std::string(*line);      // Suspends until Next is awaited.
//     }
//   }
//
// and take values from it with Next, which evaluates to the next value, or
// to nothing once the function has returned:
//
//   Sequence<std::string> lines = Lines(reader);
//   while (std::optional<std::string> line = co_await lines.Next()) {
//     ...
//   }
//
// A sequence does nothing until Next is first awaited, and between values it
// is suspended at its `co_yield`. Destroying it abandons it wherever it is.
// As with a Task, what it was given by reference or by view must stay valid
// for as long as it lives.
//
// One Next at a time: wait for each to finish before awaiting another.

#ifndef ASYNC_SEQUENCE_H_
#define ASYNC_SEQUENCE_H_

#include <coroutine>
#include <optional>
#include <utility>

template <typename T>
class [[nodiscard]] Sequence {
 public:
  class promise_type {
   public:
    Sequence get_return_object() {
      return Sequence(std::coroutine_handle<promise_type>::from_promise(*this));
    }

    std::suspend_always initial_suspend() const { return {}; }

    // At each value, and at the end, control passes straight back to whoever
    // is awaiting Next. The noexcept is the one place the language insists
    // on it, even with exceptions disabled.
    auto yield_value(T value) noexcept {
      value_.emplace(std::move(value));
      return ReturnToTaker{
          taker_,
      };
    }
    auto final_suspend() const noexcept {
      return ReturnToTaker{
          taker_,
      };
    }
    void return_void() const {}

    void set_taker(std::coroutine_handle<> taker) { taker_ = taker; }

    // The value yielded since the last call, or nothing if the function has
    // returned instead.
    std::optional<T> TakeValue() { return std::exchange(value_, std::nullopt); }

   private:
    struct ReturnToTaker {
      std::coroutine_handle<> taker;
      bool await_ready() const noexcept { return false; }
      std::coroutine_handle<> await_suspend(
          std::coroutine_handle<>) const noexcept {
        return taker;
      }
      void await_resume() const noexcept {}
    };

    // The coroutine awaiting Next.
    std::coroutine_handle<> taker_ = std::noop_coroutine();
    std::optional<T> value_;
  };

  Sequence(Sequence&& other)
      : coroutine_(std::exchange(other.coroutine_, {})) {}
  Sequence& operator=(Sequence&& other) {
    std::swap(coroutine_, other.coroutine_);
    return *this;
  }
  ~Sequence() {
    if (coroutine_) coroutine_.destroy();
  }

  // Makes `co_await sequence.Next()` run the function as far as its next
  // `co_yield` and evaluate to the value yielded, or to nothing if it returns
  // first. Once it has returned, every Next evaluates to nothing.
  auto Next() {
    struct NextAwaiter {
      std::coroutine_handle<promise_type> coroutine;
      bool await_ready() const { return coroutine.done(); }
      std::coroutine_handle<> await_suspend(
          std::coroutine_handle<> taker) const {
        coroutine.promise().set_taker(taker);
        return coroutine;
      }
      std::optional<T> await_resume() const {
        return coroutine.promise().TakeValue();
      }
    };
    return NextAwaiter{
        coroutine_,
    };
  }

 private:
  explicit Sequence(std::coroutine_handle<promise_type> coroutine)
      : coroutine_(coroutine) {}

  // The suspended function. Null once moved from.
  std::coroutine_handle<promise_type> coroutine_;
};

#endif  // ASYNC_SEQUENCE_H_
