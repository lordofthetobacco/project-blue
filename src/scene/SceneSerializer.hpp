#pragma once

#include "scene/Scene.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>

namespace blue {

enum class SceneIoError {
    CannotOpen,
    WriteFailed,
    BadMagic,
    UnsupportedVersion,
    Truncated,
    ChecksumMismatch,
    Corrupt,
    InvalidScene,
};

[[nodiscard]] const char* toString(SceneIoError error);

inline constexpr uint32_t kSceneFormatVersion = 1;

// Mesh paths are stored relative to the scene file so a scene folder can be moved;
// loadScene resolves them back and returns them in generic ('/') form.
[[nodiscard]] std::expected<void, SceneIoError> saveScene(const Scene& scene, const std::filesystem::path& path);
[[nodiscard]] std::expected<Scene, SceneIoError> loadScene(const std::filesystem::path& path);

} // namespace blue
