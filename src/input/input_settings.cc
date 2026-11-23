#include "input/input_settings.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/text.h"
#include "input/action.h"
#include "input/bindings.h"
#include "platform/file_system.h"

namespace blue::input {
namespace {

using core::ParseBool;
using core::ParseFiniteFloat;
using core::Trim;

constexpr size_t kMaxSettingsFileSize = 1024 * 1024;
constexpr std::string_view kBindPrefix = "bind.";

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

// Which actions a settings file binds, indexed by Action.
using BoundInFile = std::array<bool, kActionCount>;

// Applies one "bind.<action> = <list>" line and marks the action in
// `bound_in_file`.
void ApplyBinding(InputBindings& bindings, std::string_view action_name,
                  std::string_view value, const std::string& path,
                  BoundInFile& bound_in_file) {
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
  bound_in_file[static_cast<size_t>(*action)] = true;
}

// A saved file lists every action that existed when it was written, so an
// action it does not bind is newer and keeps its default keys. A file
// binding of another action to one of those keys (F was push before it
// became interact) would make the key do both: the new action takes the
// key, unless the defaults themselves share it between the two (Space for
// jump and fly up). An action left without keys that way gets its defaults
// back (push moves to H).
void GiveDefaultKeysToNewActions(InputBindings& bindings,
                                 const BoundInFile& bound_in_file,
                                 const std::string& path) {
  const InputBindings defaults;
  for (const Action added : AllActions()) {
    if (bound_in_file[static_cast<size_t>(added)]) {
      continue;
    }
    for (const Binding& key : defaults.For(added)) {
      for (const Action other : AllActions()) {
        if (!bound_in_file[static_cast<size_t>(other)] ||
            !bindings.IsBound(other, key) || defaults.IsBound(other, key)) {
          continue;
        }
        std::vector<Binding> kept;
        for (const Binding& binding : bindings.For(other)) {
          if (binding != key) {
            kept.push_back(binding);
          }
        }
        if (kept.empty()) {
          const std::span<const Binding> fallback = defaults.For(other);
          kept.assign(fallback.begin(), fallback.end());
        }
        core::log::Info("{}: {} goes to the new action {}; {} is bound to {}",
                        path, FormatBinding(key), ActionName(added),
                        ActionName(other), FormatBindingList(kept));
        bindings.Set(other, std::move(kept));
      }
    }
  }
}

// Applies one "key = value" `line` read from `path` to `settings`, logging
// anything it cannot use.
void ApplyLine(InputSettings& settings, std::string_view line,
               const std::string& path, BoundInFile& bound_in_file) {
  const size_t equals = line.find('=');
  if (equals == std::string_view::npos) {
    core::log::Warn("{}: ignoring malformed line '{}'", path, line);
    return;
  }

  const std::string_view key = Trim(line.substr(0, equals));
  const std::string_view value = Trim(line.substr(equals + 1));

  if (key.starts_with(kBindPrefix)) {
    ApplyBinding(settings.bindings, key.substr(kBindPrefix.size()), value, path,
                 bound_in_file);
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

  BoundInFile bound_in_file{};
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
    ApplyLine(settings, line, path, bound_in_file);
  }
  GiveDefaultKeysToNewActions(settings.bindings, bound_in_file, path);
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
