// EventLoop and Spawn by example: how tasks get run, and how several of them
// share one thread.

#include "net/event_loop.h"

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "net/stream.h"

using testing::ElementsAre;

EventLoop NewLoop() {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_EXPECT_OK(loop);
  return std::move(*loop);
}

Task<> SetFlag(bool& flag) {
  flag = true;
  co_return;
}

// Two ends of one connection, to give tasks some real I/O to wait for.
struct Pair {
  std::unique_ptr<net::Stream> client;
  std::unique_ptr<net::Stream> server;
};

Task<Pair> Connect() {
  const absl::StatusOr<net::Listener> listener = net::Listener::OnLoopback();
  ABSL_EXPECT_OK(listener);
  absl::StatusOr<std::unique_ptr<net::Stream>> client = co_await net::Dial(
      listener->address(), net::After(std::chrono::seconds(5)));
  ABSL_EXPECT_OK(client);
  absl::StatusOr<std::unique_ptr<net::Stream>> server =
      co_await listener->Accept();
  ABSL_EXPECT_OK(server);
  co_return Pair{
      std::move(*client),
      std::move(*server),
  };
}

// Reads up to the next newline, which is consumed but not returned. Gives up,
// returning what it has, if the stream ends or stays silent for a minute.
Task<std::string> ReadLine(net::Stream& stream) {
  std::string line;
  for (;;) {
    char next = 0;
    const absl::StatusOr<size_t> count = co_await stream.Read(
        std::span<char>(&next, 1), net::After(std::chrono::minutes(1)));
    if (!count.ok() || *count == 0 || next == '\n') co_return line;
    line.push_back(next);
  }
}

Task<> Write(net::Stream& stream, std::string_view text) {
  ABSL_EXPECT_OK(co_await stream.Write(text));
}

// Sends a line each way and checks that it arrives.
Task<> ExchangeLines() {
  const Pair pair = co_await Connect();
  co_await Write(*pair.client, "ping\n");
  EXPECT_EQ(co_await ReadLine(*pair.server), "ping");
  co_await Write(*pair.server, "pong\n");
  EXPECT_EQ(co_await ReadLine(*pair.client), "pong");
}

Task<int> Add(int a, int b) { co_return a + b; }

// A loop says how it does its I/O, which depends on the --io_backend flag and
// on what the system allows.
TEST(EventLoopTest, ReportsItsIoBackend) {
  EXPECT_THAT(NewLoop().io_backend(), testing::AnyOf("io_uring", "epoll"));
}

// Run blocks the calling thread until the task it was given has finished.
TEST(EventLoopTest, RunReturnsWhenMainFinishes) {
  bool ran = false;
  NewLoop().Run(SetFlag(ran));
  EXPECT_TRUE(ran);
}

// Run is the bridge from ordinary code to asynchronous code: it hands back
// whatever the task produced.
TEST(EventLoopTest, RunReturnsTheTasksResult) {
  EXPECT_EQ(NewLoop().Run(Add(2, 3)), 5);
}

// Spawn does not wait for the event loop to get round to the task: it runs
// straight away, up to the first point where it has to wait.
TEST(EventLoopTest, SpawnStartsTaskImmediately) {
  NewLoop().Run([]() -> Task<> {
    bool ran = false;
    Spawn(SetFlag(ran));
    EXPECT_TRUE(ran);
    co_return;
  }());
}

// The point of an event loop: while one task waits for I/O, the thread runs
// another.
TEST(EventLoopTest, TasksTakeTurnsWhileWaiting) {
  NewLoop().Run([]() -> Task<> {
    const Pair pair = co_await Connect();
    std::vector<std::string> events;

    Spawn([](net::Stream& server, std::vector<std::string>& events) -> Task<> {
      events.push_back("reader waits");
      co_await ReadLine(server);
      events.push_back("reader got line");
      co_await Write(server, "ack\n");
    }(*pair.server, events));

    // The reader is now suspended, and control is back here.
    events.push_back("main continues");
    co_await Write(*pair.client, "line\n");
    // Waiting for the reply gives the reader its turn.
    co_await ReadLine(*pair.client);

    EXPECT_THAT(events, ElementsAre("reader waits", "main continues",
                                    "reader got line"));
  }());
}

// Sleep pauses one task, not the thread: the other task gets to finish
// first even though it was started second.
TEST(EventLoopTest, SleepLetsOtherTasksRun) {
  NewLoop().Run([]() -> Task<> {
    std::vector<std::string> events;
    Spawn([](std::vector<std::string>& events) -> Task<> {
      co_await Sleep(std::chrono::milliseconds(5));
      events.push_back("sleeper woke");
    }(events));
    events.push_back("main continues");
    co_await Sleep(std::chrono::milliseconds(20));

    EXPECT_THAT(events, ElementsAre("main continues", "sleeper woke"));
  }());
}

// Run does not wait for spawned tasks. Those still waiting when main
// finishes are dropped, never resumed.
TEST(EventLoopTest, RunAbandonsTasksStillWaitingWhenMainFinishes) {
  NewLoop().Run([]() -> Task<> {
    Pair pair = co_await Connect();
    Spawn([](std::unique_ptr<net::Stream> server) -> Task<> {
      // Waits for a newline that never comes.
      co_await ReadLine(*server);
      ADD_FAILURE() << "abandoned task was resumed";
    }(std::move(pair.server)));
    co_await Write(*pair.client, "no newline");
  }());
}

// A server creates its loops up front, where failure is easy to report, and
// then hands each to its own thread.
TEST(EventLoopTest, RunsOnADifferentThreadThanItWasCreatedOn) {
  EventLoop loop = NewLoop();
  std::jthread([&loop] { loop.Run(ExchangeLines()); });
}

// Every thread has its own loop; they do not interfere.
TEST(EventLoopTest, SeveralThreadsEachRunTheirOwnLoop) {
  std::vector<std::jthread> threads;
  threads.reserve(4);
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([] { NewLoop().Run(ExchangeLines()); });
  }
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
// What the event loop costs: setting one up, and each turn of it.
//
//   bazel run -c opt //net:event_loop_test -- --benchmark_filter=all
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
//   --------------------------------------------------------------
//   Benchmark                    Time             CPU   Iterations
//   --------------------------------------------------------------
//   BM_CreateAndRun          24188 ns        23590 ns        17097
//   BM_OneTurnOfTheLoop        907 ns          907 ns       469342
//   BM_Spawn                  40.3 ns         40.3 ns     10049798
// End of results.
// clang-format on

namespace {

Task<> Nothing() { co_return; }

// Creating a loop and running a task that finishes at once. Dominated by
// setting up and tearing down the loop's io_uring, which is why the server
// creates its loops once, at startup.
void BM_CreateAndRun(benchmark::State& state) {
  for (auto _ : state) EventLoop::Create()->Run(Nothing());
}
BENCHMARK(BM_CreateAndRun);

Task<> SleepInLoop(benchmark::State& state) {
  for (auto _ : state) co_await Sleep(std::chrono::nanoseconds(0));
}

// One turn of the loop: a task waits for something that is already due, the
// loop goes to the kernel, comes back, and wakes it. The least any wait for
// I/O can cost.
void BM_OneTurnOfTheLoop(benchmark::State& state) {
  EventLoop::Create()->Run(SleepInLoop(state));
}
BENCHMARK(BM_OneTurnOfTheLoop);

Task<> SpawnInLoop(benchmark::State& state) {
  for (auto _ : state) Spawn(Nothing());
  co_return;
}

// Starting an independent task on a running loop, as the server does for
// each connection.
void BM_Spawn(benchmark::State& state) {
  EventLoop::Create()->Run(SpawnInLoop(state));
}
BENCHMARK(BM_Spawn);

}  // namespace
