// Sequence by example: an asynchronous function that hands out values one
// at a time and keeps its place in between.

#include "async/sequence.h"

#include <benchmark/benchmark.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "async/awaitable.h"
#include "async/task.h"
#include "async/task_scope.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

using testing::ElementsAre;
using testing::Eq;
using testing::Optional;
using testing::Pointee;

// Runs `test`, which must finish without anything outside waking it.
void RunToCompletion(Task<> test) {
  TaskScope scope;
  scope.Spawn(std::move(test));
  EXPECT_EQ(scope.unfinished(), 0);
}

// Something a task can wait for until a test says so.
class Gate : public Awaitable<Gate> {
 public:
  void Start(Waker waker) { waker_ = waker; }
  void Finish() const {}

  void Open() const { waker_.Wake(); }

 private:
  Waker waker_;
};

Sequence<int> CountTo(int last) {
  for (int i = 1; i <= last; ++i) co_yield i;
}

// Collects everything a sequence has to give.
Task<std::vector<int>> Drain(Sequence<int> sequence) {
  std::vector<int> values;
  while (std::optional<int> value = co_await sequence.Next()) {
    values.push_back(*value);
  }
  co_return values;
}

// The basic contract: each Next is the next value, in order, and nothing
// once the function has returned.
TEST(SequenceTest, HandsOutEachValueInTurnAndThenNothing) {
  RunToCompletion([]() -> Task<> {
    Sequence<int> numbers = CountTo(3);

    EXPECT_THAT(co_await numbers.Next(), Optional(1));
    EXPECT_THAT(co_await numbers.Next(), Optional(2));
    EXPECT_THAT(co_await numbers.Next(), Optional(3));
    EXPECT_THAT(co_await numbers.Next(), Eq(std::nullopt));

    // And goes on being nothing.
    EXPECT_THAT(co_await numbers.Next(), Eq(std::nullopt));
  }());
}

// The usual way to use one: a loop that ends when the sequence does.
TEST(SequenceTest, IsReadWithAWhileLoop) {
  RunToCompletion([]() -> Task<> {
    EXPECT_THAT(co_await Drain(CountTo(4)), ElementsAre(1, 2, 3, 4));
    EXPECT_THAT(co_await Drain(CountTo(0)), ElementsAre());
  }());
}

Sequence<int> CountAndTell(std::vector<std::string>& events) {
  events.push_back("started");
  co_yield 1;
  events.push_back("carried on");
  co_yield 2;
  events.push_back("finished");
}

// A sequence runs only when asked for a value, and only as far as the next
// one: it and whoever reads it take turns.
TEST(SequenceTest, RunsOnlyAsFarAsEachValue) {
  RunToCompletion([]() -> Task<> {
    std::vector<std::string> events;
    Sequence<int> numbers = CountAndTell(events);
    EXPECT_THAT(events, ElementsAre());  // Nothing until the first Next.

    co_await numbers.Next();
    EXPECT_THAT(events, ElementsAre("started"));

    co_await numbers.Next();
    EXPECT_THAT(events, ElementsAre("started", "carried on"));

    co_await numbers.Next();
    EXPECT_THAT(events, ElementsAre("started", "carried on", "finished"));
  }());
}

// What makes it worth having: the function keeps its place, and its local
// variables, from one value to the next. Here that is a running total.
Sequence<int> RunningTotals(std::vector<int> amounts) {
  int total = 0;
  for (const int amount : amounts) {
    total += amount;
    co_yield total;
  }
}

TEST(SequenceTest, KeepsItsPlaceBetweenValues) {
  RunToCompletion([]() -> Task<> {
    EXPECT_THAT(co_await Drain(RunningTotals({5, 10, 20})),
                ElementsAre(5, 15, 35));
  }());
}

Task<int> Double(int value) { co_return 2 * value; }

Sequence<int> Doubles(int last) {
  for (int i = 1; i <= last; ++i) co_yield co_await Double(i);
}

// Between values a sequence can await tasks like any asynchronous function.
TEST(SequenceTest, AwaitsTasksBetweenValues) {
  RunToCompletion([]() -> Task<> {
    EXPECT_THAT(co_await Drain(Doubles(3)), ElementsAre(2, 4, 6));
  }());
}

Sequence<int> AfterTheGate(Gate& gate) {
  co_await gate;
  co_yield 7;
}

// And it can wait for things outside itself, in which case whoever awaits
// Next waits with it.
TEST(SequenceTest, NextWaitsWhileTheSequenceDoes) {
  Gate gate;
  std::optional<int> taken;
  TaskScope scope;
  scope.Spawn([](Gate& gate, std::optional<int>& taken) -> Task<> {
    Sequence<int> sequence = AfterTheGate(gate);
    taken = co_await sequence.Next();
  }(gate, taken));
  EXPECT_EQ(taken, std::nullopt);  // Both are waiting.

  gate.Open();

  EXPECT_THAT(taken, Optional(7));
}

Sequence<std::unique_ptr<int>> Boxes() {
  co_yield std::make_unique<int>(1);
  co_yield std::make_unique<int>(2);
}

// Values are moved out, so they need not be copyable.
TEST(SequenceTest, HandsOutValuesThatCanOnlyBeMoved) {
  RunToCompletion([]() -> Task<> {
    Sequence<std::unique_ptr<int>> boxes = Boxes();

    EXPECT_THAT(co_await boxes.Next(), Optional(Pointee(1)));
    EXPECT_THAT(co_await boxes.Next(), Optional(Pointee(2)));
  }());
}

// A sequence need not be read to its end. Destroying it part way through
// destroys what it was holding.
TEST(SequenceTest, CanBeAbandonedPartWay) {
  const auto held = std::make_shared<int>(0);
  RunToCompletion([](std::shared_ptr<int> held) -> Task<> {
    {
      Sequence<int> sequence = [](std::shared_ptr<int> held) -> Sequence<int> {
        co_yield 1;
        co_yield 2;
      }(held);
      co_await sequence.Next();
      EXPECT_GT(held.use_count(), 2);  // The sequence has a copy.
    }
    EXPECT_EQ(held.use_count(), 2);  // And now it has gone.
  }(held));
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
// What a value from a sequence costs, beside what it costs to get the same
// value by awaiting a task each time.
//
//   bazel run -c opt //async:sequence_test -- --benchmark_filter=all
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
//   -------------------------------------------------------------------------
//   Benchmark                               Time             CPU   Iterations
//   -------------------------------------------------------------------------
//   BM_NextFromSequence                  5.18 ns         5.18 ns     80929672
//   BM_NextFromTask                      10.7 ns         10.7 ns     39443748
//   BM_NextFromTaskBehindInterface       12.2 ns         12.2 ns     33439892
// End of results.
// clang-format on

Sequence<int> Forever() {
  for (int i = 0;; ++i) co_yield i;
}

Task<> TakeFromSequence(benchmark::State& state) {
  Sequence<int> numbers = Forever();
  for (auto _ : state) benchmark::DoNotOptimize(co_await numbers.Next());
}

// One value from a sequence: a switch to the function and back, with no
// memory allocated.
void BM_NextFromSequence(benchmark::State& state) {
  RunToCompletion(TakeFromSequence(state));
}
BENCHMARK(BM_NextFromSequence);

Task<int> NextNumber(int& next) { co_return next++; }

Task<> TakeFromTasks(benchmark::State& state) {
  int next = 0;
  for (auto _ : state) benchmark::DoNotOptimize(co_await NextNumber(next));
}

// The same value from a task made for the purpose each time, which is what
// a sequence replaces. The compiler can often make such a task free; see the
// next benchmark for when it cannot.
void BM_NextFromTask(benchmark::State& state) {
  RunToCompletion(TakeFromTasks(state));
}
BENCHMARK(BM_NextFromTask);

// A source of numbers behind an interface, as a connection or a gateway is,
// so that the compiler cannot see into the call.
class Numbers {
 public:
  virtual ~Numbers() = default;
  virtual Task<int> Next() = 0;
};

class Counter final : public Numbers {
 public:
  Task<int> Next() override { co_return next_++; }

 private:
  int next_ = 0;
};

Task<> TakeFromInterface(benchmark::State& state) {
  const std::unique_ptr<Numbers> numbers = std::make_unique<Counter>();
  for (auto _ : state) benchmark::DoNotOptimize(co_await numbers->Next());
}

// The same again through an interface: each value now allocates and frees a
// task's state.
void BM_NextFromTaskBehindInterface(benchmark::State& state) {
  RunToCompletion(TakeFromInterface(state));
}
BENCHMARK(BM_NextFromTaskBehindInterface);

}  // namespace
