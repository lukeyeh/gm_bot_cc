// An http::Client for tests: it records the requests it is sent and answers
// them from a queue, so code that calls a web API can be tested without one.

#ifndef HTTP_FAKE_CLIENT_H_
#define HTTP_FAKE_CLIENT_H_

#include <deque>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "async/task.h"
#include "http/client.h"

namespace http {

class FakeClient final : public Client {
 public:
  // Queues the outcome of a future request: a response, or a failure to get
  // one. Requests are answered in the order outcomes were queued, and with
  // 204 No Content once the queue is empty.
  void Answer(absl::StatusOr<Response> outcome) {
    outcomes_.push_back(std::move(outcome));
  }

  // Every request sent so far, oldest first.
  const std::vector<Request>& requests() const { return requests_; }

  Task<absl::StatusOr<Response>> Send(const Request& request) override {
    requests_.push_back(request);
    if (outcomes_.empty()) {
      co_return Response{
          .status = 204,
      };
    }
    absl::StatusOr<Response> outcome = std::move(outcomes_.front());
    outcomes_.pop_front();
    co_return outcome;
  }

 private:
  std::deque<absl::StatusOr<Response>> outcomes_;
  std::vector<Request> requests_;
};

}  // namespace http

#endif  // HTTP_FAKE_CLIENT_H_
