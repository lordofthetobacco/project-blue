#ifndef BLUE_INPUT_INPUT_SETTINGS_H_
#define BLUE_INPUT_INPUT_SETTINGS_H_

#include <string>

#include "input/bindings.h"

namespace blue::input {

inline constexpr float kMinMouseSensitivity = 0.0002f;
inline constexpr float kMaxMouseSensitivity = 0.02f;
inline constexpr float kMinFovDegrees = 50.0f;
inline constexpr float kMaxFovDegrees = 120.0f;
inline constexpr float kMaxSprintFovBoostDegrees = 30.0f;

// User-tunable input and player-view options, persisted in a small
// "key = value" text file. Bindings are stored one line per action as
// "bind.<action> = <Key>, <Key>...".
struct InputSettings {
  float mouse_sensitivity = 0.0022f;  // radians per device count
  bool invert_y = false;
  bool sprint_toggle = false;  // false: sprint while held; true: press toggles
  bool crouch_toggle = false;  // likewise for crouch
  float fov_degrees = 70.0f;   // base vertical FOV of the player view
  float sprint_fov_boost_degrees = 10.0f;  // added while sprinting
  float head_bob_intensity = 0.5f;         // 0 = none, 1 = full
  InputBindings bindings;
};

// Loads the settings file at `path`. A missing file or bad values fall back to
// the defaults per key; malformed lines are logged and skipped. Numbers are
// clamped to their documented ranges. A "bind." line with an unknown action
// or an unparsable key is ignored; an empty value unbinds the action.
[[nodiscard]] InputSettings LoadInputSettings(const std::string& path);

// Writes `settings` to `path` through a temporary file that is renamed over
// it. Returns false (and logs) on failure.
[[nodiscard]] bool SaveInputSettings(const InputSettings& settings,
                                     const std::string& path);

}  // namespace blue::input

#endif  // BLUE_INPUT_INPUT_SETTINGS_H_
