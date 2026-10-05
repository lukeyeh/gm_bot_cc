// IoDriver and the operations by example: asynchronous I/O using only tasks
// and this package. EventLoop, a layer up, wraps what these tests do by hand:
// attach a driver, start some tasks, and keep waking them as their I/O
// finishes.
//
// Every test here runs twice, once on each backend, because the point of the
// interface is that code using it cannot tell them apart.

#include "os/io.h"

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "async/task_scope.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "os/socket.h"

using absl_testing::IsOkAndHolds;
using absl_testing::StatusIs;
using std::chrono::milliseconds;

os::IoDriver NewDriver(os::IoBackend backend) {
  absl::StatusOr<os::IoDriver> driver = os::IoDriver::Create({
      .backend = backend,
  });
  ABSL_EXPECT_OK(driver);
  return std::move(*driver);
}

// A socket listening on a free local port.
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

// Gives every test a driver of the backend under test, attached to the test's
// thread, and a scope to start tasks in.
class IoTest : public testing::TestWithParam<os::IoBackend> {
 protected:
  void SetUp() override { driver_.Attach(); }
  void TearDown() override {
    // Stop any I/O still in progress before scope_ frees the tasks doing it.
    driver_.CancelAll();
    driver_.Detach();
  }

  void RunUntil(const bool& done) { ::RunUntil(driver_, done); }

  os::IoDriver driver_ = NewDriver(GetParam());
  TaskScope scope_;
};

INSTANTIATE_TEST_SUITE_P(Backends, IoTest,
                         testing::Values(os::IoBackend::kIoUring,
                                         os::IoBackend::kEpoll),
                         [](const testing::TestParamInfo<os::IoBackend>& info) {
                           return std::string(os::IoBackendName(info.param));
                         });

// A driver reports the backend it ended up with.
TEST_P(IoTest, UsesTheBackendAskedFor) {
  EXPECT_EQ(driver_.backend(), GetParam());
}

Task<> SleepThenFinish(milliseconds duration, bool& done) {
  co_await os::Sleep(duration);
  done = true;
}

// Awaiting an operation that has to wait suspends the task and returns to
// whoever started it. The task continues from inside WakeFinished.
TEST_P(IoTest, AwaitingSuspendsUntilTheDriverWakes) {
  bool done = false;
  scope_.Spawn(SleepThenFinish(milliseconds(1), done));
  EXPECT_FALSE(done);
  RunUntil(done);
  EXPECT_TRUE(done);
}

// Sleeps end in order of duration, not in the order they were started.
TEST_P(IoTest, ShorterSleepEndsFirst) {
  bool long_done = false;
  bool short_done = false;
  scope_.Spawn(SleepThenFinish(milliseconds(40), long_done));
  scope_.Spawn(SleepThenFinish(milliseconds(1), short_done));
  RunUntil(short_done);
  EXPECT_FALSE(long_done);
  RunUntil(long_done);
}

// Both ends of a connection, in one task.
Task<> ExchangeGreeting(const os::Socket& listener, bool& done) {
  const os::Socket client = *os::Socket::CreateTcp();
  ABSL_EXPECT_OK(co_await os::Connect(client, *listener.LocalAddress()));
  const absl::StatusOr<os::Socket> server = co_await os::Accept(listener);
  ABSL_EXPECT_OK(server);

  // The two pieces arrive as one stream.
  EXPECT_THAT(co_await os::Send(client, "hello ", "world"), IsOkAndHolds(11));
  std::string buffer(100, '\0');
  const absl::StatusOr<size_t> received = co_await os::Receive(*server, buffer);
  ABSL_EXPECT_OK(received);
  EXPECT_EQ(buffer.substr(0, *received), "hello world");
  done = true;
}

TEST_P(IoTest, ConnectAcceptSendReceive) {
  const os::Socket listener = NewListener();
  bool done = false;
  scope_.Spawn(ExchangeGreeting(listener, done));
  RunUntil(done);
}

// Sends `total` bytes, however many Sends that takes.
Task<> SendAll(const os::Socket& socket, std::string_view data) {
  while (!data.empty()) {
    const absl::StatusOr<size_t> sent = co_await os::Send(socket, data, "");
    ABSL_EXPECT_OK(sent);
    if (!sent.ok()) co_return;
    data.remove_prefix(*sent);
  }
}

Task<> ExchangeLargeMessage(const os::Socket& listener, TaskScope& scope,
                            bool& done) {
  const os::Socket client = *os::Socket::CreateTcp();
  ABSL_EXPECT_OK(co_await os::Connect(client, *listener.LocalAddress()));
  const absl::StatusOr<os::Socket> server = co_await os::Accept(listener);
  ABSL_EXPECT_OK(server);

  // More than the kernel will buffer, so the sender has to wait for the
  // receiver part-way through.
  const std::string message(8 << 20, 'x');
  scope.Spawn(SendAll(client, message));

  std::string buffer(1 << 16, '\0');
  size_t total = 0;
  while (total < message.size()) {
    const absl::StatusOr<size_t> received =
        co_await os::Receive(*server, buffer);
    ABSL_EXPECT_OK(received);
    if (!received.ok() || *received == 0) break;
    total += *received;
  }
  EXPECT_EQ(total, message.size());
  done = true;
}

// A send that cannot finish at once waits for room and carries on; the
// reader and the writer take turns on one thread.
TEST_P(IoTest, LargeSendWaitsForTheReceiver) {
  const os::Socket listener = NewListener();
  bool done = false;
  scope_.Spawn(ExchangeLargeMessage(listener, scope_, done));
  RunUntil(done);
}

Task<> ReceiveFromSilentPeer(const os::Socket& listener, bool& done) {
  const os::Socket client = *os::Socket::CreateTcp();
  ABSL_EXPECT_OK(co_await os::Connect(client, *listener.LocalAddress()));
  std::string buffer(100, '\0');
  EXPECT_THAT(co_await os::Receive(client, buffer, milliseconds(10)),
              StatusIs(absl::StatusCode::kDeadlineExceeded));
  done = true;
}

// A time limit turns "wait forever" into DeadlineExceeded.
TEST_P(IoTest, TimeLimitStopsAnOperation) {
  const os::Socket listener = NewListener();
  bool done = false;
  scope_.Spawn(ReceiveFromSilentPeer(listener, done));
  RunUntil(done);
}

Task<> ReceiveAfterPeerCloses(const os::Socket& listener, bool& done) {
  const os::Socket client = *os::Socket::CreateTcp();
  ABSL_EXPECT_OK(co_await os::Connect(client, *listener.LocalAddress()));
  // Accepted and immediately destroyed, which closes the server's end.
  {
    const absl::StatusOr<os::Socket> server = co_await os::Accept(listener);
  }
  std::string buffer(100, '\0');
  EXPECT_THAT(co_await os::Receive(client, buffer), IsOkAndHolds(0));
  done = true;
}

// A peer closing is not an error: it is a successful receive of nothing.
TEST_P(IoTest, ReceiveOfZeroBytesMeansPeerClosed) {
  const os::Socket listener = NewListener();
  bool done = false;
  scope_.Spawn(ReceiveAfterPeerCloses(listener, done));
  RunUntil(done);
}

Task<> ConnectToNobody(os::SocketAddress address, bool& done) {
  const os::Socket client = *os::Socket::CreateTcp();
  // The kernel's ECONNREFUSED, as a status code and a readable message.
  EXPECT_THAT(co_await os::Connect(client, address),
              StatusIs(absl::StatusCode::kUnavailable,
                       testing::HasSubstr("Connection refused")));
  done = true;
}

// Kernel error numbers never reach the caller; they arrive as Status.
TEST_P(IoTest, KernelErrorsBecomeStatuses) {
  // The listener is destroyed at the end of this line, freeing its port.
  const os::SocketAddress free_address = *NewListener().LocalAddress();
  bool done = false;
  scope_.Spawn(ConnectToNobody(free_address, done));
  RunUntil(done);
}

Task<> ConnectThenWaitForever(const os::Socket& listener, bool& connected,
                              bool& woken) {
  const os::Socket client = *os::Socket::CreateTcp();
  ABSL_EXPECT_OK(co_await os::Connect(client, *listener.LocalAddress()));
  connected = true;
  std::string buffer(100, '\0');
  const absl::StatusOr<size_t> never = co_await os::Receive(client, buffer);
  woken = true;
}

// CancelAll is for shutting down: afterwards nothing is in progress, and the
// tasks that were waiting are never woken. The scope then frees them.
TEST_P(IoTest, CancelAllAbandonsWaitingTasks) {
  const os::Socket listener = NewListener();
  bool connected = false;
  bool woken = false;
  scope_.Spawn(ConnectThenWaitForever(listener, connected, woken));
  RunUntil(connected);

  driver_.CancelAll();
  EXPECT_FALSE(woken);
  EXPECT_EQ(scope_.unfinished(), 1);
}

// kAuto picks a backend that works here, whichever that is.
TEST(IoDriverTest, AutoPicksAWorkingBackend) {
  os::IoDriver driver = NewDriver(os::IoBackend::kAuto);
  EXPECT_NE(driver.backend(), os::IoBackend::kAuto);

  TaskScope scope;
  driver.Attach();
  bool done = false;
  scope.Spawn(SleepThenFinish(milliseconds(1), done));
  RunUntil(driver, done);
  driver.Detach();
}

// The names are what the --io_backend flag accepts.
TEST(IoBackendTest, ParsesAndPrintsAsAFlag) {
  os::IoBackend backend = os::IoBackend::kAuto;
  std::string error;
  EXPECT_TRUE(AbslParseFlag("epoll", &backend, &error));
  EXPECT_EQ(backend, os::IoBackend::kEpoll);
  EXPECT_EQ(AbslUnparseFlag(os::IoBackend::kIoUring), "io_uring");

  EXPECT_FALSE(AbslParseFlag("select", &backend, &error));
  EXPECT_EQ(error, "must be auto, io_uring or epoll");
}

// Operations go to the calling thread's driver, so there must be one.
TEST(IoDeathTest, OperationsNeedAnAttachedDriver) {
  TaskScope scope;
  bool done = false;
  EXPECT_DEATH(scope.Spawn(SleepThenFinish(milliseconds(1), done)),
               "no IoDriver attached");
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
// What one I/O operation costs on each backend.
//
//   bazel run -c opt //os:io_test -- --benchmark_filter=all
//
// Each benchmark appears once per backend, so the two can be read side by
// side. WaitRoundTrip is the cost of an operation that has to wait.
// SendAndReceive is real network I/O over loopback: on io_uring every
// operation goes through the queue, while on epoll a send and a receive of
// data that has already arrived are plain system calls with no waiting.
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
//   --------------------------------------------------------------------------
//   Benchmark                                Time             CPU   Iterations
//   --------------------------------------------------------------------------
//   BM_WaitRoundTrip/io_uring              853 ns          853 ns       483638
//   BM_WaitRoundTrip/epoll                 154 ns          154 ns      2694870
//   BM_SendAndReceive/io_uring/64         3960 ns         3960 ns       107588 bytes_per_second=15.4145Mi/s
//   BM_SendAndReceive/io_uring/4096       4288 ns         4288 ns        98057 bytes_per_second=911.065Mi/s
//   BM_SendAndReceive/epoll/64            3455 ns         3454 ns       124112 bytes_per_second=17.6694Mi/s
//   BM_SendAndReceive/epoll/4096          3786 ns         3785 ns       110398 bytes_per_second=1.00772Gi/s
// End of results.
// clang-format on

namespace {

// The two ends of a connection over loopback.
struct Pair {
  os::Socket client;
  os::Socket server;
};

Task<Pair> Connect() {
  os::Socket listener = *os::Socket::CreateTcp();
  listener.Bind(*os::SocketAddress::Parse("127.0.0.1", 0)).IgnoreError();
  listener.Listen().IgnoreError();
  os::Socket client = *os::Socket::CreateTcp();
  (co_await os::Connect(client, *listener.LocalAddress())).IgnoreError();
  co_return Pair{
      std::move(client),
      *co_await os::Accept(listener),
  };
}

// Runs `body`, a task holding the measured loop, on a driver of the given
// backend until it sets its `done` flag.
template <typename Body>
void RunOnBackend(os::IoBackend backend, benchmark::State& state, Body body) {
  os::IoDriver driver = NewDriver(backend);
  driver.Attach();
  {
    TaskScope scope;
    bool done = false;  // NOLINT(misc-const-correctness): set by `body`.
    scope.Spawn(body(state, done));
    RunUntil(driver, done);
  }
  driver.Detach();
}

Task<> SleepInLoop(benchmark::State& state, bool& done) {
  for (auto _ : state) co_await os::Sleep(os::Duration::zero());
  done = true;
}

// The cheapest operation that has to wait, a timer that is already due: hand
// it over, go to the kernel, come back, wake the task.
void BM_WaitRoundTrip(benchmark::State& state, os::IoBackend backend) {
  RunOnBackend(backend, state, SleepInLoop);
}
BENCHMARK_CAPTURE(BM_WaitRoundTrip, io_uring, os::IoBackend::kIoUring);
BENCHMARK_CAPTURE(BM_WaitRoundTrip, epoll, os::IoBackend::kEpoll);

Task<> SendAndReceiveInLoop(benchmark::State& state, bool& done) {
  const Pair pair = co_await Connect();
  const size_t size = static_cast<size_t>(state.range(0));
  const std::string message(size, 'x');
  std::string buffer(size, '\0');
  for (auto _ : state) {
    (co_await os::Send(pair.client, message, "")).IgnoreError();
    // The message may arrive in more than one piece.
    for (size_t received = 0; received < size;) {
      received += *co_await os::Receive(pair.server, buffer);
    }
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
  done = true;
}

// A message sent down a loopback connection and received at the other end.
void BM_SendAndReceive(benchmark::State& state, os::IoBackend backend) {
  RunOnBackend(backend, state, SendAndReceiveInLoop);
}
BENCHMARK_CAPTURE(BM_SendAndReceive, io_uring, os::IoBackend::kIoUring)
    ->Arg(64)
    ->Arg(4096);
BENCHMARK_CAPTURE(BM_SendAndReceive, epoll, os::IoBackend::kEpoll)
    ->Arg(64)
    ->Arg(4096);

}  // namespace
