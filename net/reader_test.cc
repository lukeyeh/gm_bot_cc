// net::Reader by example: turning the arbitrary pieces a stream delivers into
// the lines and blocks a protocol is made of, without losing data to timeouts.

#include "net/reader.h"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "net/event_loop.h"
#include "net/stream.h"

namespace {

using absl_testing::IsOkAndHolds;
using absl_testing::StatusIs;

void RunOnEventLoop(Task<> test) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(std::move(test));
}

// A stream that delivers a scripted sequence of pieces. A missing piece is a
// read that times out; running off the end of the script is the end of the
// stream.
class ScriptedStream final : public net::Stream {
 public:
  explicit ScriptedStream(std::deque<std::optional<std::string>> pieces)
      : pieces_(std::move(pieces)) {}

  Task<absl::StatusOr<size_t>> Read(std::span<char> buffer,
                                    net::Deadline) override {
    if (pieces_.empty()) co_return 0;

    std::optional<std::string>& next = pieces_.front();
    if (!next.has_value()) {
      pieces_.pop_front();
      co_return absl::DeadlineExceededError("timed out");
    }

    // Hands over as much of the piece as fits, keeping the rest for next time.
    std::string& piece = *next;
    const size_t count = std::min(piece.size(), buffer.size());
    std::copy_n(piece.begin(), count, buffer.begin());
    piece.erase(0, count);
    if (piece.empty()) pieces_.pop_front();
    co_return count;
  }

  Task<absl::Status> Write(std::string_view) override {
    co_return absl::OkStatus();
  }

 private:
  std::deque<std::optional<std::string>> pieces_;
};

net::Deadline Soon() { return net::After(std::chrono::seconds(5)); }

// Lines come back whole and without their terminator, however the network
// split them, including through the middle of the terminator.
TEST(ReaderTest, ReadsDelimitedPiecesAcrossDeliveries) {
  RunOnEventLoop([]() -> Task<> {
    ScriptedStream stream({
        "HTTP/1.1 200",
        " OK\r",
        "\nHost: a\r\n\r\n",
    });
    net::Reader reader(&stream);

    EXPECT_THAT(co_await reader.ReadUntil("\r\n", 100, Soon()),
                IsOkAndHolds("HTTP/1.1 200 OK"));
    EXPECT_THAT(co_await reader.ReadUntil("\r\n", 100, Soon()),
                IsOkAndHolds("Host: a"));
    EXPECT_THAT(co_await reader.ReadUntil("\r\n", 100, Soon()),
                IsOkAndHolds(""));
  }());
}

// Read takes exactly what was asked for and leaves the rest for later.
TEST(ReaderTest, ReadsExactSizes) {
  RunOnEventLoop([]() -> Task<> {
    ScriptedStream stream({
        "abc",
        "defgh",
    });
    net::Reader reader(&stream);

    EXPECT_THAT(co_await reader.Read(4, Soon()), IsOkAndHolds("abcd"));
    EXPECT_THAT(co_await reader.Read(4, Soon()), IsOkAndHolds("efgh"));
  }());
}

// The key promise: a timeout consumes nothing, so the caller can simply ask
// again later.
TEST(ReaderTest, TimeoutLosesNothing) {
  RunOnEventLoop([]() -> Task<> {
    ScriptedStream stream({
        "par",
        std::nullopt,
        "tial\r\n",
    });
    net::Reader reader(&stream);

    EXPECT_THAT(co_await reader.ReadUntil("\r\n", 100, Soon()),
                StatusIs(absl::StatusCode::kDeadlineExceeded));
    EXPECT_THAT(co_await reader.ReadUntil("\r\n", 100, Soon()),
                IsOkAndHolds("partial"));
  }());
}

// Fill and Peek let a protocol inspect a header before deciding how long the
// whole message is.
TEST(ReaderTest, FillMakesBytesAvailableWithoutConsumingThem) {
  RunOnEventLoop([]() -> Task<> {
    ScriptedStream stream({
        "\x03",
        "abc",
    });
    net::Reader reader(&stream);

    ABSL_EXPECT_OK(co_await reader.Fill(1, Soon()));
    const size_t length = static_cast<unsigned char>(reader.Peek()[0]);
    EXPECT_THAT(co_await reader.Read(1 + length, Soon()), IsOkAndHolds("\x03"
                                                                       "abc"));
  }());
}

// Messages far larger than the reader's first buffer are read just the same.
TEST(ReaderTest, GrowsToFitLargeReads) {
  RunOnEventLoop([]() -> Task<> {
    const std::string big(300'000, 'x');
    ScriptedStream stream({
        big,
        "tail",
    });
    net::Reader reader(&stream);

    EXPECT_THAT(co_await reader.Read(big.size(), Soon()), IsOkAndHolds(big));
    EXPECT_THAT(co_await reader.Read(4, Soon()), IsOkAndHolds("tail"));
  }());
}

// Some protocols mark the end of a body by closing the connection.
TEST(ReaderTest, ReadsToTheEndOfTheStream) {
  RunOnEventLoop([]() -> Task<> {
    ScriptedStream stream({
        "one ",
        "two",
    });
    net::Reader reader(&stream);

    EXPECT_THAT(co_await reader.ReadToEnd(Soon()), IsOkAndHolds("one two"));
  }());
}

// A stream that ends before the request is satisfied is a broken connection.
TEST(ReaderTest, EndOfStreamMidMessageIsUnavailable) {
  RunOnEventLoop([]() -> Task<> {
    ScriptedStream stream({
        "no terminator",
    });
    net::Reader reader(&stream);

    EXPECT_THAT(co_await reader.ReadUntil("\r\n", 100, Soon()),
                StatusIs(absl::StatusCode::kUnavailable));
    EXPECT_THAT(co_await reader.Read(100, Soon()),
                StatusIs(absl::StatusCode::kUnavailable));
  }());
}

// A peer cannot make the reader buffer without bound by never sending the
// delimiter.
TEST(ReaderTest, RejectsPiecesLongerThanTheLimit) {
  RunOnEventLoop([]() -> Task<> {
    ScriptedStream stream({
        std::string(10'000, 'x'),
    });
    net::Reader reader(&stream);

    EXPECT_THAT(co_await reader.ReadUntil("\r\n", 1000, Soon()),
                StatusIs(absl::StatusCode::kResourceExhausted));
  }());
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //net:reader_test -- --benchmark_filter=all
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
//   -------------------------------------------------------
//   Benchmark             Time             CPU   Iterations
//   -------------------------------------------------------
//   BM_ReadUntil       85.3 ns         85.3 ns      4949911
// End of results.
// clang-format on

// An endless supply of one line, delivered a buffer-full at a time.
class RepeatingStream final : public net::Stream {
 public:
  Task<absl::StatusOr<size_t>> Read(std::span<char> buffer,
                                    net::Deadline) override {
    static constexpr std::string_view kLine =
        "Content-Type: application/json\r\n";
    for (char& byte : buffer) {
      byte = kLine[position_++ % kLine.size()];
    }
    co_return buffer.size();
  }

  Task<absl::Status> Write(std::string_view) override {
    co_return absl::OkStatus();
  }

 private:
  size_t position_ = 0;
};

Task<> ReadLines(benchmark::State& state) {
  RepeatingStream stream;
  net::Reader reader(&stream);
  for (auto _ : state) {
    benchmark::DoNotOptimize(co_await reader.ReadUntil("\r\n", 100, Soon()));
  }
}

// Splitting a stream into header-sized lines: the cost per line when the
// bytes are already there.
void BM_ReadUntil(benchmark::State& state) {
  EventLoop::Create()->Run(ReadLines(state));
}
BENCHMARK(BM_ReadUntil);

}  // namespace
