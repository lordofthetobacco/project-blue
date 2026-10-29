#pragma once

#include <filesystem>

namespace blue {

inline constexpr float kMinMouseSensitivity = 0.0002f;
inline constexpr float kMaxMouseSensitivity = 0.02f;

struct InputSettings {
    float mouseSensitivity = 0.0022f; // radians per device count
    bool invertY = false;
};

// Missing file or bad values fall back to defaults per key; bad lines are logged.
[[nodiscard]] InputSettings loadInputSettings(const std::filesystem::path& path);

// Writes to a temporary file and renames it over `path`. Returns false (and logs) on failure.
[[nodiscard]] bool saveInputSettings(const InputSettings& settings, const std::filesystem::path& path);

} // namespace blue
