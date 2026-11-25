#ifndef BLUE_NET_NET_SETTINGS_H_
#define BLUE_NET_NET_SETTINGS_H_

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "net/transport.h"

namespace blue::net {

// The development connect-token key the dedicated server shares
// (docs/netcode/protocol.md section 1): the 32 ASCII bytes
// "blue-dev-key-do-not-use-in-prod!". Minting tokens on the client is the
// development path until a token service exists.
inline constexpr std::array<uint8_t, kConnectTokenKeyBytes> kDevConnectKey = {
    'b', 'l', 'u', 'e', '-', 'd', 'e', 'v', '-', 'k', 'e',
    'y', '-', 'd', 'o', '-', 'n', 'o', 't', '-', 'u', 's',
    'e', '-', 'i', 'n', '-', 'p', 'r', 'o', 'd', '!'};

// The dedicated server's default UDP port.
inline constexpr uint16_t kDefaultServerPort = 27015;
inline constexpr std::string_view kDefaultServerAddress = "127.0.0.1:27015";
inline constexpr std::string_view kDefaultPlayerName = "player";
// File name of the settings, next to the input settings file.
inline constexpr std::string_view kNetSettingsFileName = "net.cfg";

// What the connect menu remembers between runs, persisted as a small
// "key = value" text file (like input::InputSettings).
struct NetSettings {
  std::string server_address = std::string(kDefaultServerAddress);
  std::string player_name = std::string(kDefaultPlayerName);
};

// Returns the path of the net settings file in the directory of
// `input_settings_path` (the input settings file).
[[nodiscard]] std::string NetSettingsPathNextTo(
    std::string_view input_settings_path);

// Loads the settings file at `path`. A missing file or bad values fall back
// to the defaults per key; malformed lines are logged and skipped.
[[nodiscard]] NetSettings LoadNetSettings(const std::string& path);

// Writes `settings` to `path` through a temporary file renamed over it.
// Returns false (and logs) on failure.
[[nodiscard]] bool SaveNetSettings(const NetSettings& settings,
                                   const std::string& path);

// Returns `name` trimmed, cut to at most kNameMaxBytes bytes on a UTF-8
// character boundary, with control characters dropped.
[[nodiscard]] std::string SanitizePlayerName(std::string_view name);

}  // namespace blue::net

#endif  // BLUE_NET_NET_SETTINGS_H_
