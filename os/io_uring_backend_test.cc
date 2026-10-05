// What is particular to the io_uring backend. Everything the two backends
// have in common is in io_test.cc, which runs on both.

#include "os/io_uring_backend.h"

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <utility>

#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "async/task_scope.h"
#include "gtest/gtest.h"
#include "os/io.h"

os::IoDriver NewIoUringDriver(const os::IoUringOptions& options = {}) {
  absl::StatusOr<os::IoDriver> driver = os::IoDriver::Create({
      .backend = os::IoBackend::kIoUring,
      .io_uring = options,
  });
  ABSL_EXPECT_OK(driver);
  return std::move(*driver);
}

Task<> SleepThenFinish(bool& done) {
  co_await os::Sleep(std::chrono::milliseconds(1));
  done = true;
}

// The event loop in miniature: keep waking the tasks whose operations have
// finished until one of them sets `done`.
void RunUntil(os::IoDriver& driver, const bool& done) {
  while (!done) driver.WakeFinished(done);
}

// The backend can be made directly, which is what IoDriver does.
TEST(IoUringBackendTest, IsAvailableOnThisSystem) {
  ABSL_EXPECT_OK(os_internal::NewIoUringBackend({}));
}

// On io_uring nothing finishes without a trip to the kernel, not even a
// sleep of no time at all: the task always suspends first.
TEST(IoUringBackendTest, EveryOperationWaitsForTheKernel) {
  os::IoDriver driver = NewIoUringDriver();
  TaskScope scope;
  driver.Attach();
  bool done = false;
  scope.Spawn([](bool& done) -> Task<> {
    co_await os::Sleep(os::Duration::zero());
    done = true;
  }(done));
  EXPECT_FALSE(done);
  RunUntil(driver, done);
  driver.Detach();
}

// The other way to have the kernel finish operations; see IoUringOptions.
TEST(IoUringBackendTest, CompletionWorkCanBeImmediate) {
  os::IoDriver driver = NewIoUringDriver({
      .queue_depth = 8,
      .completion_work = os::IoUringOptions::CompletionWork::kImmediately,
  });
  TaskScope scope;
  driver.Attach();
  bool done = false;
  scope.Spawn(SleepThenFinish(done));
  RunUntil(driver, done);
  driver.Detach();
}

// More operations than the queue holds are fine: the queue is handed to the
// kernel early to make room.
TEST(IoUringBackendTest, QueueDepthIsNotALimitOnOperations) {
  os::IoDriver driver = NewIoUringDriver({
      .queue_depth = 4,
  });
  TaskScope scope;
  driver.Attach();
  constexpr int kTasks = 50;
  int finished = 0;
  bool all_finished = false;
  for (int i = 0; i < kTasks; ++i) {
    scope.Spawn([](int& finished, bool& all_finished) -> Task<> {
      co_await os::Sleep(std::chrono::milliseconds(1));
      all_finished = ++finished == kTasks;
    }(finished, all_finished));
  }
  RunUntil(driver, all_finished);
  EXPECT_EQ(finished, kTasks);
  driver.Detach();
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
// What batching buys on io_uring.
//
//   bazel run -c opt //os:io_uring_backend_test -- --benchmark_filter=all
//
// A number of tasks each do one operation per trip into the kernel. One
// iteration is one trip; compare operations/s across batch sizes to see how
// much of an operation's cost is the trip itself.
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
//   -----------------------------------------------------------------------------------
//   Benchmark                         Time             CPU   Iterations UserCounters...
//   -----------------------------------------------------------------------------------
//   BM_BatchedOperations/1          887 ns          886 ns       471620 operations/s=1.12838M/s
//   BM_BatchedOperations/16       10642 ns        10631 ns        38627 operations/s=1.50497M/s
//   BM_BatchedOperations/256      26712 ns        26677 ns        15542 operations/s=1.49942M/s
// End of results.
// clang-format on

namespace {

Task<> SleepForever(uint64_t& operations) {
  for (;;) {
    co_await os::Sleep(os::Duration::zero());
    ++operations;
  }
}

void BM_BatchedOperations(benchmark::State& state) {
  os::IoDriver driver = NewIoUringDriver();
  driver.Attach();
  {
    TaskScope scope;
    uint64_t operations = 0;
    for (int i = 0; i < state.range(0); ++i) {
      scope.Spawn(SleepForever(operations));
    }
    const bool stop = false;
    for (auto _ : state) driver.WakeFinished(stop);
    state.counters["operations/s"] = benchmark::Counter(
        static_cast<double>(operations), benchmark::Counter::kIsRate);
    // Stop the timers before the scope frees the tasks waiting on them.
    driver.CancelAll();
  }
  driver.Detach();
}
BENCHMARK(BM_BatchedOperations)->Arg(1)->Arg(16)->Arg(256);

}  // namespace
