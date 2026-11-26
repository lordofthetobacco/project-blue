#include "net/address_resolver.h"

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

namespace blue::net {
namespace {

using ResolveResult = core::Result<std::string, ResolveError>;

constexpr int kMaxPort = 65535;

#ifdef _WIN32
// Winsock needs WSAStartup before getaddrinfo. Reference-counted, so it
// pairs with the netcode runtime's own startup in any order and any number
// of lookups may overlap.
class WinsockScope {
 public:
  WinsockScope() {
    const std::lock_guard<std::mutex> lock(Mutex());
    if (Count() == 0) {
      WSADATA data;
      started_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
      if (!started_) {
        return;
      }
    }
    ++Count();
    counted_ = true;
  }
  WinsockScope(const WinsockScope&) = delete;
  WinsockScope& operator=(const WinsockScope&) = delete;
  ~WinsockScope() {
    if (!counted_) {
      return;
    }
    const std::lock_guard<std::mutex> lock(Mutex());
    if (--Count() == 0) {
      WSACleanup();
    }
  }

  bool ok() const { return counted_; }

 private:
  static std::mutex& Mutex() {
    static std::mutex mutex;
    return mutex;
  }
  static int& Count() {
    static int count = 0;
    return count;
  }

  bool started_ = false;
  bool counted_ = false;
};
#else
class WinsockScope {
 public:
  bool ok() const { return true; }
};
#endif

// A server address split into its host (without brackets) and port.
struct HostPort {
  std::string host;
  bool bracketed = false;  // The host was written as "[v6]".
  int port = 0;
};

// Parses a decimal port in 1..65535. Returns 0 when `text` is not one.
int ParsePort(std::string_view text) {
  if (text.empty() || text.size() > 5) {
    return 0;
  }
  int port = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return 0;
    }
    port = port * 10 + (c - '0');
  }
  return port <= kMaxPort ? port : 0;
}

// Splits "host:port" or "[v6]:port". A bare IPv6 host (more than one colon
// without brackets) is malformed, as the port would be ambiguous.
std::optional<HostPort> SplitAddress(std::string_view address) {
  HostPort result;
  std::string_view port_text;
  if (!address.empty() && address.front() == '[') {
    const size_t close = address.find(']');
    if (close == std::string_view::npos || close + 1 >= address.size() ||
        address[close + 1] != ':') {
      return std::nullopt;
    }
    result.host = std::string(address.substr(1, close - 1));
    result.bracketed = true;
    port_text = address.substr(close + 2);
  } else {
    const size_t colon = address.rfind(':');
    if (colon == std::string_view::npos) {
      return std::nullopt;
    }
    result.host = std::string(address.substr(0, colon));
    if (result.host.find(':') != std::string::npos) {
      return std::nullopt;
    }
    port_text = address.substr(colon + 1);
  }
  result.port = ParsePort(port_text);
  if (result.host.empty() || result.port == 0) {
    return std::nullopt;
  }
  return result;
}

// True when `host` is a numeric address of the form its brackets announce:
// dotted IPv4 without brackets, IPv6 with them.
bool IsNumericHost(const HostPort& parts) {
  if (parts.bracketed) {
    in6_addr v6;
    return inet_pton(AF_INET6, parts.host.c_str(), &v6) == 1;
  }
  in_addr v4;
  return inet_pton(AF_INET, parts.host.c_str(), &v4) == 1;
}

// Formats `address` numerically with `port`; IPv6 comes out bracketed.
std::optional<std::string> FormatAddress(const addrinfo& address, int port) {
  char text[INET6_ADDRSTRLEN] = {};
  if (address.ai_family == AF_INET) {
    const auto* v4 = reinterpret_cast<const sockaddr_in*>(address.ai_addr);
    if (inet_ntop(AF_INET, &v4->sin_addr, text, sizeof(text)) == nullptr) {
      return std::nullopt;
    }
    return std::string(text) + ":" + std::to_string(port);
  }
  if (address.ai_family == AF_INET6) {
    const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address.ai_addr);
    if (inet_ntop(AF_INET6, &v6->sin6_addr, text, sizeof(text)) == nullptr) {
      return std::nullopt;
    }
    return "[" + std::string(text) + "]:" + std::to_string(port);
  }
  return std::nullopt;
}

// Looks `host` up and returns the first IPv4 result, else the first IPv6 one.
ResolveResult Lookup(const std::string& host, int port) {
  const WinsockScope winsock;
  if (!winsock.ok()) {
    return ResolveResult::Error(ResolveError::kNotFound);
  }
  addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* results = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0 ||
      results == nullptr) {
    return ResolveResult::Error(ResolveError::kNotFound);
  }
  const addrinfo* first_v4 = nullptr;
  const addrinfo* first_v6 = nullptr;
  for (const addrinfo* it = results; it != nullptr; it = it->ai_next) {
    if (it->ai_family == AF_INET && first_v4 == nullptr) {
      first_v4 = it;
    } else if (it->ai_family == AF_INET6 && first_v6 == nullptr) {
      first_v6 = it;
    }
  }
  const addrinfo* chosen = first_v4 != nullptr ? first_v4 : first_v6;
  std::optional<std::string> formatted;
  if (chosen != nullptr) {
    formatted = FormatAddress(*chosen, port);
  }
  freeaddrinfo(results);
  if (!formatted.has_value()) {
    return ResolveResult::Error(ResolveError::kNotFound);
  }
  return ResolveResult::Ok(*std::move(formatted));
}

}  // namespace

std::string_view ResolveErrorText(ResolveError error) {
  switch (error) {
    case ResolveError::kMalformed:
      return "malformed server address";
    case ResolveError::kNotFound:
      return "host name not found";
  }
  return "unknown resolve error";
}

bool IsNumericAddress(std::string_view address) {
  const std::optional<HostPort> parts = SplitAddress(address);
  return parts.has_value() && IsNumericHost(*parts);
}

ResolveResult ResolveAddress(std::string_view address) {
  const std::optional<HostPort> parts = SplitAddress(address);
  if (!parts.has_value()) {
    return ResolveResult::Error(ResolveError::kMalformed);
  }
  if (IsNumericHost(*parts)) {
    return ResolveResult::Ok(std::string(address));
  }
  if (parts->bracketed) {
    // "[name]:port": brackets are only for IPv6 literals.
    return ResolveResult::Error(ResolveError::kMalformed);
  }
  return Lookup(parts->host, parts->port);
}

// --- PendingResolve ----------------------------------------------------------

// Shared between the PendingResolve and its lookup thread, so it outlives
// whichever finishes last.
struct PendingResolve::State {
  std::mutex mutex;
  std::optional<ResolveResult> result;
};

PendingResolve::PendingResolve(std::string address)
    : state_(std::make_shared<State>()) {
  std::thread([state = state_, address = std::move(address)]() {
    ResolveResult result = ResolveAddress(address);
    const std::lock_guard<std::mutex> lock(state->mutex);
    state->result = std::move(result);
  }).detach();
}

PendingResolve::~PendingResolve() = default;

std::optional<ResolveResult> PendingResolve::Poll() {
  const std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->result;
}

}  // namespace blue::net
