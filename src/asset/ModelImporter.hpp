#pragma once

#include "asset/MeshData.hpp"

#include <glm/vec3.hpp>

#include <expected>
#include <filesystem>

namespace blue {

enum class ImportError { FileNotFound, ParseFailed, NoMeshes, TooLarge };

[[nodiscard]] const char* toString(ImportError error);

struct ImportOptions {
    // Used for meshes without a material of their own.
    glm::vec3 defaultColor{0.8f};
};

// Loads every triangle mesh in the file into one MeshData with node transforms baked in.
// The engine is right-handed, Y-up, CCW front faces, which is also assimp's convention.
[[nodiscard]] std::expected<MeshData, ImportError> importModel(const std::filesystem::path& path,
                                                              const ImportOptions& options = {});

} // namespace blue
