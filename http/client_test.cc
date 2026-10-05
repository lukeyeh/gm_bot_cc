// http::Client by example, against a real server on a loopback port: what it
// puts on the wire, and how it reads each way a server can frame a body.

#include "http/client.h"

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "async/task.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "http/head.h"
#include "net/event_loop.h"
#include "net/reader.h"
#include "net/stream.h"

namespace {

using absl_testing::StatusIs;
using testing::ElementsAre;
using testing::EndsWith;
using testing::HasSubstr;

void RunOnEventLoop(Task<> test) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(std::move(test));
}

net::Deadline Soon() { return net::After(std::chrono::seconds(5)); }

// What the server is to do with one connection: answer each request arriving
// on it with the next of `replies`, written to the wire exactly as given, and
// then close.
using Replies = std::vector<std::string>;

// An HTTP server on a loopback port, serving the connections it is given
// replies for, one after another. Every request it receives is recorded, head
// then body.
class Server {
 public:
  explicit Server(std::vector<Replies> connections) {
    absl::StatusOr<net::Listener> listener = net::Listener::OnLoopback();
    ABSL_EXPECT_OK(listener);
    listener_ = std::make_unique<net::Listener>(std::move(*listener));
    Spawn(Serve(*listener_, std::move(connections), requests_));
  }

  std::string Url(std::string_view path) const {
    return absl::StrCat("http://127.0.0.1:", listener_->address().port, path);
  }

  const std::vector<std::string>& requests() const { return requests_; }

 private:
  static Task<> Serve(const net::Listener& listener,
                      std::vector<Replies> connections,
                      std::vector<std::string>& requests) {
    for (const Replies& replies : connections) {
      const absl::StatusOr<std::unique_ptr<net::Stream>> stream =
          co_await listener.Accept();
      ABSL_EXPECT_OK(stream);
      if (!stream.ok()) co_return;
      net::Reader reader(stream->get());
      for (const std::string& reply : replies) {
        const absl::StatusOr<http::Head> head =
            co_await http::ReadHead(reader, Soon());
        ABSL_EXPECT_OK(head);
        if (!head.ok()) co_return;
        size_t length = 0;
        (void)absl::SimpleAtoi(
            http::FindHeader(head->headers, "Content-Length"), &length);
        const absl::StatusOr<std::string_view> body =
            co_await reader.Read(length, Soon());
        ABSL_EXPECT_OK(body);
        if (!body.ok()) co_return;
        requests.push_back(absl::StrCat(http::FormatHead(*head), *body));
        ABSL_EXPECT_OK(co_await (*stream)->Write(reply));
      }
    }
  }

  // On the heap so that the server task's reference survives a move.
  std::unique_ptr<net::Listener> listener_;
  std::vector<std::string> requests_;
};

// A complete response with a counted body.
std::string Ok(std::string_view body) {
  return absl::StrCat("HTTP/1.1 200 OK\r\nContent-Length: ", body.size(),
                      "\r\n\r\n", body);
}

Task<absl::StatusOr<http::Response>> Get(http::Client& client,
                                         std::string url) {
  co_return co_await client.Send(http::Request{
      .url = std::move(url),
  });
}

// The whole exchange for a simple GET.
TEST(ClientTest, SendsARequestAndReturnsTheResponse) {
  RunOnEventLoop([]() -> Task<> {
    const Server server({
        {
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 11\r\n"
            "\r\n"
            R"({"ok":true})",
        },
    });
    const std::unique_ptr<http::Client> client = http::NewClient();

    const absl::StatusOr<http::Response> response =
        co_await client->Send(http::Request{
            .url = server.Url("/gateway?v=10"),
            .headers =
                {
                    http::Header{
                        .name = "Authorization",
                        .value = "Bot token",
                    },
                },
        });

    ABSL_EXPECT_OK(response);
    if (!response.ok()) co_return;
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->Get("content-type"), "application/json");
    EXPECT_EQ(response->body, R"({"ok":true})");
    const std::string& sent = server.requests().front();
    EXPECT_THAT(sent, HasSubstr("GET /gateway?v=10 HTTP/1.1\r\n"));
    EXPECT_THAT(sent, HasSubstr("Authorization: Bot token\r\n"));
    EXPECT_THAT(sent, HasSubstr("Host: 127.0.0.1:"));
  }());
}

// A body is sent with its length. So is the absence of one, on methods that
// could have had one: some servers insist.
TEST(ClientTest, SendsABodyWithItsLength) {
  RunOnEventLoop([]() -> Task<> {
    const Server server({
        {
            "HTTP/1.1 204 No Content\r\n\r\n",
            "HTTP/1.1 204 No Content\r\n\r\n",
        },
    });
    const std::unique_ptr<http::Client> client = http::NewClient();

    ABSL_EXPECT_OK(co_await client->Send(http::Request{
        .method = http::Method::kPost,
        .url = server.Url("/messages"),
        .body = R"({"content":"gm"})",
    }));
    ABSL_EXPECT_OK(co_await client->Send(http::Request{
        .method = http::Method::kPut,
        .url = server.Url("/reactions"),
    }));

    EXPECT_THAT(server.requests(),
                ElementsAre(AllOf(HasSubstr("POST /messages HTTP/1.1\r\n"),
                                  HasSubstr("Content-Length: 16\r\n"),
                                  EndsWith(R"({"content":"gm"})")),
                            AllOf(HasSubstr("PUT /reactions HTTP/1.1\r\n"),
                                  HasSubstr("Content-Length: 0\r\n"))));
  }());
}

// An error status is still a response; what to make of it is up to the caller.
TEST(ClientTest, ReturnsErrorStatusesAsResponses) {
  RunOnEventLoop([]() -> Task<> {
    const Server server({
        {
            "HTTP/1.1 429 Too Many Requests\r\n"
            "Retry-After: 2\r\n"
            "Content-Length: 4\r\n"
            "\r\n"
            "slow",
        },
    });
    const std::unique_ptr<http::Client> client = http::NewClient();

    const absl::StatusOr<http::Response> response =
        co_await Get(*client, server.Url("/"));

    ABSL_EXPECT_OK(response);
    if (!response.ok()) co_return;
    EXPECT_EQ(response->status, 429);
    EXPECT_EQ(response->Get("Retry-After"), "2");
    EXPECT_EQ(response->body, "slow");
  }());
}

// Servers that do not know the length up front send the body in chunks.
TEST(ClientTest, ReadsAChunkedBody) {
  RunOnEventLoop([]() -> Task<> {
    const Server server({
        {
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "5\r\nhello\r\n"
            "7;note=ignored\r\n, world\r\n"
            "0\r\n"
            "Trailer: ignored\r\n"
            "\r\n",
        },
    });
    const std::unique_ptr<http::Client> client = http::NewClient();

    const absl::StatusOr<http::Response> response =
        co_await Get(*client, server.Url("/"));

    ABSL_EXPECT_OK(response);
    if (response.ok()) EXPECT_EQ(response->body, "hello, world");
  }());
}

// With neither a length nor chunks, the body is whatever arrives before the
// server closes the connection.
TEST(ClientTest, ReadsABodyThatEndsWhenTheConnectionDoes) {
  RunOnEventLoop([]() -> Task<> {
    const Server server({
        {
            "HTTP/1.1 200 OK\r\n\r\nuntil the end",
        },
    });
    const std::unique_ptr<http::Client> client = http::NewClient();

    const absl::StatusOr<http::Response> response =
        co_await Get(*client, server.Url("/"));

    ABSL_EXPECT_OK(response);
    if (response.ok()) EXPECT_EQ(response->body, "until the end");
  }());
}

// Requests to the same server share one connection: the server here accepts
// only one, and answers three requests on it.
TEST(ClientTest, ReusesTheConnection) {
  RunOnEventLoop([]() -> Task<> {
    const Server server({
        {
            Ok("1"),
            Ok("2"),
            Ok("3"),
        },
    });
    const std::unique_ptr<http::Client> client = http::NewClient();

    std::vector<std::string> bodies;
    for (int i = 0; i < 3; ++i) {
      const absl::StatusOr<http::Response> response =
          co_await Get(*client, server.Url("/"));
      ABSL_EXPECT_OK(response);
      if (response.ok()) bodies.push_back(response->body);
    }

    EXPECT_THAT(bodies, ElementsAre("1", "2", "3"));
  }());
}

// Servers close idle connections without warning. The caller never sees that:
// the request is simply made on a new connection.
TEST(ClientTest, ReconnectsWhenTheServerDroppedTheConnection) {
  RunOnEventLoop([]() -> Task<> {
    // Two connections, each closed after one reply.
    const Server server({
        {
            Ok("first"),
        },
        {
            Ok("second"),
        },
    });
    const std::unique_ptr<http::Client> client = http::NewClient();

    std::vector<std::string> bodies;
    for (int i = 0; i < 2; ++i) {
      const absl::StatusOr<http::Response> response =
          co_await Get(*client, server.Url("/"));
      ABSL_EXPECT_OK(response);
      if (response.ok()) bodies.push_back(response->body);
    }

    EXPECT_THAT(bodies, ElementsAre("first", "second"));
  }());
}

// What is not HTTP is reported as such rather than returned as a response.
TEST(ClientTest, RejectsAReplyThatIsNotHttp) {
  RunOnEventLoop([]() -> Task<> {
    const Server server({
        {
            "SSH-2.0-OpenSSH\r\n\r\n",
        },
    });
    const std::unique_ptr<http::Client> client = http::NewClient();

    EXPECT_THAT(co_await Get(*client, server.Url("/")),
                StatusIs(absl::StatusCode::kInvalidArgument));
  }());
}

// The failures that mean "no response was had".
TEST(ClientTest, ReportsWhyThereIsNoResponse) {
  RunOnEventLoop([]() -> Task<> {
    const std::unique_ptr<http::Client> client = http::NewClient();

    EXPECT_THAT(co_await Get(*client, "ftp://example.com/"),
                StatusIs(absl::StatusCode::kInvalidArgument));

    // A port that was listened on a moment ago and no longer is.
    std::string dead_url;
    {
      const absl::StatusOr<net::Listener> gone = net::Listener::OnLoopback();
      ABSL_EXPECT_OK(gone);
      dead_url = absl::StrCat("http://127.0.0.1:", gone->address().port, "/");
    }
    EXPECT_THAT(co_await Get(*client, dead_url),
                StatusIs(absl::StatusCode::kUnavailable));
  }());
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //http:client_test -- --benchmark_filter=all
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
//   BM_RoundTrip      10741 ns        10741 ns        38087
// End of results.
// clang-format on

Task<> RoundTrips(benchmark::State& state) {
  // A server that answers every request on one connection, for ever.
  const absl::StatusOr<net::Listener> listener = net::Listener::OnLoopback();
  Spawn([](const net::Listener& listener) -> Task<> {
    const absl::StatusOr<std::unique_ptr<net::Stream>> stream =
        co_await listener.Accept();
    net::Reader reader(stream->get());
    const std::string reply = Ok(R"({"id":"1","content":"gm"})");
    while ((co_await http::ReadHead(reader, net::After(std::chrono::hours(1))))
               .ok()) {
      (co_await (*stream)->Write(reply)).IgnoreError();
    }
  }(*listener));

  const std::unique_ptr<http::Client> client = http::NewClient();
  const std::string url =
      absl::StrCat("http://127.0.0.1:", listener->address().port, "/");
  for (auto _ : state) {
    benchmark::DoNotOptimize(co_await Get(*client, url));
  }
}

// One request and response over a connection that is already open: what each
// call to a web API costs on top of the network.
void BM_RoundTrip(benchmark::State& state) {
  EventLoop::Create()->Run(RoundTrips(state));
}
BENCHMARK(BM_RoundTrip);

}  // namespace
