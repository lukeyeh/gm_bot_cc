// HTTP heads by example: how they are written and read.

#include "http/head.h"

#include <benchmark/benchmark.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "net/event_loop.h"
#include "net/reader.h"
#include "net/stream.h"

namespace {

using absl_testing::StatusIs;

void RunOnEventLoop(Task<> test) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(std::move(test));
}

// Reads a head from a peer that sent `wire` and then closed.
Task<absl::StatusOr<http::Head>> ReadHeadFrom(std::string_view wire) {
  const net::Deadline soon = net::After(std::chrono::seconds(5));
  const absl::StatusOr<net::Listener> listener = net::Listener::OnLoopback();
  ABSL_EXPECT_OK(listener);
  absl::StatusOr<std::unique_ptr<net::Stream>> sender =
      co_await net::Dial(listener->address(), soon);
  ABSL_EXPECT_OK(sender);
  const absl::StatusOr<std::unique_ptr<net::Stream>> receiver =
      co_await listener->Accept();
  ABSL_EXPECT_OK(receiver);
  ABSL_EXPECT_OK(co_await (*sender)->Write(wire));
  sender->reset();

  net::Reader reader(receiver->get());
  co_return co_await http::ReadHead(reader, soon);
}

// A head is a start line, one line per header, and a blank line.
TEST(FormatHeadTest, WritesStartLineHeadersAndBlankLine) {
  const http::Head head{
      .start_line = "GET /gateway HTTP/1.1",
      .headers =
          {
              http::Header{
                  .name = "Host",
                  .value = "discord.com",
              },
              http::Header{
                  .name = "Accept",
                  .value = "*/*",
              },
          },
  };

  EXPECT_EQ(http::FormatHead(head),
            "GET /gateway HTTP/1.1\r\n"
            "Host: discord.com\r\n"
            "Accept: */*\r\n"
            "\r\n");
}

// Header names are case-insensitive; servers do vary the spelling.
TEST(FindHeaderTest, IgnoresCase) {
  const std::vector<http::Header> headers = {
      http::Header{
          .name = "content-length",
          .value = "12",
      },
  };

  EXPECT_EQ(http::FindHeader(headers, "Content-Length"), "12");
  EXPECT_EQ(http::FindHeader(headers, "Retry-After"), "");
}

// Reading stops at the blank line, so whatever follows is the body.
TEST(ReadHeadTest, ReadsUpToTheBlankLine) {
  RunOnEventLoop([]() -> Task<> {
    const absl::StatusOr<http::Head> head = co_await ReadHeadFrom(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type:application/json\r\n"
        "X-Padded:   spaced out  \r\n"
        "\r\n"
        "body");
    ABSL_EXPECT_OK(head);
    if (!head.ok()) co_return;

    EXPECT_EQ(head->start_line, "HTTP/1.1 200 OK");
    EXPECT_EQ(head->headers.size(), 2);
    EXPECT_EQ(http::FindHeader(head->headers, "Content-Type"),
              "application/json");
    // Padding around a value is not part of it.
    EXPECT_EQ(http::FindHeader(head->headers, "X-Padded"), "spaced out");
  }());
}

TEST(ReadHeadTest, RejectsALineThatIsNotAHeader) {
  RunOnEventLoop([]() -> Task<> {
    EXPECT_THAT(co_await ReadHeadFrom("HTTP/1.1 200 OK\r\nnonsense\r\n\r\n"),
                StatusIs(absl::StatusCode::kInvalidArgument));
  }());
}

// A peer that hangs up mid-head has not sent a message.
TEST(ReadHeadTest, FailsIfThePeerClosesEarly) {
  RunOnEventLoop([]() -> Task<> {
    EXPECT_THAT(co_await ReadHeadFrom("HTTP/1.1 200 OK\r\nHost: a\r\n"),
                StatusIs(absl::StatusCode::kUnavailable));
  }());
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //http:head_test -- --benchmark_filter=all
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
//   --------------------------------------------------------
//   Benchmark              Time             CPU   Iterations
//   --------------------------------------------------------
//   BM_FormatHead        136 ns          136 ns      3247903
//   BM_ReadHead          631 ns          631 ns       621036
// End of results.
// clang-format on

void BM_FormatHead(benchmark::State& state) {
  const http::Head head{
      .start_line =
          "POST /api/v10/channels/2000000000000000002/messages "
          "HTTP/1.1",
      .headers =
          {
              http::Header{
                  .name = "Authorization",
                  .value = "Bot xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
              },
              http::Header{
                  .name = "User-Agent",
                  .value =
                      "DiscordBot (https://github.com/lukeyeh/gm_bot_cc, 0.1)",
              },
              http::Header{
                  .name = "Content-Type",
                  .value = "application/json",
              },
              http::Header{
                  .name = "Host",
                  .value = "discord.com",
              },
              http::Header{
                  .name = "Content-Length",
                  .value = "83",
              },
          },
  };
  for (auto _ : state) benchmark::DoNotOptimize(http::FormatHead(head));
}
BENCHMARK(BM_FormatHead);

Task<> ReadHeads(benchmark::State& state) {
  // A peer that sends the same response head for ever.
  const absl::StatusOr<net::Listener> listener = net::Listener::OnLoopback();
  const net::Deadline never = net::After(std::chrono::hours(1));
  Spawn([](const net::Listener& listener) -> Task<> {
    const absl::StatusOr<std::unique_ptr<net::Stream>> stream =
        co_await listener.Accept();
    std::string heads;
    for (int i = 0; i < 64; ++i) {
      heads +=
          "HTTP/1.1 200 OK\r\nDate: Mon, 05 Oct 2026 12:00:00 GMT\r\n"
          "Content-Type: application/json\r\nContent-Length: 0\r\n"
          "X-RateLimit-Remaining: 4\r\nX-RateLimit-Reset-After: 1.0\r\n"
          "Via: 1.1 google\r\nServer: cloudflare\r\n\r\n";
    }
    while ((co_await (*stream)->Write(heads)).ok()) {
    }
  }(*listener));

  const absl::StatusOr<std::unique_ptr<net::Stream>> stream =
      co_await net::Dial(listener->address(), never);
  net::Reader reader(stream->get());
  for (auto _ : state) {
    benchmark::DoNotOptimize(co_await http::ReadHead(reader, never));
  }
}

// Reading a response head of the size Discord sends, from bytes that have
// already arrived.
void BM_ReadHead(benchmark::State& state) {
  EventLoop::Create()->Run(ReadHeads(state));
}
BENCHMARK(BM_ReadHead);

}  // namespace
