// net::Dial and net::Listener by example: opening a stream, and how a test
// plays the server.

#include "net/stream.h"

#include <benchmark/benchmark.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "net/event_loop.h"

namespace {

using absl_testing::IsOkAndHolds;
using absl_testing::StatusIs;

void RunOnEventLoop(Task<> test) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(std::move(test));
}

net::Deadline Soon() { return net::After(std::chrono::seconds(5)); }

// A dialled stream and the one the listener accepts are the two ends of one
// connection.
struct Pair {
  std::unique_ptr<net::Stream> client;
  std::unique_ptr<net::Stream> server;
};

Task<Pair> Connect() {
  const absl::StatusOr<net::Listener> listener = net::Listener::OnLoopback();
  ABSL_EXPECT_OK(listener);
  absl::StatusOr<std::unique_ptr<net::Stream>> client =
      co_await net::Dial(listener->address(), Soon());
  ABSL_EXPECT_OK(client);
  absl::StatusOr<std::unique_ptr<net::Stream>> server =
      co_await listener->Accept();
  ABSL_EXPECT_OK(server);
  co_return Pair{
      std::move(*client),
      std::move(*server),
  };
}

// The basic contract: what one end writes, the other reads.
TEST(StreamTest, CarriesBytesBothWays) {
  RunOnEventLoop([]() -> Task<> {
    const Pair pair = co_await Connect();
    std::array<char, 16> buffer{};

    ABSL_EXPECT_OK(co_await pair.client->Write("ping"));
    EXPECT_THAT(co_await pair.server->Read(buffer, Soon()), IsOkAndHolds(4));
    EXPECT_EQ(std::string(buffer.data(), 4), "ping");

    ABSL_EXPECT_OK(co_await pair.server->Write("pong"));
    EXPECT_THAT(co_await pair.client->Read(buffer, Soon()), IsOkAndHolds(4));
    EXPECT_EQ(std::string(buffer.data(), 4), "pong");
  }());
}

// Write sends everything, however much that is; the reader gets it in
// whatever pieces the network delivers.
TEST(StreamTest, WritesEverythingHoweverLarge) {
  RunOnEventLoop([]() -> Task<> {
    const Pair pair = co_await Connect();
    const std::string big(1'000'000, 'x');
    Spawn([](net::Stream& client, const std::string& big) -> Task<> {
      ABSL_EXPECT_OK(co_await client.Write(big));
    }(*pair.client, big));

    std::array<char, size_t{64} * 1024> buffer{};
    size_t total = 0;
    while (total < big.size()) {
      const absl::StatusOr<size_t> count =
          co_await pair.server->Read(buffer, Soon());
      ABSL_EXPECT_OK(count);
      if (!count.ok()) co_return;
      if (*count == 0) {
        ADD_FAILURE() << "the stream ended early";
        co_return;
      }
      total += *count;
    }
    EXPECT_EQ(total, big.size());
  }());
}

// Destroying a stream closes it, which the peer sees as the end of the
// stream rather than as a failure.
TEST(StreamTest, DestroyingAStreamEndsItForThePeer) {
  RunOnEventLoop([]() -> Task<> {
    Pair pair = co_await Connect();
    pair.client.reset();

    std::array<char, 16> buffer{};
    EXPECT_THAT(co_await pair.server->Read(buffer, Soon()), IsOkAndHolds(0));
  }());
}

// A quiet peer costs the caller no more than the deadline, and the stream is
// still good afterwards.
TEST(StreamTest, ReadGivesUpAtTheDeadlineAndStaysUsable) {
  RunOnEventLoop([]() -> Task<> {
    const Pair pair = co_await Connect();
    std::array<char, 16> buffer{};

    EXPECT_THAT(co_await pair.client->Read(
                    buffer, net::After(std::chrono::milliseconds(10))),
                StatusIs(absl::StatusCode::kDeadlineExceeded));

    ABSL_EXPECT_OK(co_await pair.server->Write("late"));
    EXPECT_THAT(co_await pair.client->Read(buffer, Soon()), IsOkAndHolds(4));
  }());
}

// An unreachable server is Unavailable: worth trying again later.
TEST(DialTest, FailsWhenNothingListens) {
  RunOnEventLoop([]() -> Task<> {
    net::Address address;
    {
      const absl::StatusOr<net::Listener> listener =
          net::Listener::OnLoopback();
      ABSL_EXPECT_OK(listener);
      if (!listener.ok()) co_return;
      address = listener->address();
    }

    EXPECT_THAT(co_await net::Dial(address, Soon()),
                StatusIs(absl::StatusCode::kUnavailable));
  }());
}

// Hosts are given by name, as in a URL.
TEST(DialTest, ResolvesHostNames) {
  RunOnEventLoop([]() -> Task<> {
    const absl::StatusOr<net::Listener> listener = net::Listener::OnLoopback();
    ABSL_EXPECT_OK(listener);
    if (!listener.ok()) co_return;
    net::Address address = listener->address();
    address.host = "localhost";

    ABSL_EXPECT_OK(co_await net::Dial(address, Soon()));
  }());
}

// After is the usual way to say "give this so long".
TEST(AfterTest, IsThatFarInTheFuture) {
  const net::Deadline before = std::chrono::steady_clock::now();
  const net::Deadline deadline = net::After(std::chrono::seconds(10));
  EXPECT_GE(deadline - before, std::chrono::seconds(10));
  EXPECT_LT(deadline - before, std::chrono::seconds(11));
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //net:stream_test -- --benchmark_filter=all
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
//   ---------------------------------------------------------------
//   Benchmark                     Time             CPU   Iterations
//   ---------------------------------------------------------------
//   BM_WriteThenRead/64        5231 ns         5231 ns        78244
//   BM_WriteThenRead/256       5289 ns         5288 ns        79672
// End of results.
// clang-format on

Task<> PingPong(benchmark::State& state) {
  const Pair pair = co_await Connect();
  std::array<char, 256> buffer{};
  const std::string message(static_cast<size_t>(state.range(0)), 'x');
  for (auto _ : state) {
    (co_await pair.client->Write(message)).IgnoreError();
    benchmark::DoNotOptimize(co_await pair.server->Read(buffer, Soon()));
  }
}

// One small write and the read that receives it, over loopback: what the
// kernel and the event loop cost per message before any protocol is added.
void BM_WriteThenRead(benchmark::State& state) {
  EventLoop::Create()->Run(PingPong(state));
}
BENCHMARK(BM_WriteThenRead)->Arg(64)->Arg(256);

}  // namespace
