#include "websocket/websocket.h"

#include <openssl/evp.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "async/status_macros.h"
#include "async/task.h"
#include "http/head.h"
#include "net/reader.h"
#include "net/stream.h"
#include "net/url.h"

namespace websocket {
namespace {

// How long the opening handshake may take.
constexpr std::chrono::seconds kHandshakeTimeout(30);

// A bound on what a peer can make us hold in memory for one message.
constexpr uint64_t kMaxMessageBytes = uint64_t{64} * 1024 * 1024;

// Frame types, as numbered by RFC 6455.
enum class Opcode : uint8_t {
  kContinuation = 0x0,
  kText = 0x1,
  kBinary = 0x2,
  kClose = 0x8,
  kPing = 0x9,
  kPong = 0xA,
};

// In a frame's first byte: this frame is the last of its message.
constexpr uint8_t kFinalBit = 0x80;
// In a frame's second byte: the payload is masked.
constexpr uint8_t kMaskedBit = 0x80;

// Clients disguise their payloads from intermediaries; servers do not.
enum class Role : uint8_t { kClient, kServer };

// The value a server must answer a handshake key with, proving that it
// understood the request as a WebSocket handshake: the SHA-1 of the key and a
// fixed string, in base64.
std::string AcceptValue(std::string_view key) {
  const std::string input =
      absl::StrCat(key, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  EVP_Digest(input.data(), input.size(), digest.data(), &size, EVP_sha1(),
             nullptr);
  return absl::Base64Escape(
      std::string_view(reinterpret_cast<const char*>(digest.data()), size));
}

// A mask is four bytes, repeated along the payload.
constexpr size_t kMaskBytes = 4;

// Writes `payload` to `out` with each byte combined (XOR) with the byte of
// `mask` at its position, which both masks a payload and unmasks one. `out`
// may be where the payload already is.
//
// Written over whole positions of the mask at a time so that the compiler
// can do several bytes in one step; a message can be many kilobytes.
void ApplyMask(std::string_view mask, std::string_view payload, char* out) {
  const size_t whole = payload.size() - payload.size() % kMaskBytes;
  for (size_t i = 0; i < whole; i += kMaskBytes) {
    for (size_t j = 0; j < kMaskBytes; ++j) {
      out[i + j] = static_cast<char>(payload[i + j] ^ mask[j]);
    }
  }
  for (size_t i = whole; i < payload.size(); ++i) {
    out[i] = static_cast<char>(payload[i] ^ mask[i % kMaskBytes]);
  }
}

struct Frame {
  bool final = true;
  Opcode opcode = Opcode::kText;
  std::string payload;
};

class FramedConnection final : public Connection {
 public:
  FramedConnection(std::unique_ptr<net::Stream> stream, Role role)
      : stream_(std::move(stream)), reader_(stream_.get()), role_(role) {}

  // Client side of the opening handshake: an HTTP request asking to switch
  // protocols, which the server must accept by echoing a digest of our key.
  Task<absl::Status> RequestUpgrade(const net::Url& url, Deadline deadline) {
    std::string nonce(16, '\0');
    for (char& byte : nonce) {
      byte = static_cast<char>(absl::Uniform<uint8_t>(random_));
    }
    const std::string key = absl::Base64Escape(nonce);

    const std::string request = http::FormatHead(http::Head{
        .start_line = absl::StrCat("GET ", url.target, " HTTP/1.1"),
        .headers =
            {
                http::Header{
                    .name = "Host",
                    .value = url.authority,
                },
                http::Header{
                    .name = "Upgrade",
                    .value = "websocket",
                },
                http::Header{
                    .name = "Connection",
                    .value = "Upgrade",
                },
                http::Header{
                    .name = "Sec-WebSocket-Key",
                    .value = key,
                },
                http::Header{
                    .name = "Sec-WebSocket-Version",
                    .value = "13",
                },
            },
    });
    CO_RETURN_IF_ERROR(co_await stream_->Write(request));

    CO_ASSIGN_OR_RETURN(const http::Head answer,
                        co_await http::ReadHead(reader_, deadline));
    if (!absl::StartsWith(answer.start_line, "HTTP/1.1 101") ||
        http::FindHeader(answer.headers, "Sec-WebSocket-Accept") !=
            AcceptValue(key)) {
      co_return absl::FailedPreconditionError(absl::StrCat(
          "server declined the WebSocket handshake: ", answer.start_line));
    }

    co_return absl::OkStatus();
  }

  // Server side of the opening handshake.
  Task<absl::Status> AcceptUpgrade(Deadline deadline) {
    CO_ASSIGN_OR_RETURN(const http::Head request,
                        co_await http::ReadHead(reader_, deadline));

    const std::string_view key =
        http::FindHeader(request.headers, "Sec-WebSocket-Key");
    if (key.empty() ||
        !absl::EqualsIgnoreCase(http::FindHeader(request.headers, "Upgrade"),
                                "websocket")) {
      (co_await stream_->Write("HTTP/1.1 400 Bad Request\r\n\r\n"))
          .IgnoreError();
      co_return absl::FailedPreconditionError(
          absl::StrCat("not a WebSocket handshake: ", request.start_line));
    }

    const std::string answer = http::FormatHead(http::Head{
        .start_line = "HTTP/1.1 101 Switching Protocols",
        .headers =
            {
                http::Header{
                    .name = "Upgrade",
                    .value = "websocket",
                },
                http::Header{
                    .name = "Connection",
                    .value = "Upgrade",
                },
                http::Header{
                    .name = "Sec-WebSocket-Accept",
                    .value = AcceptValue(key),
                },
            },
    });
    co_return co_await stream_->Write(answer);
  }

  Task<absl::StatusOr<Incoming>> Receive(Deadline deadline) override {
    for (;;) {
      CO_ASSIGN_OR_RETURN(Frame frame, co_await ReadFrame(deadline));

      switch (frame.opcode) {
        case Opcode::kPing:
          CO_RETURN_IF_ERROR(co_await Write(Opcode::kPong, frame.payload));
          continue;

        case Opcode::kPong: continue;

        case Opcode::kClose: {
          // Echoing the close frame completes the closing handshake. The peer
          // may not wait for it, which is fine.
          (co_await Write(Opcode::kClose, frame.payload.substr(0, 2)))
              .IgnoreError();

          Close close;
          if (frame.payload.size() >= 2) {
            close.code = (static_cast<uint8_t>(frame.payload[0]) << 8) |
                         static_cast<uint8_t>(frame.payload[1]);
            close.reason = frame.payload.substr(2);
          }
          co_return close;
        }

        case Opcode::kText:
        case Opcode::kBinary: partial_ = std::move(frame.payload); break;
        case Opcode::kContinuation: partial_.append(frame.payload); break;

        default:
          co_return absl::InvalidArgumentError("unknown WebSocket frame type");
      }

      if (partial_.size() > kMaxMessageBytes) {
        co_return absl::ResourceExhaustedError("WebSocket message too large");
      }
      if (frame.final) co_return std::exchange(partial_, std::string());
    }
  }

  Task<absl::Status> Send(std::string_view text) override {
    co_return co_await Write(Opcode::kText, text);
  }

 private:
  // Reads one whole frame. Nothing is consumed until all of it has arrived,
  // so a deadline can pass at any point without losing the frame.
  //
  // A frame is: a byte of flags and opcode; a byte holding the mask flag and a
  // 7-bit length, where 126 and 127 mean the real length follows in the next
  // 2 or 8 bytes; a 4-byte mask if flagged; then the payload.
  Task<absl::StatusOr<Frame>> ReadFrame(Deadline deadline) {
    CO_RETURN_IF_ERROR(co_await reader_.Fill(2, deadline));
    const uint8_t length_byte = static_cast<uint8_t>(reader_.Peek()[1]);
    const bool masked = (length_byte & kMaskedBit) != 0;
    const uint8_t short_length = length_byte & 0x7F;
    const size_t length_bytes =
        short_length == 126 ? 2 : (short_length == 127 ? 8 : 0);
    const size_t header_bytes = 2 + length_bytes + (masked ? 4 : 0);

    CO_RETURN_IF_ERROR(co_await reader_.Fill(header_bytes, deadline));
    uint64_t payload_bytes = short_length;
    if (length_bytes > 0) {
      payload_bytes = 0;
      for (const char byte : reader_.Peek().substr(2, length_bytes)) {
        payload_bytes = (payload_bytes << 8) | static_cast<uint8_t>(byte);
      }
    }
    if (payload_bytes > kMaxMessageBytes) {
      co_return absl::ResourceExhaustedError("WebSocket frame too large");
    }

    CO_ASSIGN_OR_RETURN(
        const std::string_view bytes,
        co_await reader_.Read(header_bytes + payload_bytes, deadline));

    const uint8_t first = static_cast<uint8_t>(bytes.front());
    Frame frame{
        .final = (first & kFinalBit) != 0,
        .opcode = static_cast<Opcode>(first & 0x0F),
        .payload = std::string(bytes.substr(header_bytes)),
    };
    if (masked) {
      const std::string_view mask = bytes.substr(header_bytes - kMaskBytes);
      ApplyMask(mask.substr(0, kMaskBytes), frame.payload,
                frame.payload.data());
    }

    co_return frame;
  }

  Task<absl::Status> Write(Opcode opcode, std::string_view payload) {
    std::string frame;
    frame.reserve(payload.size() + 14);
    frame.push_back(
        static_cast<char>(kFinalBit | static_cast<uint8_t>(opcode)));

    const uint8_t mask_bit = role_ == Role::kClient ? kMaskedBit : 0;
    const uint64_t size = payload.size();
    if (size < 126) {
      frame.push_back(static_cast<char>(mask_bit | size));
    } else if (size <= 0xFFFF) {
      frame.push_back(static_cast<char>(mask_bit | 126));
      frame.push_back(static_cast<char>(size >> 8));
      frame.push_back(static_cast<char>(size & 0xFF));
    } else {
      frame.push_back(static_cast<char>(mask_bit | 127));
      for (int shift = 56; shift >= 0; shift -= 8) {
        frame.push_back(static_cast<char>((size >> shift) & 0xFF));
      }
    }

    if (role_ == Role::kClient) {
      std::array<char, kMaskBytes> mask{};
      for (char& byte : mask) {
        byte = static_cast<char>(absl::Uniform<uint8_t>(random_));
      }
      frame.append(mask.data(), mask.size());

      const size_t header_bytes = frame.size();
      frame.resize(header_bytes + payload.size());
      ApplyMask(std::string_view(mask.data(), mask.size()), payload,
                frame.data() + header_bytes);
    } else {
      frame.append(payload);
    }

    co_return co_await stream_->Write(frame);
  }

  std::unique_ptr<net::Stream> stream_;
  net::Reader reader_;
  Role role_;
  absl::BitGen random_;
  // The fragments so far of a message still arriving.
  std::string partial_;
};

}  // namespace

Task<absl::StatusOr<std::unique_ptr<Connection>>> Connect(std::string url) {
  CO_ASSIGN_OR_RETURN(const net::Url parsed, net::ParseUrl(url));
  const Deadline deadline = net::After(kHandshakeTimeout);

  CO_ASSIGN_OR_RETURN(std::unique_ptr<net::Stream> stream,
                      co_await net::Dial(parsed.address, deadline));
  auto connection =
      std::make_unique<FramedConnection>(std::move(stream), Role::kClient);
  CO_RETURN_IF_ERROR(co_await connection->RequestUpgrade(parsed, deadline));

  co_return connection;
}

Task<absl::StatusOr<std::unique_ptr<Connection>>> Accept(
    std::unique_ptr<net::Stream> stream) {
  auto connection =
      std::make_unique<FramedConnection>(std::move(stream), Role::kServer);
  CO_RETURN_IF_ERROR(
      co_await connection->AcceptUpgrade(net::After(kHandshakeTimeout)));

  co_return connection;
}

}  // namespace websocket
