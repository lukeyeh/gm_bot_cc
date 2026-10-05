// Awaitable and Waker by example: writing the thing at the bottom of a chain
// of tasks, the one that actually waits for something.

#include "async/awaitable.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "async/task.h"
#include "async/task_scope.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::ElementsAre;

// A mailbox with room for one message. A task waits for the message with
// `co_await mailbox.Receive()`; anyone can deliver one with Deliver.
class Mailbox {
 public:
  // What `co_await mailbox.Receive()` waits on. The two functions are all an
  // awaitable has to provide.
  class Receive : public Awaitable<Receive> {
   public:
    explicit Receive(Mailbox& mailbox) : mailbox_(mailbox) {}

    // The task is now suspended. Remember how to wake it.
    void Start(Waker waker) { mailbox_.waiting_ = waker; }

    // The task has been woken. This is what its co_await evaluates to.
    std::string Finish() { return std::move(mailbox_.message_); }

   private:
    Mailbox& mailbox_;
  };

  void Deliver(std::string message) {
    message_ = std::move(message);
    waiting_.Wake();
  }

 private:
  std::string message_;
  Waker waiting_;
};

Task<> Reader(Mailbox& mailbox, std::vector<std::string>& events) {
  events.push_back("reader waits");
  const std::string message = co_await Mailbox::Receive(mailbox);
  events.push_back("reader got " + message);
}

// The whole life of a wait: the task suspends in Start, control returns to
// whoever started it, and Wake continues it later, from inside Deliver.
TEST(AwaitableTest, StartSuspendsAndWakeContinues) {
  Mailbox mailbox;
  std::vector<std::string> events;
  TaskScope scope;

  scope.Spawn(Reader(mailbox, events));
  events.push_back("spawn returned");
  mailbox.Deliver("hello");
  events.push_back("deliver returned");

  EXPECT_THAT(events, ElementsAre("reader waits", "spawn returned",
                                  "reader got hello", "deliver returned"));
}

// An awaitable's wait passes up through every task above it: here the task
// that waits on the mailbox is itself awaited by another.
Task<size_t> MessageLength(Mailbox& mailbox) {
  const std::string message = co_await Mailbox::Receive(mailbox);
  co_return message.size();
}

Task<> StoreLength(Mailbox& mailbox, size_t& length) {
  length = co_await MessageLength(mailbox);
}

TEST(AwaitableTest, WakingContinuesTheWholeChainOfTasks) {
  Mailbox mailbox;
  size_t length = 0;
  TaskScope scope;
  scope.Spawn(StoreLength(mailbox, length));
  EXPECT_EQ(length, 0);
  mailbox.Deliver("four");
  EXPECT_EQ(length, 4);
}

// A value that may or may not be available yet. With Ready, an awaitable can
// say "no need to wait" and the task carries on without suspending.
class Countdown : public Awaitable<Countdown> {
 public:
  explicit Countdown(int remaining) : remaining_(remaining) {}

  bool Ready() const { return remaining_ == 0; }
  void Start(Waker waker) { waker_ = waker; }
  int Finish() const { return remaining_; }

  // Counts down by one, waking the waiter on reaching zero.
  void Tick() {
    if (--remaining_ == 0) waker_.Wake();
  }

 private:
  int remaining_;
  Waker waker_;
};

Task<> AwaitThenSetFlag(Countdown& countdown, bool& done) {
  co_await countdown;
  done = true;
}

// When Ready says the wait is over, the task runs straight through.
TEST(AwaitableTest, ReadyAwaitableDoesNotSuspend) {
  Countdown countdown(0);
  bool done = false;
  TaskScope scope;
  scope.Spawn(AwaitThenSetFlag(countdown, done));
  EXPECT_TRUE(done);
}

// When it is not, the task suspends in Start as usual.
TEST(AwaitableTest, UnreadyAwaitableSuspendsUntilWoken) {
  Countdown countdown(1);
  bool done = false;
  TaskScope scope;
  scope.Spawn(AwaitThenSetFlag(countdown, done));
  EXPECT_FALSE(done);
  countdown.Tick();
  EXPECT_TRUE(done);
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
// What it costs to suspend a task and wake it again.
//
//   bazel run -c opt //async:awaitable_test -- --benchmark_filter=all
//
// Every wait in the server ends in one of these: a task suspends on an
// awaitable, control returns to the event loop, and later the event loop
// wakes the task. This measures that round trip with nothing else in it; in
// particular, no I/O.
//
// clang-format off
// Results. Written by perf/record_results.py; do not edit by hand.
//
//   Date      2026-10-05
//   CPU       Intel(R) Core(TM) i7-9700 CPU @ 3.00GHz, 8 cores, L3 12 MiB (1 instance)
//   Memory    31 GB
//   Disk      Samsung SSD 990 EVO Plus 4TB (ext4)
//   System    Linux 7.0.0-38-generic, CPU governor: powersave
//   Compiler  clang version 21.1.8
//   Build     bazel -c opt (-O2), C++20, no exceptions
//   I/O       io_uring, except where a benchmark's name says epoll
//
//   -------------------------------------------------------------------------------
//   Benchmark                                     Time             CPU   Iterations
//   -------------------------------------------------------------------------------
//   BM_SuspendAndWake                          2.74 ns         2.74 ns    154312208
//   BM_SuspendAndWakeUnderNestedTasks/1        21.6 ns         21.6 ns     18929926
//   BM_SuspendAndWakeUnderNestedTasks/4        68.9 ns         68.9 ns      6077001
//   BM_SuspendAndWakeUnderNestedTasks/16        242 ns          242 ns      1740633
// End of results.
// clang-format on

namespace {

// Something to wait on that the benchmark controls: Open wakes the waiter.
class Gate {
 public:
  class Pass : public Awaitable<Pass> {
   public:
    explicit Pass(Gate& gate) : gate_(gate) {}
    void Start(Waker waker) { gate_.waiting_ = waker; }
    void Finish() const {}

   private:
    Gate& gate_;
  };

  void Open() const { waiting_.Wake(); }

 private:
  Waker waiting_;
};

Task<> PassForever(Gate& gate) {
  for (;;) co_await Gate::Pass(gate);
}

// One suspend and one wake. Each Open continues the task, which goes round
// its loop and suspends again before Open returns.
void BM_SuspendAndWake(benchmark::State& state) {
  Gate gate;
  TaskScope scope;
  scope.Spawn(PassForever(gate));
  for (auto _ : state) gate.Open();
}
BENCHMARK(BM_SuspendAndWake);

// Waits at the gate from `depth` tasks down.
Task<> PassNested(Gate& gate, int depth) {
  if (depth == 0) {
    co_await Gate::Pass(gate);
  } else {
    co_await PassNested(gate, depth - 1);
  }
}

Task<> PassNestedForever(Gate& gate, int depth) {
  for (;;) co_await PassNested(gate, depth);
}

// The same, with the wait at the bottom of a chain of tasks. A wake continues
// only the innermost task; this shows what each extra layer above it adds,
// which is the cost of creating and finishing that layer's task.
void BM_SuspendAndWakeUnderNestedTasks(benchmark::State& state) {
  Gate gate;
  TaskScope scope;
  scope.Spawn(PassNestedForever(gate, static_cast<int>(state.range(0))));
  for (auto _ : state) gate.Open();
}
BENCHMARK(BM_SuspendAndWakeUnderNestedTasks)->Arg(1)->Arg(4)->Arg(16);

}  // namespace
