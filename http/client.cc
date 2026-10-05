#include "http/client.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "async/status_macros.h"
#include "async/task.h"
#include "http/head.h"
#include "net/reader.h"
#include "net/stream.h"
#include "net/url.h"

namespace http {
namespace {

// How long one request may take from start to finish.
constexpr std::chrono::seconds kTimeout(30);

// More than any API response should be; a bound on what a server can make
// the client hold in memory.
constexpr uint64_t kMaxBodyBytes = uint64_t{64} * 1024 * 1024;

// Longer than any line of a well-behaved chunked body.
constexpr size_t kMaxLineBytes = size_t{16} * 1024;

std::string_view Name(Method method) {
  switch (method) {
    case Method::kGet: return "GET";
    case Method::kPost: return "POST";
    case Method::kPut: return "PUT";
    case Method::kPatch: return "PATCH";
    case Method::kDelete: return "DELETE";
  }
  return "GET";
}

absl::Status TooLarge() {
  return absl::ResourceExhaustedError("HTTP response body too large");
}

std::string Format(const Request& request, const net::Url& url) {
  Head head{
      .start_line =
          absl::StrCat(Name(request.method), " ", url.target, " HTTP/1.1"),
      .headers = request.headers,
  };
  head.headers.push_back(Header{
      .name = "Host",
      .value = url.authority,
  });

  // Stated even when zero for methods that could have had a body, because
  // some servers insist.
  if (request.method != Method::kGet || !request.body.empty()) {
    head.headers.push_back(Header{
        .name = "Content-Length",
        .value = absl::StrCat(request.body.size()),
    });
  }

  return absl::StrCat(FormatHead(head), request.body);
}

// A connection to one server, kept open between requests.
class Connection {
 public:
  Connection(std::unique_ptr<net::Stream> stream, net::Address address)
      : stream_(std::move(stream)),
        reader_(stream_.get()),
        address_(std::move(address)) {}

  bool IsTo(const net::Address& address) const {
    return address_.host == address.host && address_.port == address.port &&
           address_.security == address.security;
  }

  // True once any part of a response has arrived. Until then a failure may
  // just mean the server had closed an idle connection, and it is safe to try
  // again on a new one.
  bool response_started() const { return response_started_; }

  // True if another request may follow the last one on this connection.
  bool reusable() const { return reusable_; }

  Task<absl::StatusOr<Response>> Exchange(const Request& request,
                                          const net::Url& url,
                                          net::Deadline deadline) {
    response_started_ = false;
    reusable_ = false;

    const std::string wire = Format(request, url);
    CO_RETURN_IF_ERROR(co_await stream_->Write(wire));
    CO_RETURN_IF_ERROR(co_await reader_.Fill(1, deadline));
    response_started_ = true;

    Response response;
    // Responses numbered 1xx are progress reports ahead of the real one.
    while (response.status < 200) {
      CO_ASSIGN_OR_RETURN(Head head, co_await ReadHead(reader_, deadline));

      // "HTTP/1.1 200 OK"
      const std::vector<std::string_view> parts =
          absl::StrSplit(head.start_line, absl::MaxSplits(' ', 2));
      if (parts.size() < 2 || !parts[0].starts_with("HTTP/1.") ||
          !absl::SimpleAtoi(parts[1], &response.status)) {
        co_return absl::InvalidArgumentError(
            absl::StrCat("malformed HTTP status line: ", head.start_line));
      }

      response.headers = std::move(head.headers);
    }

    bool ends_at_close = false;
    CO_ASSIGN_OR_RETURN(response.body,
                        co_await ReadBody(response, deadline, ends_at_close));

    reusable_ = !ends_at_close &&
                !absl::EqualsIgnoreCase(response.Get("Connection"), "close");
    co_return response;
  }

 private:
  // Reads the body in whichever of the three ways the response frames it.
  Task<absl::StatusOr<std::string>> ReadBody(const Response& response,
                                             net::Deadline deadline,
                                             bool& ends_at_close) {
    if (response.status == 204 || response.status == 304) {
      co_return std::string();
    }

    if (absl::StrContainsIgnoreCase(response.Get("Transfer-Encoding"),
                                    "chunked")) {
      co_return co_await ReadChunks(deadline);
    }

    const std::string_view length = response.Get("Content-Length");
    if (length.empty()) {
      ends_at_close = true;
      CO_ASSIGN_OR_RETURN(const std::string_view rest,
                          co_await reader_.ReadToEnd(deadline));
      co_return std::string(rest);
    }

    uint64_t bytes = 0;
    if (!absl::SimpleAtoi(length, &bytes)) {
      co_return absl::InvalidArgumentError(
          absl::StrCat("malformed Content-Length: ", length));
    }
    if (bytes > kMaxBodyBytes) co_return TooLarge();

    CO_ASSIGN_OR_RETURN(const std::string_view body,
                        co_await reader_.Read(bytes, deadline));
    co_return std::string(body);
  }

  // A chunked body is a series of "<size in hex>CRLF<bytes>CRLF", ending with
  // a chunk of size zero and optional trailing headers.
  Task<absl::StatusOr<std::string>> ReadChunks(net::Deadline deadline) {
    std::string body;
    for (;;) {
      CO_ASSIGN_OR_RETURN(
          const std::string_view line,
          co_await reader_.ReadUntil("\r\n", kMaxLineBytes, deadline));

      uint64_t bytes = 0;
      // Anything after a semicolon is an extension, to be ignored.
      if (!absl::SimpleHexAtoi(line.substr(0, line.find(';')), &bytes)) {
        co_return absl::InvalidArgumentError(
            absl::StrCat("malformed HTTP chunk size: ", line));
      }
      if (bytes == 0) break;
      if (body.size() + bytes > kMaxBodyBytes) co_return TooLarge();

      // Each chunk is followed by a CRLF that is not part of it.
      CO_ASSIGN_OR_RETURN(const std::string_view chunk,
                          co_await reader_.Read(bytes + 2, deadline));
      body.append(chunk.substr(0, bytes));
    }

    for (;;) {
      CO_ASSIGN_OR_RETURN(
          const std::string_view trailer,
          co_await reader_.ReadUntil("\r\n", kMaxLineBytes, deadline));
      if (trailer.empty()) co_return body;
    }
  }

  std::unique_ptr<net::Stream> stream_;
  net::Reader reader_;
  net::Address address_;
  bool response_started_ = false;
  bool reusable_ = false;
};

class NetClient final : public Client {
 public:
  Task<absl::StatusOr<Response>> Send(const Request& request) override {
    CO_ASSIGN_OR_RETURN(const net::Url url, net::ParseUrl(request.url));
    const net::Deadline deadline = net::After(kTimeout);

    if (connection_.has_value() && connection_->IsTo(url.address)) {
      absl::StatusOr<Response> response =
          co_await connection_->Exchange(request, url, deadline);
      if (response.ok() || connection_->response_started()) {
        co_return Finish(std::move(response));
      }
      // The server had closed the idle connection; start again on a new one.
    }

    connection_.reset();
    CO_ASSIGN_OR_RETURN(std::unique_ptr<net::Stream> stream,
                        co_await net::Dial(url.address, deadline));
    connection_.emplace(std::move(stream), url.address);

    co_return Finish(co_await connection_->Exchange(request, url, deadline));
  }

 private:
  // Keeps the connection for next time if it is still good.
  absl::StatusOr<Response> Finish(absl::StatusOr<Response> response) {
    if (connection_.has_value() &&
        (!response.ok() || !connection_->reusable())) {
      connection_.reset();
    }
    return response;
  }

  std::optional<Connection> connection_;
};

}  // namespace

std::unique_ptr<Client> NewClient() { return std::make_unique<NetClient>(); }

}  // namespace http
