// Shows what net::ParseUrl makes of the URLs clients are given.

#include "net/url.h"

#include <benchmark/benchmark.h>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "net/stream.h"

namespace net {
namespace {

using ::absl_testing::StatusIs;

// The scheme decides both the security and the default port.
TEST(ParseUrlTest, SecureSchemesUseTlsOnPort443) {
  for (const char* const text : {
           "https://discord.com/api/v10/gateway",
           "wss://discord.com/api/v10/gateway",
       }) {
    const absl::StatusOr<Url> url = ParseUrl(text);
    ABSL_ASSERT_OK(url);
    EXPECT_EQ(url->address.host, "discord.com");
    EXPECT_EQ(url->address.port, 443);
    EXPECT_EQ(url->address.security, Security::kTls);
    EXPECT_EQ(url->authority, "discord.com");
    EXPECT_EQ(url->target, "/api/v10/gateway");
  }
}

TEST(ParseUrlTest, PlainSchemesUsePlaintextOnPort80) {
  for (const char* const text : {
           "http://example.com/",
           "ws://example.com/",
       }) {
    const absl::StatusOr<Url> url = ParseUrl(text);
    ABSL_ASSERT_OK(url);
    EXPECT_EQ(url->address.port, 80);
    EXPECT_EQ(url->address.security, Security::kPlaintext);
  }
}

// An explicit port is dialled, and is part of the Host header value.
TEST(ParseUrlTest, KeepsAnExplicitPort) {
  const absl::StatusOr<Url> url = ParseUrl("http://127.0.0.1:8080/x");
  ABSL_ASSERT_OK(url);
  EXPECT_EQ(url->address.host, "127.0.0.1");
  EXPECT_EQ(url->address.port, 8080);
  EXPECT_EQ(url->authority, "127.0.0.1:8080");
}

// The target is always something that can follow "GET " in a request.
TEST(ParseUrlTest, TargetIsNeverEmpty) {
  const absl::StatusOr<Url> bare = ParseUrl("wss://gateway.discord.gg");
  ABSL_ASSERT_OK(bare);
  EXPECT_EQ(bare->target, "/");

  const absl::StatusOr<Url> query_only =
      ParseUrl("wss://gateway.discord.gg?v=10");
  ABSL_ASSERT_OK(query_only);
  EXPECT_EQ(query_only->target, "/?v=10");
}

// The query is sent to the server; the fragment never is.
TEST(ParseUrlTest, KeepsTheQueryAndDropsTheFragment) {
  const absl::StatusOr<Url> url = ParseUrl("https://a.example/p?q=1#frag");
  ABSL_ASSERT_OK(url);
  EXPECT_EQ(url->target, "/p?q=1");
}

TEST(ParseUrlTest, RejectsWhatItCannotDial) {
  for (const char* const text : {
           "discord.com/api",         // No scheme.
           "ftp://example.com/file",  // Not a scheme a client here speaks.
           "https:///path",           // No host.
           "https://example.com:0/",  // Not a port.
           "https://example.com:http/",
       }) {
    EXPECT_THAT(ParseUrl(text), StatusIs(absl::StatusCode::kInvalidArgument))
        << text;
  }
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //net:url_test -- --benchmark_filter=all
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
//   ------------------------------------------------------
//   Benchmark            Time             CPU   Iterations
//   ------------------------------------------------------
//   BM_ParseUrl       54.1 ns         53.0 ns      7855853
// End of results.
// clang-format on

void BM_ParseUrl(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        ParseUrl("https://discord.com/api/v10/channels/2000000000000000002/"
                 "messages"));
  }
}
BENCHMARK(BM_ParseUrl);

}  // namespace
}  // namespace net
