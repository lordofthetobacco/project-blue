// net::ResolveAddress and PendingResolve without the internet: numeric
// addresses pass through, "localhost" resolves to a loopback address that
// netcode_parse_address accepts, malformed inputs and a name under the
// reserved ".invalid" domain fail with the matching error.

#include <doctest/doctest.h>
#include <netcode.h>

#include <chrono>
#include <optional>
#include <string>
#include <thread>

#include "net/address_resolver.h"

namespace blue::net {
namespace {

constexpr int kMaxPolls = 3000;
constexpr std::chrono::milliseconds kPollSleep{10};

bool NetcodeAccepts(const std::string& address) {
  netcode_address_t parsed;
  return netcode_parse_address(address.c_str(), &parsed) == NETCODE_OK;
}

}  // namespace

TEST_CASE("numeric addresses pass through unchanged") {
  for (const char* address : {"1.2.3.4:27015", "127.0.0.1:1", "[::1]:27015",
                              "[2001:db8::7]:65535"}) {
    CAPTURE(address);
    CHECK(IsNumericAddress(address));
    const auto resolved = ResolveAddress(address);
    REQUIRE(resolved.ok());
    CHECK(resolved.value() == address);
  }
}

TEST_CASE("host names are not numeric") {
  CHECK_FALSE(IsNumericAddress("play.example.com:27015"));
  CHECK_FALSE(IsNumericAddress("localhost:27015"));
  CHECK_FALSE(IsNumericAddress("1.2.3.4"));
  CHECK_FALSE(IsNumericAddress("[::1]"));
}

TEST_CASE("localhost resolves to a numeric loopback address") {
  const auto resolved = ResolveAddress("localhost:27015");
  REQUIRE(resolved.ok());
  CHECK(IsNumericAddress(resolved.value()));
  CHECK(NetcodeAccepts(resolved.value()));
  const bool v4 = resolved.value() == "127.0.0.1:27015";
  const bool v6 = resolved.value() == "[::1]:27015";
  CHECK((v4 || v6));
}

TEST_CASE("malformed addresses give kMalformed") {
  for (const char* address :
       {"", "host", ":27015", "host:", "host:0", "host:70000", "host:abc",
        "host:-1", "[::1]", "[::1]27015", "[name]:27015", "::1:27015"}) {
    CAPTURE(address);
    const auto resolved = ResolveAddress(address);
    REQUIRE_FALSE(resolved.ok());
    CHECK(resolved.error() == ResolveError::kMalformed);
  }
}

TEST_CASE("an unknown name gives kNotFound") {
  const auto resolved = ResolveAddress("something.invalid:27015");
  REQUIRE_FALSE(resolved.ok());
  CHECK(resolved.error() == ResolveError::kNotFound);
}

TEST_CASE("ResolveErrorText names every error") {
  CHECK_FALSE(ResolveErrorText(ResolveError::kMalformed).empty());
  CHECK_FALSE(ResolveErrorText(ResolveError::kNotFound).empty());
}

TEST_CASE("PendingResolve yields the same result as ResolveAddress") {
  for (const char* address :
       {"localhost:27015", "something.invalid:27015", "host:0",
        "10.0.0.1:27015"}) {
    CAPTURE(address);
    PendingResolve pending{std::string(address)};
    std::optional<core::Result<std::string, ResolveError>> polled;
    for (int i = 0; i < kMaxPolls && !polled.has_value(); ++i) {
      polled = pending.Poll();
      if (!polled.has_value()) {
        std::this_thread::sleep_for(kPollSleep);
      }
    }
    REQUIRE(polled.has_value());
    const auto direct = ResolveAddress(address);
    CHECK(polled->ok() == direct.ok());
    if (direct.ok()) {
      CHECK(polled->value() == direct.value());
    } else {
      CHECK(polled->error() == direct.error());
    }
    // Same value on every call.
    const auto again = pending.Poll();
    REQUIRE(again.has_value());
    CHECK(again->ok() == polled->ok());
  }
}

TEST_CASE("destroying a PendingResolve right away is safe") {
  for (int i = 0; i < 20; ++i) {
    PendingResolve pending{"something.invalid:27015"};
  }
  // Give the abandoned lookups a moment to finish in the background.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

}  // namespace blue::net
