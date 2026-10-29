#include "input/input_settings.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/log.h"
#include "platform/file_system.h"

namespace blue::input {
namespace {

constexpr size_t kMaxSettingsFileSize = 1024 * 1024;

// Returns `text` without leading and trailing whitespace.
std::string_view Trim(std::string_view text) {
  const auto is_space = [](char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
  };
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

// Returns the float spelled by all of `text`, or nullopt if it is not a
// number or is not finite.
std::optional<float> ParseFiniteFloat(std::string_view text) {
  float value = 0.0f;
  const auto [end, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size() ||
      !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

// Returns the boolean spelled by `text` ("true"/"false"/"1"/"0", any case), or
// nullopt for anything else.
std::optional<bool> ParseBool(std::string_view text) {
  std::string lower(text);
  std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (lower == "true" || lower == "1") {
    return true;
  }
  if (lower == "false" || lower == "0") {
    return false;
  }
  return std::nullopt;
}

// Applies one "key = value" `line` read from `path` to `settings`, logging
// anything it cannot use.
void ApplyLine(InputSettings& settings, std::string_view line,
               const std::string& path) {
  const size_t equals = line.find('=');
  if (equals == std::string_view::npos) {
    core::log::Warn("{}: ignoring malformed line '{}'", path, line);
    return;
  }

  const std::string_view key = Trim(line.substr(0, equals));
  const std::string_view value = Trim(line.substr(equals + 1));

  if (key == "mouse_sensitivity") {
    if (const std::optional<float> parsed = ParseFiniteFloat(value)) {
      settings.mouse_sensitivity =
          std::clamp(*parsed, kMinMouseSensitivity, kMaxMouseSensitivity);
    } else {
      core::log::Warn("{}: invalid mouse_sensitivity '{}'", path, value);
    }
  } else if (key == "invert_y") {
    if (const std::optional<bool> parsed = ParseBool(value)) {
      settings.invert_y = *parsed;
    } else {
      core::log::Warn("{}: invalid invert_y '{}'", path, value);
    }
  } else {
    core::log::Warn("{}: unknown key '{}'", path, key);
  }
}

}  // namespace

InputSettings LoadInputSettings(const std::string& path) {
  InputSettings settings;

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
    const std::string_view line =
        Trim(std::string_view(text).substr(line_start, line_end - line_start));
    line_start = line_end + 1;
    if (line.empty() || line.front() == '#') {
      continue;
    }
    ApplyLine(settings, line, path);
  }
  return settings;
}

bool SaveInputSettings(const InputSettings& settings, const std::string& path) {
  const std::string text = std::format(
      "# Project Blue input settings\nmouse_sensitivity = {}\ninvert_y = {}\n",
      settings.mouse_sensitivity, settings.invert_y);
  const auto written =
      platform::WriteFileAtomic(path, std::as_bytes(std::span(text)));
  if (!written.ok()) {
    core::log::Warn("cannot write settings file '{}': {}", path,
                    platform::ToString(written.error()));
    return false;
  }
  return true;
}

}  // namespace blue::input
