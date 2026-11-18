#include <doctest/doctest.h>

#include <optional>

#include "platform/window.h"

namespace blue::platform {
namespace {

TEST_CASE("ParseWindowPair reads two integers separated by a comma") {
  const std::optional<WindowPair> pair = ParseWindowPair("1920,-40");
  REQUIRE(pair.has_value());
  CHECK(pair->x == 1920);
  CHECK(pair->y == -40);
}

TEST_CASE("ParseWindowPair rejects anything else") {
  for (const char* text :
       {"", ",", "1920", "1920,", ",40", "1920x40", "19 20,40", "1920,40,1",
        "a,b", "1920,40 ", "99999999999,1"}) {
    CAPTURE(text);
    CHECK_FALSE(ParseWindowPair(text).has_value());
  }
}

}  // namespace
}  // namespace blue::platform
