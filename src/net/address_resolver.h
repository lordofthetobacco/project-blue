#ifndef BLUE_NET_ADDRESS_RESOLVER_H_
#define BLUE_NET_ADDRESS_RESOLVER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "core/result.h"

namespace blue::net {

// Turns a server address with a DNS name ("play.example.com:27015") into the
// numeric form the transport accepts (netcode_parse_address only parses
// "a.b.c.d:port" and "[v6]:port"). No netcode type appears in this header.

// Failures of address resolution.
enum class ResolveError : uint8_t {
  kMalformed,  // Not "host:port", empty host, or port outside 1..65535.
  kNotFound,   // The name lookup failed or returned no usable address.
};

// Returns a short lowercase phrase for `error` for logs and status lines.
[[nodiscard]] std::string_view ResolveErrorText(ResolveError error);

// True when `address` is already numeric ("1.2.3.4:27015" or "[::1]:27015"),
// i.e. needs no lookup.
[[nodiscard]] bool IsNumericAddress(std::string_view address);

// Returns `address` ("host:port"; host is a DNS name, IPv4 or [IPv6]) as a
// numeric "a.b.c.d:port" or "[v6]:port" string that netcode_parse_address
// accepts. A numeric address is returned without a lookup. Prefers the first
// IPv4 result of the lookup, else the first IPv6 one. Blocks for the duration
// of a DNS lookup; use PendingResolve on the frame thread.
[[nodiscard]] core::Result<std::string, ResolveError> ResolveAddress(
    std::string_view address);

// A lookup running on its own thread, so the frame loop never blocks on DNS.
// Poll from one thread only.
class PendingResolve {
 public:
  // Starts the lookup of `address` (see ResolveAddress).
  explicit PendingResolve(std::string address);
  PendingResolve(const PendingResolve&) = delete;
  PendingResolve& operator=(const PendingResolve&) = delete;
  // Never blocks: an unfinished lookup is abandoned (getaddrinfo cannot be
  // cancelled) and finishes in the background.
  ~PendingResolve();

  // Returns nullopt while the lookup runs; afterwards the result (the same
  // value on every call).
  [[nodiscard]] std::optional<core::Result<std::string, ResolveError>> Poll();

 private:
  struct State;
  std::shared_ptr<State> state_;
};

}  // namespace blue::net

#endif  // BLUE_NET_ADDRESS_RESOLVER_H_
