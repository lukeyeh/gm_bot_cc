#include "net/tls.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "async/status_macros.h"
#include "async/task.h"
#include "net/stream.h"

namespace net {
namespace {

// How much ciphertext to ask the transport for at a time: one TLS record at
// its largest, plus its framing.
constexpr size_t kCiphertextChunk = 16 * 1024 + 512;

// Frees whichever TLS library object it is handed.
struct Free {
  void operator()(BIO* bio) const { BIO_free(bio); }
  void operator()(X509* certificate) const { X509_free(certificate); }
  void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
  void operator()(SSL* session) const { SSL_free(session); }
  void operator()(SSL_CTX* context) const { SSL_CTX_free(context); }
};

template <typename T>
using Owned = std::unique_ptr<T, Free>;

// Describes the most recent failure inside the TLS library.
std::string LibraryError() {
  const unsigned long code = ERR_get_error();
  ERR_clear_error();
  if (code == 0) return "connection closed";
  std::array<char, 256> text{};
  ERR_error_string_n(code, text.data(), text.size());
  return text.data();
}

Owned<BIO> MemoryBio(std::string_view contents) {
  return Owned<BIO>(
      BIO_new_mem_buf(contents.data(), static_cast<int>(contents.size())));
}

absl::Status TrustRoots(SSL_CTX* context, std::string_view pem) {
  if (pem.empty()) {
    if (SSL_CTX_set_default_verify_paths(context) != 1) {
      return absl::InternalError(
          absl::StrCat("cannot load system roots: ", LibraryError()));
    }
    return absl::OkStatus();
  }

  const Owned<BIO> source = MemoryBio(pem);
  int added = 0;
  for (;;) {
    const Owned<X509> root(
        PEM_read_bio_X509(source.get(), nullptr, nullptr, nullptr));
    if (root == nullptr) break;

    X509_STORE_add_cert(SSL_CTX_get_cert_store(context), root.get());
    ++added;
  }
  ERR_clear_error();  // Running off the end of the PEM is how the loop ends.

  if (added == 0) {
    return absl::InvalidArgumentError("no certificates in trusted roots");
  }
  return absl::OkStatus();
}

absl::Status UseIdentity(SSL_CTX* context, const TlsIdentity& identity) {
  const Owned<BIO> certificate_source = MemoryBio(identity.certificate_pem);
  const Owned<X509> certificate(
      PEM_read_bio_X509(certificate_source.get(), nullptr, nullptr, nullptr));

  const Owned<BIO> key_source = MemoryBio(identity.private_key_pem);
  const Owned<EVP_PKEY> key(
      PEM_read_bio_PrivateKey(key_source.get(), nullptr, nullptr, nullptr));

  if (certificate == nullptr || key == nullptr ||
      SSL_CTX_use_certificate(context, certificate.get()) != 1 ||
      SSL_CTX_use_PrivateKey(context, key.get()) != 1) {
    return absl::InvalidArgumentError(
        absl::StrCat("unusable TLS identity: ", LibraryError()));
  }

  return absl::OkStatus();
}

Owned<SSL_CTX> NewContext() {
  Owned<SSL_CTX> context(SSL_CTX_new(TLS_method()));
  SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION);
  return context;
}

// Runs the TLS protocol over another stream.
//
// The TLS library never touches the network. It reads ciphertext from one
// memory buffer (`incoming_`) and writes it to another (`outgoing_`), and
// says so when it needs more of the former. This class moves bytes between
// those buffers and the transport, which is where the waiting happens: the
// library is never in the middle of a call while a task is suspended.
class TlsStream final : public Stream {
 public:
  TlsStream(std::unique_ptr<Stream> transport, Owned<SSL> session)
      : transport_(std::move(transport)),
        session_(std::move(session)),
        incoming_(BIO_new(BIO_s_mem())),
        outgoing_(BIO_new(BIO_s_mem())) {
    SSL_set_bio(session_.get(), incoming_, outgoing_);  // Takes ownership.
  }

  Task<absl::Status> Handshake(Deadline deadline) {
    for (;;) {
      const int result = SSL_do_handshake(session_.get());
      // Read before Flush, which may run other library calls' errors over it.
      const int need = SSL_get_error(session_.get(), result);

      CO_RETURN_IF_ERROR(co_await Flush());
      if (result == 1) co_return absl::OkStatus();
      if (need != SSL_ERROR_WANT_READ) break;

      CO_RETURN_IF_ERROR(co_await Feed(deadline));
    }

    const long verdict = SSL_get_verify_result(session_.get());
    if (verdict != X509_V_OK) {
      ERR_clear_error();
      co_return absl::UnauthenticatedError(
          absl::StrCat("peer certificate rejected: ",
                       X509_verify_cert_error_string(verdict)));
    }

    co_return absl::UnavailableError(
        absl::StrCat("TLS handshake failed: ", LibraryError()));
  }

  Task<absl::StatusOr<size_t>> Read(std::span<char> buffer,
                                    Deadline deadline) override {
    for (;;) {
      const int count =
          SSL_read(session_.get(), buffer.data(),
                   static_cast<int>(std::min<size_t>(buffer.size(), INT_MAX)));
      if (count > 0) co_return static_cast<size_t>(count);

      const int need = SSL_get_error(session_.get(), count);
      // The peer said goodbye properly.
      if (need == SSL_ERROR_ZERO_RETURN) co_return 0;
      if (need != SSL_ERROR_WANT_READ) {
        co_return absl::UnavailableError(
            absl::StrCat("TLS read failed: ", LibraryError()));
      }

      // Reading can oblige the library to answer the peer, for instance when
      // the peer asks to change keys.
      CO_RETURN_IF_ERROR(co_await Flush());
      CO_RETURN_IF_ERROR(co_await Feed(deadline));
    }
  }

  Task<absl::Status> Write(std::string_view data) override {
    while (!data.empty()) {
      const int count =
          SSL_write(session_.get(), data.data(),
                    static_cast<int>(std::min<size_t>(data.size(), INT_MAX)));
      if (count <= 0) {
        co_return absl::UnavailableError(
            absl::StrCat("TLS write failed: ", LibraryError()));
      }
      data.remove_prefix(static_cast<size_t>(count));
    }

    co_return co_await Flush();
  }

 private:
  // Sends the ciphertext the library has produced.
  Task<absl::Status> Flush() {
    const char* bytes = nullptr;
    const long size = BIO_get_mem_data(outgoing_, &bytes);
    if (size <= 0) co_return absl::OkStatus();

    const absl::Status written = co_await transport_->Write(
        std::string_view(bytes, static_cast<size_t>(size)));
    // Only now, since the write refers to the buffer's memory.
    BIO_reset(outgoing_);
    co_return written;
  }

  // Receives more ciphertext for the library to read.
  Task<absl::Status> Feed(Deadline deadline) {
    std::array<char, kCiphertextChunk> chunk;
    CO_ASSIGN_OR_RETURN(const size_t count,
                        co_await transport_->Read(chunk, deadline));
    if (count == 0) {
      // Without the goodbye that TLS requires, so possibly cut short.
      co_return absl::UnavailableError("connection closed");
    }

    BIO_write(incoming_, chunk.data(), static_cast<int>(count));
    co_return absl::OkStatus();
  }

  std::unique_ptr<Stream> transport_;
  Owned<SSL> session_;
  // Ciphertext from the peer that the library has yet to read, and ciphertext
  // from the library that has yet to be sent. Owned by session_.
  BIO* incoming_;
  BIO* outgoing_;
};

Task<absl::StatusOr<std::unique_ptr<Stream>>> Handshake(
    std::unique_ptr<Stream> transport, Owned<SSL> session, Deadline deadline) {
  auto stream =
      std::make_unique<TlsStream>(std::move(transport), std::move(session));
  CO_RETURN_IF_ERROR(co_await stream->Handshake(deadline));

  co_return stream;
}

// The parts of connecting that cannot wait.
absl::StatusOr<Owned<SSL>> NewClientSession(const std::string& hostname,
                                            std::string_view roots_pem) {
  const Owned<SSL_CTX> context = NewContext();
  ABSL_RETURN_IF_ERROR(TrustRoots(context.get(), roots_pem));
  SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);

  Owned<SSL> session(SSL_new(context.get()));
  SSL_set_connect_state(session.get());
  if (SSL_set_tlsext_host_name(session.get(), hostname.c_str()) != 1 ||
      SSL_set1_host(session.get(), hostname.c_str()) != 1) {
    return absl::InvalidArgumentError(
        absl::StrCat("unusable TLS hostname ", hostname, ": ", LibraryError()));
  }

  return session;
}

absl::StatusOr<Owned<SSL>> NewServerSession(const TlsIdentity& identity) {
  const Owned<SSL_CTX> context = NewContext();
  ABSL_RETURN_IF_ERROR(UseIdentity(context.get(), identity));

  Owned<SSL> session(SSL_new(context.get()));
  SSL_set_accept_state(session.get());
  return session;
}

}  // namespace

Task<absl::StatusOr<std::unique_ptr<Stream>>> TlsConnect(
    std::unique_ptr<Stream> transport, std::string hostname, Deadline deadline,
    std::string trusted_roots_pem) {
  CO_ASSIGN_OR_RETURN(Owned<SSL> session,
                      NewClientSession(hostname, trusted_roots_pem));

  co_return co_await Handshake(std::move(transport), std::move(session),
                               deadline);
}

Task<absl::StatusOr<std::unique_ptr<Stream>>> TlsAccept(
    std::unique_ptr<Stream> transport, TlsIdentity identity,
    Deadline deadline) {
  CO_ASSIGN_OR_RETURN(Owned<SSL> session, NewServerSession(identity));

  co_return co_await Handshake(std::move(transport), std::move(session),
                               deadline);
}

}  // namespace net
