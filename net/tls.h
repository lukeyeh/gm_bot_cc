// TLS as a layer over any Stream: hand in a connected stream, get back one
// that encrypts everything written to it and has verified who is on the other
// end. Most code wants `net::Dial`, which applies this for you.

#ifndef NET_TLS_H_
#define NET_TLS_H_

#include <memory>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "async/task.h"
#include "net/stream.h"

namespace net {

// Performs the client side of a TLS handshake over `transport` and evaluates
// to the encrypted stream. The peer must present a certificate for `hostname`
// that chains to one of `trusted_roots_pem`, or, if that is empty, to one of
// the roots this machine trusts. Fails with Unauthenticated if it does not.
Task<absl::StatusOr<std::unique_ptr<Stream>>> TlsConnect(
    std::unique_ptr<Stream> transport, std::string hostname, Deadline deadline,
    std::string trusted_roots_pem = {});

// What a TLS server proves its identity with, both in PEM form.
struct TlsIdentity {
  std::string certificate_pem;
  std::string private_key_pem;
};

// Performs the server side of a TLS handshake over `transport`, presenting
// `identity`, and evaluates to the encrypted stream.
Task<absl::StatusOr<std::unique_ptr<Stream>>> TlsAccept(
    std::unique_ptr<Stream> transport, TlsIdentity identity, Deadline deadline);

}  // namespace net

#endif  // NET_TLS_H_
