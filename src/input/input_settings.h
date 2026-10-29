#ifndef BLUE_INPUT_INPUT_SETTINGS_H_
#define BLUE_INPUT_INPUT_SETTINGS_H_

#include <string>

namespace blue::input {

inline constexpr float kMinMouseSensitivity = 0.0002f;
inline constexpr float kMaxMouseSensitivity = 0.02f;

// User-tunable input options, persisted in a small key = value text file.
struct InputSettings {
  float mouse_sensitivity = 0.0022f;  // radians per device count
  bool invert_y = false;
};

// Loads the settings file at `path`. A missing file or bad values fall back to
// the defaults per key; malformed lines are logged and skipped. Sensitivity is
// clamped to [kMinMouseSensitivity, kMaxMouseSensitivity].
[[nodiscard]] InputSettings LoadInputSettings(const std::string& path);

// Writes `settings` to `path` through a temporary file that is renamed over
// it. Returns false (and logs) on failure.
[[nodiscard]] bool SaveInputSettings(const InputSettings& settings,
                                     const std::string& path);

}  // namespace blue::input

#endif  // BLUE_INPUT_INPUT_SETTINGS_H_
