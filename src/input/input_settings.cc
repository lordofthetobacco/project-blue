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
#include <vector>

#include "core/log.h"
#include "input/action.h"
#include "input/bindings.h"
#include "platform/file_system.h"

namespace blue::input {
namespace {

constexpr size_t kMaxSettingsFileSize = 1024 * 1024;
constexpr std::string_view kBindPrefix = "bind.";

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

// Stores the float in `value` into `target`, clamped to [min, max], or logs
// why it cannot.
void ApplyFloat(float& target, std::string_view key, std::string_view value,
                float min, float max, const std::string& path) {
  if (const std::optional<float> parsed = ParseFiniteFloat(value)) {
    target = std::clamp(*parsed, min, max);
  } else {
    core::log::Warn("{}: invalid {} '{}'", path, key, value);
  }
}

void ApplyBool(bool& target, std::string_view key, std::string_view value,
               const std::string& path) {
  if (const std::optional<bool> parsed = ParseBool(value)) {
    target = *parsed;
  } else {
    core::log::Warn("{}: invalid {} '{}'", path, key, value);
  }
}

// Applies one "bind.<action> = <list>" line.
void ApplyBinding(InputBindings& bindings, std::string_view action_name,
                  std::string_view value, const std::string& path) {
  const std::optional<Action> action = ParseAction(action_name);
  if (!action) {
    core::log::Warn("{}: unknown action '{}'", path, action_name);
    return;
  }
  std::optional<std::vector<Binding>> list = ParseBindingList(value);
  if (!list) {
    core::log::Warn("{}: invalid binding list for {} '{}'", path, action_name,
                    value);
    return;
  }
  bindings.Set(*action, std::move(*list));
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

  if (key.starts_with(kBindPrefix)) {
    ApplyBinding(settings.bindings, key.substr(kBindPrefix.size()), value,
                 path);
  } else if (key == "mouse_sensitivity") {
    ApplyFloat(settings.mouse_sensitivity, key, value, kMinMouseSensitivity,
               kMaxMouseSensitivity, path);
  } else if (key == "invert_y") {
    ApplyBool(settings.invert_y, key, value, path);
  } else if (key == "sprint_toggle") {
    ApplyBool(settings.sprint_toggle, key, value, path);
  } else if (key == "crouch_toggle") {
    ApplyBool(settings.crouch_toggle, key, value, path);
  } else if (key == "fov_degrees") {
    ApplyFloat(settings.fov_degrees, key, value, kMinFovDegrees,
               kMaxFovDegrees, path);
  } else if (key == "sprint_fov_boost_degrees") {
    ApplyFloat(settings.sprint_fov_boost_degrees, key, value, 0.0f,
               kMaxSprintFovBoostDegrees, path);
  } else if (key == "head_bob_intensity") {
    ApplyFloat(settings.head_bob_intensity, key, value, 0.0f, 1.0f, path);
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
  std::string text = std::format(
      "# Project Blue input settings\n"
      "mouse_sensitivity = {}\n"
      "invert_y = {}\n"
      "sprint_toggle = {}\n"
      "crouch_toggle = {}\n"
      "fov_degrees = {}\n"
      "sprint_fov_boost_degrees = {}\n"
      "head_bob_intensity = {}\n"
      "\n"
      "# Key bindings: SDL key names or Mouse1..Mouse5, comma separated.\n"
      "# An empty value unbinds the action.\n",
      settings.mouse_sensitivity, settings.invert_y, settings.sprint_toggle,
      settings.crouch_toggle, settings.fov_degrees,
      settings.sprint_fov_boost_degrees, settings.head_bob_intensity);
  for (const Action action : AllActions()) {
    text += std::format("{}{} = {}\n", kBindPrefix, ActionName(action),
                        FormatBindingList(settings.bindings.For(action)));
  }
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
