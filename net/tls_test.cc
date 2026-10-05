// TLS layered over a stream, by example: bytes pass through once the client
// is satisfied of the server's identity, and not otherwise.

#include "net/tls.h"

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
#include "net/stream.h"

namespace {

using absl_testing::IsOkAndHolds;
using absl_testing::StatusIs;

// A self-signed certificate for "localhost" and its key, made with
//   openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes
//     -days 36500 -subj /CN=localhost -addext subjectAltName=DNS:localhost
constexpr char kLocalhostCertificate[] = R"(-----BEGIN CERTIFICATE-----
MIIBlTCCATugAwIBAgIUHf3QRo7Itch/rkOjGi0h0qZTyREwCgYIKoZIzj0EAwIw
FDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTI2MTAwNjAwMDAyNloYDzIxMjYwOTEy
MDAwMDI2WjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjO
PQMBBwNCAATv1/leL63KOVRrXcV1mNT8LnJp9olvz9++cTnmr7AgdPpYMym1VPgy
FcSaEiRZ+wG99tEpbgG9zP6mTdGYqsOpo2kwZzAdBgNVHQ4EFgQUdCyRbOC1Wui0
MxrzUBST/1Uk/bEwHwYDVR0jBBgwFoAUdCyRbOC1Wui0MxrzUBST/1Uk/bEwDwYD
VR0TAQH/BAUwAwEB/zAUBgNVHREEDTALgglsb2NhbGhvc3QwCgYIKoZIzj0EAwID
SAAwRQIhAOX2FpTbFkCDAbO5mhwsvaZN5NNuur3fAz6Nc9KVD2xkAiAAny9EDln8
EmVm3QQim9RzB2++VndITDzMjXDorznd8w==
-----END CERTIFICATE-----
)";

constexpr char kLocalhostKey[] = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgZjuhaLHJaiZVZE8K
fLjthFxsuP4o9X6EYgkEyrHnyBKhRANCAATv1/leL63KOVRrXcV1mNT8LnJp9olv
z9++cTnmr7AgdPpYMym1VPgyFcSaEiRZ+wG99tEpbgG9zP6mTdGYqsOp
-----END PRIVATE KEY-----
)";

// A different self-signed certificate, which did not sign the one above.
constexpr char kUnrelatedCertificate[] = R"(-----BEGIN CERTIFICATE-----
MIIBfTCCASOgAwIBAgIUDAY/bvop1FDHCawYkon9GGPzVrswCgYIKoZIzj0EAwIw
EzERMA8GA1UEAwwIc3RyYW5nZXIwIBcNMjYxMDA2MDAwMDI2WhgPMjEyNjA5MTIw
MDAwMjZaMBMxETAPBgNVBAMMCHN0cmFuZ2VyMFkwEwYHKoZIzj0CAQYIKoZIzj0D
AQcDQgAEAX2MykeOlWQANvfZ1tKTIgMkx6jb3GIqREwmYwGPr8XB/so8S+DeoP1r
bhkrsdQtcpbUYDBUCbrRYr3ifeq9z6NTMFEwHQYDVR0OBBYEFExqRxDbkqhJtO3d
3LnphCpbudW0MB8GA1UdIwQYMBaAFExqRxDbkqhJtO3d3LnphCpbudW0MA8GA1Ud
EwEB/wQFMAMBAf8wCgYIKoZIzj0EAwIDSAAwRQIgHZ9kk9SQ+e87PdguMzRCzawa
LmluREV+OTCWQcoemwkCIQD9okH96bWGY1YHOHXHI0wIEeri26GpyLWz1AQHSTw3
Kg==
-----END CERTIFICATE-----
)";

void RunOnEventLoop(Task<> test) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(std::move(test));
}

net::Deadline Soon() { return net::After(std::chrono::seconds(5)); }

// A TLS server for "localhost" serving one connection: it reads four bytes
// and answers "pong".
Task<> Serve(const net::Listener& listener) {
  absl::StatusOr<std::unique_ptr<net::Stream>> accepted =
      co_await listener.Accept();
  ABSL_EXPECT_OK(accepted);
  const absl::StatusOr<std::unique_ptr<net::Stream>> secure =
      co_await net::TlsAccept(std::move(*accepted),
                              net::TlsIdentity{
                                  .certificate_pem = kLocalhostCertificate,
                                  .private_key_pem = kLocalhostKey,
                              },
                              Soon());
  // The client walks away mid-handshake in the tests where it rejects us.
  if (!secure.ok()) co_return;
  std::array<char, 4> request{};
  if ((co_await (*secure)->Read(request, Soon())).ok()) {
    (co_await (*secure)->Write("pong")).IgnoreError();
  }
}

// Starts the server above and connects to it as a client that expects
// `hostname` and trusts `roots_pem`.
Task<absl::StatusOr<std::unique_ptr<net::Stream>>> ConnectToServer(
    const net::Listener& listener, std::string hostname,
    std::string roots_pem) {
  Spawn(Serve(listener));
  absl::StatusOr<std::unique_ptr<net::Stream>> plain =
      co_await net::Dial(listener.address(), Soon());
  ABSL_EXPECT_OK(plain);
  co_return co_await net::TlsConnect(std::move(*plain), std::move(hostname),
                                     Soon(), std::move(roots_pem));
}

net::Listener NewListener() {
  absl::StatusOr<net::Listener> listener = net::Listener::OnLoopback();
  ABSL_EXPECT_OK(listener);
  return std::move(*listener);
}

// The normal case: the server proves it is who the client asked for, and the
// two can talk.
TEST(TlsTest, CarriesBytesOnceTheServerIsVerified) {
  RunOnEventLoop([]() -> Task<> {
    const net::Listener listener = NewListener();
    const absl::StatusOr<std::unique_ptr<net::Stream>> stream =
        co_await ConnectToServer(listener, "localhost", kLocalhostCertificate);
    ABSL_EXPECT_OK(stream);
    if (!stream.ok()) co_return;

    ABSL_EXPECT_OK(co_await (*stream)->Write("ping"));
    std::array<char, 16> buffer{};
    EXPECT_THAT(co_await (*stream)->Read(buffer, Soon()), IsOkAndHolds(4));
    EXPECT_EQ(std::string(buffer.data(), 4), "pong");
  }());
}

// A certificate that does not chain to a trusted root proves nothing.
TEST(TlsTest, RejectsAServerNoTrustedRootVouchesFor) {
  RunOnEventLoop([]() -> Task<> {
    const net::Listener listener = NewListener();
    EXPECT_THAT(
        co_await ConnectToServer(listener, "localhost", kUnrelatedCertificate),
        StatusIs(absl::StatusCode::kUnauthenticated));
  }());
}

// Nor does a trusted certificate that is for some other host.
TEST(TlsTest, RejectsAServerWithACertificateForAnotherHost) {
  RunOnEventLoop([]() -> Task<> {
    const net::Listener listener = NewListener();
    EXPECT_THAT(co_await ConnectToServer(listener, "discord.com",
                                         kLocalhostCertificate),
                StatusIs(absl::StatusCode::kUnauthenticated));
  }());
}

// A handshake with a peer that never answers ends at the deadline.
TEST(TlsTest, HandshakeGivesUpAtTheDeadline) {
  RunOnEventLoop([]() -> Task<> {
    const net::Listener listener = NewListener();
    absl::StatusOr<std::unique_ptr<net::Stream>> silent =
        co_await net::Dial(listener.address(), Soon());
    ABSL_EXPECT_OK(silent);
    if (!silent.ok()) co_return;

    EXPECT_THAT(
        co_await net::TlsConnect(std::move(*silent), "localhost",
                                 net::After(std::chrono::milliseconds(20)),
                                 kLocalhostCertificate),
        StatusIs(absl::StatusCode::kDeadlineExceeded));
  }());
}

// Trusted roots that are not certificates are the caller's mistake.
TEST(TlsTest, RejectsUnusableTrustedRoots) {
  RunOnEventLoop([]() -> Task<> {
    const net::Listener listener = NewListener();
    EXPECT_THAT(co_await ConnectToServer(listener, "localhost", "not a PEM"),
                StatusIs(absl::StatusCode::kInvalidArgument));
  }());
}

// More than one TLS record's worth, both to check that records are
// reassembled and that a large write is not held up by a small buffer.
TEST(TlsTest, CarriesMoreThanOneRecord) {
  RunOnEventLoop([]() -> Task<> {
    const net::Listener listener = NewListener();
    // A server that sends 100 kB and closes.
    Spawn([](const net::Listener& listener) -> Task<> {
      absl::StatusOr<std::unique_ptr<net::Stream>> accepted =
          co_await listener.Accept();
      ABSL_EXPECT_OK(accepted);
      const absl::StatusOr<std::unique_ptr<net::Stream>> secure =
          co_await net::TlsAccept(std::move(*accepted),
                                  net::TlsIdentity{
                                      .certificate_pem = kLocalhostCertificate,
                                      .private_key_pem = kLocalhostKey,
                                  },
                                  Soon());
      ABSL_EXPECT_OK(secure);
      const std::string big(100'000, 'x');
      ABSL_EXPECT_OK(co_await (*secure)->Write(big));
    }(listener));

    absl::StatusOr<std::unique_ptr<net::Stream>> plain =
        co_await net::Dial(listener.address(), Soon());
    ABSL_EXPECT_OK(plain);
    if (!plain.ok()) co_return;
    const absl::StatusOr<std::unique_ptr<net::Stream>> stream =
        co_await net::TlsConnect(std::move(*plain), "localhost", Soon(),
                                 kLocalhostCertificate);
    ABSL_EXPECT_OK(stream);
    if (!stream.ok()) co_return;

    std::array<char, 8192> buffer{};
    size_t total = 0;
    while (total < 100'000) {
      const absl::StatusOr<size_t> count =
          co_await (*stream)->Read(buffer, Soon());
      ABSL_EXPECT_OK(count);
      if (!count.ok()) co_return;
      if (*count == 0) {
        ADD_FAILURE() << "the stream ended early";
        co_return;
      }
      total += *count;
    }
    EXPECT_EQ(total, 100'000);
  }());
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //net:tls_test -- --benchmark_filter=all
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
//   BM_TlsHandshake       1249654 ns      1207431 ns          348
//   BM_TlsMessage/256        8931 ns         8930 ns        46393 bytes_per_second=27.3398Mi/s
//   BM_TlsMessage/4096      11337 ns        11337 ns        38266 bytes_per_second=344.558Mi/s
// End of results.
// clang-format on

// Both ends of a TLS connection over loopback.
struct SecurePair {
  std::unique_ptr<net::Stream> client;
  std::unique_ptr<net::Stream> server;
};

Task<SecurePair> ConnectSecurely(const net::Listener& listener) {
  SecurePair pair;
  Spawn([](const net::Listener& listener,
           std::unique_ptr<net::Stream>& server) -> Task<> {
    absl::StatusOr<std::unique_ptr<net::Stream>> accepted =
        co_await listener.Accept();
    absl::StatusOr<std::unique_ptr<net::Stream>> secure =
        co_await net::TlsAccept(std::move(*accepted),
                                net::TlsIdentity{
                                    .certificate_pem = kLocalhostCertificate,
                                    .private_key_pem = kLocalhostKey,
                                },
                                Soon());
    server = std::move(*secure);
  }(listener, pair.server));

  absl::StatusOr<std::unique_ptr<net::Stream>> plain =
      co_await net::Dial(listener.address(), Soon());
  absl::StatusOr<std::unique_ptr<net::Stream>> client =
      co_await net::TlsConnect(std::move(*plain), "localhost", Soon(),
                               kLocalhostCertificate);
  pair.client = std::move(*client);

  // The server's side finishes a moment after the client's.
  while (pair.server == nullptr) co_await Sleep(std::chrono::microseconds(50));
  co_return pair;
}

Task<> Handshakes(benchmark::State& state) {
  const net::Listener listener = NewListener();
  for (auto _ : state) {
    benchmark::DoNotOptimize(co_await ConnectSecurely(listener));
  }
}

// Connecting and shaking hands: what every new connection to Discord costs
// before the network's own delay.
void BM_TlsHandshake(benchmark::State& state) {
  EventLoop::Create()->Run(Handshakes(state));
}
BENCHMARK(BM_TlsHandshake);

Task<> SecureMessages(benchmark::State& state) {
  const net::Listener listener = NewListener();
  const SecurePair pair = co_await ConnectSecurely(listener);
  std::array<char, 8192> buffer{};
  const std::string message(static_cast<size_t>(state.range(0)), 'x');
  for (auto _ : state) {
    (co_await pair.server->Write(message)).IgnoreError();
    size_t received = 0;
    while (received < message.size()) {
      const absl::StatusOr<size_t> count =
          co_await pair.client->Read(buffer, Soon());
      if (!count.ok()) co_return;
      received += *count;
    }
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

// One message through an established TLS connection: encryption, the kernel,
// decryption.
void BM_TlsMessage(benchmark::State& state) {
  EventLoop::Create()->Run(SecureMessages(state));
}
BENCHMARK(BM_TlsMessage)->Arg(256)->Arg(4096);

}  // namespace
