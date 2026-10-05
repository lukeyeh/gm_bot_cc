// What is particular to the epoll backend. Everything the two backends have
// in common is in io_test.cc, which runs on both.

#include "os/epoll_backend.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "async/task_scope.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "os/io.h"
#include "os/socket.h"

using absl_testing::IsOkAndHolds;

os::IoDriver NewEpollDriver() {
  absl::StatusOr<os::IoDriver> driver = os::IoDriver::Create({
      .backend = os::IoBackend::kEpoll,
  });
  ABSL_EXPECT_OK(driver);
  return std::move(*driver);
}

os::Socket NewListener() {
  os::Socket listener = *os::Socket::CreateTcp();
  ABSL_EXPECT_OK(listener.Bind(*os::SocketAddress::Parse("127.0.0.1", 0)));
  ABSL_EXPECT_OK(listener.Listen());
  return listener;
}

// The event loop in miniature: keep waking the tasks whose operations have
// finished until one of them sets `done`.
void RunUntil(os::IoDriver& driver, const bool& done) {
  while (!done) driver.WakeFinished(done);
}

// The backend can be made directly, which is what IoDriver does.
TEST(EpollBackendTest, IsAvailable) {
  ABSL_EXPECT_OK(os_internal::NewEpollBackend());
}

// Connects, then sends and receives without ever returning to the driver.
Task<> SendAndReceiveReadyData(const os::Socket& listener, bool& connected,
                               bool& done) {
  const os::Socket client = *os::Socket::CreateTcp();
  ABSL_EXPECT_OK(co_await os::Connect(client, *listener.LocalAddress()));
  const absl::StatusOr<os::Socket> server = co_await os::Accept(listener);
  ABSL_EXPECT_OK(server);
  connected = true;

  EXPECT_THAT(co_await os::Send(client, "ready", ""), IsOkAndHolds(5));
  std::string buffer(100, '\0');
  EXPECT_THAT(co_await os::Receive(*server, buffer), IsOkAndHolds(5));
  done = true;
}

// The reason epoll is not much slower in practice: an operation that can
// proceed is done on the spot, and the task does not suspend. Here the send
// has room and the receive has data waiting, so once connected the task runs
// to the end without the driver being asked for anything.
TEST(EpollBackendTest, OperationsThatCanProceedDoNotWait) {
  os::IoDriver driver = NewEpollDriver();
  const os::Socket listener = NewListener();
  TaskScope scope;
  driver.Attach();

  bool connected = false;
  bool done = false;
  scope.Spawn(SendAndReceiveReadyData(listener, connected, done));
  // Over loopback even the connection is usually made at once; if not, wait
  // for it and nothing more.
  RunUntil(driver, connected);
  EXPECT_TRUE(done);

  driver.CancelAll();
  driver.Detach();
}

Task<> AcceptOne(const os::Socket& listener, int& accepted, bool& done) {
  const absl::StatusOr<os::Socket> peer = co_await os::Accept(listener);
  ABSL_EXPECT_OK(peer);
  ++accepted;
  done = true;
}

Task<> ConnectOnce(const os::Socket& listener, bool& done) {
  const os::Socket client = *os::Socket::CreateTcp();
  ABSL_EXPECT_OK(co_await os::Connect(client, *listener.LocalAddress()));
  done = true;
}

// Several threads can wait on one listening socket, each with its own
// driver, which is how the server spreads connections over cores. Each
// connection is accepted exactly once.
TEST(EpollBackendTest, ThreadsShareAListener) {
  constexpr int kThreads = 4;
  const os::Socket listener = NewListener();
  std::vector<int> accepted(kThreads, 0);
  {
    std::vector<std::jthread> acceptors;
    acceptors.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
      acceptors.emplace_back([&listener, &count = accepted[i]] {
        os::IoDriver driver = NewEpollDriver();
        TaskScope scope;
        driver.Attach();
        bool done = false;
        scope.Spawn(AcceptOne(listener, count, done));
        RunUntil(driver, done);
        driver.Detach();
      });
    }

    // One connection per waiting thread.
    os::IoDriver driver = NewEpollDriver();
    TaskScope scope;
    driver.Attach();
    for (int i = 0; i < kThreads; ++i) {
      bool done = false;
      scope.Spawn(ConnectOnce(listener, done));
      RunUntil(driver, done);
    }
    driver.Detach();
  }
  for (const int count : accepted) EXPECT_EQ(count, 1);
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
// The two paths an operation can take on epoll.
//
//   bazel run -c opt //os:epoll_backend_test -- --benchmark_filter=all
//
// SendThenReceive never waits: every operation is one system call. Compare
// with ReceiveThenSend, where the receive is awaited before the data exists,
// so it has to be parked, woken by epoll and tried again.
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
//   -------------------------------------------------------------
//   Benchmark                   Time             CPU   Iterations
//   -------------------------------------------------------------
//   BM_SendThenReceive       3383 ns         3383 ns       123363
//   BM_ReceiveThenSend       4323 ns         4323 ns        97749
// End of results.
// clang-format on

namespace {

struct Pair {
  os::Socket client;
  os::Socket server;
};

Task<Pair> Connect(const os::Socket& listener) {
  os::Socket client = *os::Socket::CreateTcp();
  (co_await os::Connect(client, *listener.LocalAddress())).IgnoreError();
  co_return Pair{
      std::move(client),
      *co_await os::Accept(listener),
  };
}

Task<> SendThenReceiveInLoop(const os::Socket& listener,
                             benchmark::State& state, bool& done) {
  const Pair pair = co_await Connect(listener);
  std::string buffer(64, '\0');
  for (auto _ : state) {
    (co_await os::Send(pair.client, "0123456789abcdef", "")).IgnoreError();
    benchmark::DoNotOptimize(co_await os::Receive(pair.server, buffer));
  }
  done = true;
}

void BM_SendThenReceive(benchmark::State& state) {
  os::IoDriver driver = NewEpollDriver();
  const os::Socket listener = NewListener();
  driver.Attach();
  {
    TaskScope scope;
    bool done = false;
    scope.Spawn(SendThenReceiveInLoop(listener, state, done));
    RunUntil(driver, done);
  }
  driver.Detach();
}
BENCHMARK(BM_SendThenReceive);

Task<> ReceiveForever(const os::Socket& server) {
  std::string buffer(64, '\0');
  for (;;) {
    const absl::StatusOr<size_t> received =
        co_await os::Receive(server, buffer);
    if (!received.ok() || *received == 0) co_return;
  }
}

Task<> ReceiveThenSendInLoop(const os::Socket& listener, TaskScope& scope,
                             benchmark::State& state, bool& done) {
  const Pair pair = co_await Connect(listener);
  // The receiver runs first and parks, having nothing to read.
  scope.Spawn(ReceiveForever(pair.server));
  for (auto _ : state) {
    (co_await os::Send(pair.client, "0123456789abcdef", "")).IgnoreError();
    // Lets the driver wake the receiver, which reads and parks again.
    co_await os::Sleep(os::Duration::zero());
  }
  done = true;
}

void BM_ReceiveThenSend(benchmark::State& state) {
  os::IoDriver driver = NewEpollDriver();
  const os::Socket listener = NewListener();
  driver.Attach();
  {
    TaskScope scope;
    bool done = false;
    scope.Spawn(ReceiveThenSendInLoop(listener, scope, state, done));
    RunUntil(driver, done);
    driver.CancelAll();
  }
  driver.Detach();
}
BENCHMARK(BM_ReceiveThenSend);

}  // namespace
