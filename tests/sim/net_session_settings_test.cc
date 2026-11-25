#include <doctest/doctest.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "net/net_settings.h"
#include "net/protocol.h"
#include "platform/file_system.h"
#include "tests/scratch_directory.h"

namespace blue::net {
namespace {

TEST_CASE("the dev connect key is the server's") {
  const std::string_view key(
      reinterpret_cast<const char*>(kDevConnectKey.data()),
      kDevConnectKey.size());
  CHECK(key == "blue-dev-key-do-not-use-in-prod!");
  CHECK(kNetcodeProtocolId == 0x424C55455F4D5031ULL);
}

TEST_CASE("player names are sanitised") {
  CHECK(SanitizePlayerName("  alice  ") == "alice");
  CHECK(SanitizePlayerName("a\tb\x01"
                           "c") == "abc");
  CHECK(SanitizePlayerName(std::string(40, 'x')) == std::string(32, 'x'));
  // A two-byte character that would straddle the limit is left out.
  const std::string almost = std::string(31, 'y') + "\xC3\xA9";
  CHECK(SanitizePlayerName(almost) == std::string(31, 'y'));
  CHECK(SanitizePlayerName("\xFF\xFEok") == "ok");
  CHECK(SanitizePlayerName("   ").empty());
}

TEST_CASE("net settings live next to the input settings") {
  CHECK(NetSettingsPathNextTo("C:/prefs/ProjectBlue/input.cfg") ==
        "C:/prefs/ProjectBlue/net.cfg");
  CHECK(NetSettingsPathNextTo("input.cfg") == "net.cfg");
}

TEST_CASE("net settings round-trip and fall back per key") {
  blue::testing::ScratchDirectory dir("net_settings");
  const std::string path = dir.Child("net.cfg");
  const NetSettings missing = LoadNetSettings(path);
  CHECK(missing.server_address == kDefaultServerAddress);
  CHECK(missing.player_name == kDefaultPlayerName);

  const NetSettings saved{.server_address = "10.0.0.5:27015",
                          .player_name = "bob"};
  REQUIRE(SaveNetSettings(saved, path));
  const NetSettings loaded = LoadNetSettings(path);
  CHECK(loaded.server_address == saved.server_address);
  CHECK(loaded.player_name == saved.player_name);

  const std::string text =
      "# comment\nserver_address =\nplayer_name = \x01\x02\nbogus\nx = 1\n";
  REQUIRE(platform::WriteFileAtomic(path, std::as_bytes(std::span(text))).ok());
  const NetSettings fallback = LoadNetSettings(path);
  CHECK(fallback.server_address == kDefaultServerAddress);
  CHECK(fallback.player_name == kDefaultPlayerName);
}

}  // namespace
}  // namespace blue::net
