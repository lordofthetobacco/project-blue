#include "net/net_settings.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>

#include "core/log.h"
#include "core/path.h"
#include "core/text.h"
#include "net/protocol.h"
#include "platform/file_system.h"

namespace blue::net {
namespace {

constexpr size_t kMaxSettingsFileSize = 64 * 1024;
// Longest server address accepted from the file.
constexpr size_t kMaxAddressBytes = 255;

// Applies one "key = value" `line` read from `path` to `settings`.
void ApplyLine(NetSettings& settings, std::string_view line,
               const std::string& path) {
  const size_t equals = line.find('=');
  if (equals == std::string_view::npos) {
    core::log::Warn("{}: ignoring malformed line '{}'", path, line);
    return;
  }
  const std::string_view key = core::Trim(line.substr(0, equals));
  const std::string_view value = core::Trim(line.substr(equals + 1));
  if (key == "server_address") {
    if (value.empty() || value.size() > kMaxAddressBytes) {
      core::log::Warn("{}: invalid server_address '{}'", path, value);
      return;
    }
    settings.server_address = std::string(value);
  } else if (key == "player_name") {
    std::string name = SanitizePlayerName(value);
    if (name.empty()) {
      core::log::Warn("{}: invalid player_name '{}'", path, value);
      return;
    }
    settings.player_name = std::move(name);
  } else {
    core::log::Warn("{}: unknown key '{}'", path, key);
  }
}

// Returns the length of the UTF-8 sequence starting with `lead`, or 0 for a
// byte that cannot start one.
size_t Utf8Length(uint8_t lead) {
  if (lead < 0x80) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 0;
}

}  // namespace

std::string NetSettingsPathNextTo(std::string_view input_settings_path) {
  return core::JoinPath(core::ParentPath(input_settings_path),
                        kNetSettingsFileName);
}

std::string SanitizePlayerName(std::string_view name) {
  const std::string_view trimmed = core::Trim(name);
  std::string result;
  result.reserve(kNameMaxBytes);
  size_t at = 0;
  while (at < trimmed.size()) {
    const auto lead = static_cast<uint8_t>(trimmed[at]);
    const size_t length = Utf8Length(lead);
    if (length == 0 || at + length > trimmed.size()) {
      ++at;  // A stray byte: not UTF-8.
      continue;
    }
    bool valid = true;
    for (size_t i = 1; i < length; ++i) {
      valid = valid && (static_cast<uint8_t>(trimmed[at + i]) & 0xC0) == 0x80;
    }
    const bool control = length == 1 && (lead < 0x20 || lead == 0x7F);
    if (valid && !control) {
      if (result.size() + length > kNameMaxBytes) {
        break;
      }
      result.append(trimmed.substr(at, length));
    }
    at += valid ? length : 1;
  }
  return std::string(core::Trim(result));
}

NetSettings LoadNetSettings(const std::string& path) {
  NetSettings settings;
  const auto bytes = platform::ReadFile(path, kMaxSettingsFileSize);
  if (!bytes.ok()) {
    return settings;
  }
  std::string text;
  text.reserve(bytes.value().size());
  for (const std::byte b : bytes.value()) {
    text.push_back(static_cast<char>(b));
  }
  size_t line_start = 0;
  while (line_start < text.size()) {
    size_t line_end = text.find('\n', line_start);
    if (line_end == std::string::npos) {
      line_end = text.size();
    }
    const std::string_view line = core::Trim(
        std::string_view(text).substr(line_start, line_end - line_start));
    line_start = line_end + 1;
    if (line.empty() || line.front() == '#') {
      continue;
    }
    ApplyLine(settings, line, path);
  }
  return settings;
}

bool SaveNetSettings(const NetSettings& settings, const std::string& path) {
  const std::string text = std::format(
      "# Project Blue multiplayer settings\n"
      "server_address = {}\n"
      "player_name = {}\n",
      settings.server_address, settings.player_name);
  const auto written =
      platform::WriteFileAtomic(path, std::as_bytes(std::span(text)));
  if (!written.ok()) {
    core::log::Warn("cannot write net settings file '{}': {}", path,
                    platform::ToString(written.error()));
    return false;
  }
  return true;
}

}  // namespace blue::net
