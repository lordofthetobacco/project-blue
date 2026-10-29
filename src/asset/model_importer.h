#ifndef BLUE_ASSET_MODEL_IMPORTER_H_
#define BLUE_ASSET_MODEL_IMPORTER_H_

#include <string>

#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "core/result.h"

namespace blue::asset {

enum class ImportError {
  kFileNotFound,  // The path is not a regular file.
  kParseFailed,   // The importer could not parse the file.
  kNoMeshes,      // The file holds no usable triangle meshes.
  kTooLarge,      // The vertex count does not fit in an Index.
};

// Returns a short human-readable name for `error`, for log messages.
[[nodiscard]] const char* ToString(ImportError error);

struct ImportOptions {
  // Used for meshes without a material of their own.
  glm::vec3 default_color{0.8f};
};

// Loads every triangle mesh in the file at `path` into one MeshData with node
// transforms baked in. Meshes without normals are skipped with a warning.
// Returns kFileNotFound if `path` is not a regular file, kParseFailed if the
// file cannot be parsed, kNoMeshes if nothing usable remains, and kTooLarge if
// the vertices would overflow Index. The engine is right-handed, Y-up, CCW
// front faces, which is also assimp's convention.
[[nodiscard]] core::Result<MeshData, ImportError> ImportModel(
    const std::string& path, const ImportOptions& options = {});

}  // namespace blue::asset

#endif  // BLUE_ASSET_MODEL_IMPORTER_H_
