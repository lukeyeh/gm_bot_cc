// http::FakeClient by example: how a test uses it in place of a real server.

#include "http/fake_client.h"

#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "http/client.h"
#include "net/event_loop.h"

namespace {

using absl_testing::StatusIs;

void RunOnEventLoop(Task<> test) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(std::move(test));
}

// Queued outcomes are handed out in order, and the requests are kept for the
// test to inspect.
TEST(FakeClientTest, AnswersFromTheQueueAndRecordsRequests) {
  RunOnEventLoop([]() -> Task<> {
    http::FakeClient client;
    client.Answer(http::Response{
        .status = 200,
        .body = "first",
    });
    client.Answer(absl::UnavailableError("network down"));

    const absl::StatusOr<http::Response> first =
        co_await client.Send(http::Request{
            .url = "https://example.com/a",
        });
    ABSL_EXPECT_OK(first);
    if (first.ok()) EXPECT_EQ(first->body, "first");
    EXPECT_THAT(co_await client.Send(http::Request{
                    .method = http::Method::kPost,
                    .url = "https://example.com/b",
                }),
                StatusIs(absl::StatusCode::kUnavailable));

    EXPECT_EQ(client.requests().size(), 2);
    EXPECT_EQ(client.requests().front().url, "https://example.com/a");
    EXPECT_EQ(client.requests().back().method, http::Method::kPost);
  }());
}

// Tests that do not care about responses need not queue any.
TEST(FakeClientTest, AnswersNoContentByDefault) {
  RunOnEventLoop([]() -> Task<> {
    http::FakeClient client;

    const absl::StatusOr<http::Response> response =
        co_await client.Send(http::Request{
            .url = "https://example.com/",
        });
    ABSL_EXPECT_OK(response);
    if (response.ok()) EXPECT_EQ(response->status, 204);
  }());
}

}  // namespace
