// Reads a Stream in the units protocols are made of (lines, fixed-size
// blocks) rather than in whatever pieces the network delivers.
//
// A Reader never loses data to a deadline: an operation that times out has
// consumed nothing, and repeating it later picks up where the stream is.
//
// What a read returns is not a copy: it refers to memory inside the reader
// and is valid only until the reader's next operation.

#ifndef NET_READER_H_
#define NET_READER_H_

#include <cstddef>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "net/stream.h"

namespace net {

class Reader {
 public:
  // `stream` must outlive the reader, and must not be read from directly while
  // the reader is in use, since the reader reads ahead.
  explicit Reader(Stream* stream) : stream_(stream) {}

  // Consumes and returns exactly the next `bytes` bytes.
  Task<absl::StatusOr<std::string_view>> Read(size_t bytes, Deadline deadline);

  // Consumes the bytes up to and including the next `delimiter`, and returns
  // them without it. Fails with ResourceExhausted if that would be more than
  // `max_bytes`, so that a peer cannot make the reader buffer without bound.
  Task<absl::StatusOr<std::string_view>> ReadUntil(std::string_view delimiter,
                                                   size_t max_bytes,
                                                   Deadline deadline);

  // Consumes and returns everything up to the end of the stream.
  Task<absl::StatusOr<std::string_view>> ReadToEnd(Deadline deadline);

  // Waits until at least `bytes` bytes are available from Peek, consuming
  // nothing. This is how to look at a header before deciding how much to read.
  Task<absl::Status> Fill(size_t bytes, Deadline deadline);

  // The bytes received but not yet consumed.
  std::string_view Peek() const {
    return std::string_view(buffer_).substr(consumed_, filled_ - consumed_);
  }

  // All of the above fail with Unavailable if the stream ends or breaks
  // before they are satisfied, and DeadlineExceeded if the deadline passes
  // first.

 private:
  // Receives more bytes into the buffer. Evaluates to whether there were
  // any: false is the end of the stream.
  Task<absl::StatusOr<bool>> Receive(Deadline deadline);

  // Arranges for there to be free space after filled_, by reusing the space
  // of bytes already returned or, failing that, growing the buffer.
  void MakeRoom();

  Stream* stream_;

  // Storage for incoming bytes. Bytes [consumed_, filled_) have been received
  // but not yet returned; bytes from filled_ on are free space.
  std::string buffer_;
  size_t consumed_ = 0;
  size_t filled_ = 0;
};

}  // namespace net

#endif  // NET_READER_H_
