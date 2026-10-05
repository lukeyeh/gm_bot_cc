// TaskScope by example: starting tasks without waiting for them.

#include "async/task_scope.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <vector>

#include "async/awaitable.h"
#include "async/task.h"
#include "gtest/gtest.h"

// Something to wait on that the test controls: Open lets the waiter through.
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

Task<> SetFlag(bool& flag) {
  flag = true;
  co_return;
}

Task<> PassThenSetFlag(Gate& gate, bool& flag) {
  co_await Gate::Pass(gate);
  flag = true;
}

// Records that it was destroyed, to show when a task's locals are cleaned up.
struct Tracker {
  explicit Tracker(bool& destroyed) : destroyed_(destroyed) {}
  ~Tracker() { destroyed_ = true; }
  bool& destroyed_;
};

Task<> HoldTrackerAtGate(Gate& gate, bool& destroyed) {
  const Tracker tracker(destroyed);
  co_await Gate::Pass(gate);
}

// Spawn does not queue the task for later: it runs straight away, up to the
// first point where it has to wait.
TEST(TaskScopeTest, SpawnStartsTaskImmediately) {
  TaskScope scope;
  bool ran = false;
  scope.Spawn(SetFlag(ran));
  EXPECT_TRUE(ran);
  // It finished, so the scope has already forgotten it.
  EXPECT_EQ(scope.unfinished(), 0);
}

// A task that waits stays in the scope until it is woken and finishes.
TEST(TaskScopeTest, TracksTasksUntilTheyFinish) {
  TaskScope scope;
  Gate gate;
  bool passed = false;
  scope.Spawn(PassThenSetFlag(gate, passed));
  EXPECT_FALSE(passed);
  EXPECT_EQ(scope.unfinished(), 1);

  gate.Open();
  EXPECT_TRUE(passed);
  EXPECT_EQ(scope.unfinished(), 0);
}

// When the scope ends, tasks still waiting are destroyed where they stand:
// their local variables are cleaned up, and the rest of the task never runs.
TEST(TaskScopeTest, AbandonsUnfinishedTasksWhenDestroyed) {
  Gate gate;
  bool destroyed = false;
  {
    TaskScope scope;
    scope.Spawn(HoldTrackerAtGate(gate, destroyed));
    EXPECT_FALSE(destroyed);
  }
  EXPECT_TRUE(destroyed);
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
// What it costs to start a task nobody awaits.
//
//   bazel run -c opt //async:task_scope_test -- --benchmark_filter=all
//
// The server spawns one task per connection, so this is part of the cost of
// accepting a client.
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
//   -------------------------------------------------------------------
//   Benchmark                         Time             CPU   Iterations
//   -------------------------------------------------------------------
//   BM_SpawnTaskThatFinishes       25.3 ns         25.3 ns     16991585
//   BM_SpawnWaitAndFinish          26.3 ns         26.3 ns     15777949
//   BM_SpawnAndAbandon/1           40.4 ns         40.4 ns     10869152 items_per_second=24.7553M/s
//   BM_SpawnAndAbandon/64          7000 ns         6999 ns        58583 items_per_second=9.14355M/s
//   BM_SpawnAndAbandon/4096      448896 ns       448877 ns          957 items_per_second=9.125M/s
// End of results.
// clang-format on

namespace {

Task<> Nothing() { co_return; }

Task<> PassOnce(Gate& gate) { co_await Gate::Pass(gate); }

// Spawning a task that finishes at once: the scope starts it, it runs to the
// end, and it is freed, all inside Spawn.
void BM_SpawnTaskThatFinishes(benchmark::State& state) {
  TaskScope scope;
  for (auto _ : state) scope.Spawn(Nothing());
}
BENCHMARK(BM_SpawnTaskThatFinishes);

// The life of a typical spawned task: it starts, waits for something, is
// woken, and finishes.
void BM_SpawnWaitAndFinish(benchmark::State& state) {
  Gate gate;
  TaskScope scope;
  for (auto _ : state) {
    scope.Spawn(PassOnce(gate));
    gate.Open();
  }
}
BENCHMARK(BM_SpawnWaitAndFinish);

// Starting a number of tasks that then wait, and abandoning them all by
// ending the scope. Shows the cost of tracking many unfinished tasks at once
// and of cleaning them up.
void BM_SpawnAndAbandon(benchmark::State& state) {
  const size_t tasks = static_cast<size_t>(state.range(0));
  std::vector<Gate> gates(tasks);
  for (auto _ : state) {
    TaskScope scope;
    for (Gate& gate : gates) scope.Spawn(PassOnce(gate));
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_SpawnAndAbandon)->Arg(1)->Arg(64)->Arg(4096);

}  // namespace
