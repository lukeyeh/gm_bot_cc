#include "net/reader.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "async/status_macros.h"
#include "async/task.h"
#include "net/stream.h"

namespace net {
namespace {

// Initial size of the buffer, and so the most that one receive can return
// until the buffer has had to grow.
constexpr size_t kInitialBufferBytes = 4096;

absl::Status Truncated() {
  return absl::UnavailableError("connection closed mid-message");
}

}  // namespace

void Reader::MakeRoom() {
  if (consumed_ > 0) {
    std::copy(buffer_.data() + consumed_, buffer_.data() + filled_,
              buffer_.data());
    filled_ -= consumed_;
    consumed_ = 0;
  }

  if (filled_ == buffer_.size()) {
    buffer_.resize(std::max(kInitialBufferBytes, 2 * buffer_.size()));
  }
}

Task<absl::StatusOr<bool>> Reader::Receive(Deadline deadline) {
  MakeRoom();
  const absl::StatusOr<size_t> received = co_await stream_->Read(
      std::span<char>(buffer_).subspan(filled_), deadline);

  if (absl::IsDeadlineExceeded(received.status())) co_return received.status();
  // Whatever else went wrong, to the caller it means the peer is not there.
  if (!received.ok()) {
    co_return absl::UnavailableError(received.status().message());
  }

  filled_ += *received;
  co_return *received > 0;
}

Task<absl::Status> Reader::Fill(size_t bytes, Deadline deadline) {
  while (filled_ - consumed_ < bytes) {
    CO_ASSIGN_OR_RETURN(const bool more, co_await Receive(deadline));
    if (!more) co_return Truncated();
  }

  co_return absl::OkStatus();
}

Task<absl::StatusOr<std::string_view>> Reader::Read(size_t bytes,
                                                    Deadline deadline) {
  CO_RETURN_IF_ERROR(co_await Fill(bytes, deadline));

  const std::string_view result = Peek().substr(0, bytes);
  consumed_ += bytes;
  co_return result;
}

Task<absl::StatusOr<std::string_view>> Reader::ReadUntil(
    std::string_view delimiter, size_t max_bytes, Deadline deadline) {
  for (;;) {
    const std::string_view unread = Peek();
    const size_t length = unread.find(delimiter);
    if (length != std::string_view::npos && length <= max_bytes) {
      consumed_ += length + delimiter.size();
      co_return unread.substr(0, length);
    }

    // Checked before reading more so that a peer sending an endless piece
    // cannot make the buffer grow without bound.
    if (unread.size() > max_bytes) {
      co_return absl::ResourceExhaustedError(
          absl::StrCat("no delimiter within ", max_bytes, " bytes"));
    }

    CO_ASSIGN_OR_RETURN(const bool more, co_await Receive(deadline));
    if (!more) co_return Truncated();
  }
}

Task<absl::StatusOr<std::string_view>> Reader::ReadToEnd(Deadline deadline) {
  for (;;) {
    CO_ASSIGN_OR_RETURN(const bool more, co_await Receive(deadline));
    if (!more) break;
  }

  const std::string_view result = Peek();
  consumed_ = filled_;
  co_return result;
}

}  // namespace net
