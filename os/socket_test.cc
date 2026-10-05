// Socket and SocketAddress by example: setting up a socket to listen on,
// with every failure reported as a Status.

#include "os/socket.h"

#include <benchmark/benchmark.h>

#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using absl_testing::StatusIs;

// Port 0 means "any free port", which keeps tests from clashing.
os::SocketAddress AnyLocalPort() {
  return *os::SocketAddress::Parse("127.0.0.1", 0);
}

os::Socket NewSocket() {
  absl::StatusOr<os::Socket> socket = os::Socket::CreateTcp();
  ABSL_EXPECT_OK(socket);
  return std::move(*socket);
}

TEST(SocketAddressTest, ParsesDottedAddress) {
  const absl::StatusOr<os::SocketAddress> address =
      os::SocketAddress::Parse("192.168.1.20", 8080);
  ABSL_ASSERT_OK(address);
  EXPECT_EQ(address->port(), 8080);
  EXPECT_EQ(address->ToString(), "192.168.1.20:8080");
}

// Only numeric IPv4 addresses are understood; names are not looked up.
TEST(SocketAddressTest, RejectsAnythingElse) {
  EXPECT_THAT(os::SocketAddress::Parse("localhost", 80),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(os::SocketAddress::Parse("1.2.3", 80),
              StatusIs(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(os::SocketAddress::Parse("::1", 80),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// Resolve is Parse for clients, which are given names rather than addresses.
TEST(SocketAddressTest, ResolvesNamesAndDottedAddresses) {
  const absl::StatusOr<os::SocketAddress> by_name =
      os::SocketAddress::Resolve("localhost", 443);
  ABSL_ASSERT_OK(by_name);
  EXPECT_EQ(by_name->ToString(), "127.0.0.1:443");

  const absl::StatusOr<os::SocketAddress> by_address =
      os::SocketAddress::Resolve("192.168.1.20", 8080);
  ABSL_ASSERT_OK(by_address);
  EXPECT_EQ(by_address->ToString(), "192.168.1.20:8080");
}

// The steps a server takes before it can accept anyone.
TEST(SocketTest, BindsAndListens) {
  os::Socket socket = NewSocket();
  ABSL_EXPECT_OK(socket.Enable(os::SocketOption::kReuseAddress));
  ABSL_EXPECT_OK(socket.Bind(AnyLocalPort()));
  ABSL_EXPECT_OK(socket.Listen());
}

// After binding to port 0, LocalAddress says which port the system picked.
TEST(SocketTest, LocalAddressReportsChosenPort) {
  os::Socket socket = NewSocket();
  ABSL_ASSERT_OK(socket.Bind(AnyLocalPort()));
  const absl::StatusOr<os::SocketAddress> local = socket.LocalAddress();
  ABSL_ASSERT_OK(local);
  EXPECT_NE(local->port(), 0);
}

// The kernel's "address already in use" arrives as FailedPrecondition, with
// the kernel's own wording in the message.
TEST(SocketTest, BindFailsWhenAddressIsTaken) {
  os::Socket first = NewSocket();
  ABSL_ASSERT_OK(first.Bind(AnyLocalPort()));
  ABSL_ASSERT_OK(first.Listen());

  os::Socket second = NewSocket();
  EXPECT_THAT(second.Bind(*first.LocalAddress()),
              StatusIs(absl::StatusCode::kFailedPrecondition,
                       testing::HasSubstr("Address already in use")));
}

// Options can be set on any socket, listening or not.
TEST(SocketTest, EnablesOptions) {
  os::Socket socket = NewSocket();
  ABSL_EXPECT_OK(socket.Enable(os::SocketOption::kNoDelay));
  ABSL_EXPECT_OK(socket.Enable(os::SocketOption::kReuseAddress));
}

// A moved-from socket owns nothing; the destination closes the socket.
TEST(SocketTest, MoveTransfersOwnership) {
  os::Socket socket = NewSocket();
  os::Socket moved = std::move(socket);
  ABSL_EXPECT_OK(moved.Bind(AnyLocalPort()));
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
// What the socket wrapper costs.
//
//   bazel run -c opt //os:socket_test -- --benchmark_filter=all
//
// Addresses are pure computation. Creating and binding sockets are trips into
// the kernel, and set the floor for what a new connection can cost.
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
//   BM_ParseAddress                28.9 ns         28.9 ns     14518413
//   BM_AddressToString             61.4 ns         61.3 ns      6442201
//   BM_CreateAndCloseSocket        2193 ns         2187 ns       193667
//   BM_CreateBindListenClose       3907 ns         3888 ns       110276
// End of results.
// clang-format on

namespace {

// Turning "192.168.1.20" into an address.
void BM_ParseAddress(benchmark::State& state) {
  const std::string ip = "192.168.1.20";
  for (auto _ : state) {
    benchmark::DoNotOptimize(os::SocketAddress::Parse(ip, 8080));
  }
}
BENCHMARK(BM_ParseAddress);

// Turning an address back into text, as error messages do.
void BM_AddressToString(benchmark::State& state) {
  const os::SocketAddress address =
      *os::SocketAddress::Parse("192.168.1.20", 8080);
  for (auto _ : state) benchmark::DoNotOptimize(address.ToString());
}
BENCHMARK(BM_AddressToString);

// Asking the kernel for a socket and giving it back: two system calls.
void BM_CreateAndCloseSocket(benchmark::State& state) {
  for (auto _ : state) benchmark::DoNotOptimize(os::Socket::CreateTcp());
}
BENCHMARK(BM_CreateAndCloseSocket);

// Everything a server does before it can accept: create, bind to a free port,
// listen, and (here) close again.
void BM_CreateBindListenClose(benchmark::State& state) {
  const os::SocketAddress any_port = *os::SocketAddress::Parse("127.0.0.1", 0);
  for (auto _ : state) {
    os::Socket socket = *os::Socket::CreateTcp();
    benchmark::DoNotOptimize(socket.Bind(any_port));
    benchmark::DoNotOptimize(socket.Listen());
  }
}
BENCHMARK(BM_CreateBindListenClose);

}  // namespace
