#ifndef BLUE_ASSET_MODEL_IMPORTER_H_
#define BLUE_ASSET_MODEL_IMPORTER_H_

#include <string>
#include <vector>

#include "asset/mesh_data.h"
#include "core/result.h"
#include "material/material.h"

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
  // Replace imported materials by their sidecar .bmat files where one exists
  // (see material::SidecarMaterialPath()).
  bool use_material_overrides = true;
};

// A static model: one mesh with node transforms baked in, and the materials
// its submeshes index.
struct Model {
  MeshData mesh;
  std::vector<material::MaterialDesc> materials;
};

// Loads every triangle mesh in the file at `path` into one MeshData with node
// transforms baked in, split into one submesh per material (each material is
// one contiguous index range). Materials come from the file (glTF PBR, or
// MTL/Phong converted, see the .cc) with identical materials merged, then
// replaced by sidecar .bmat files when `options` allows. Meshes without a
// material share a neutral grey default. UVs (channel 0 into uv0, channel 1
// into uv1, else a copy of uv0) follow the top-left convention of
// asset::Vertex; tangents are generated for uv0
// (MikkTSpace-compatible); meshes without UVs get zero UVs and a fallback
// tangent basis. Meshes without normals are skipped with a warning. Textures
// are referenced, not decoded. Returns kFileNotFound if `path` is not a
// regular file, kParseFailed if the file cannot be parsed, kNoMeshes if
// nothing usable remains, and kTooLarge if the vertices would overflow
// Index. The engine is right-handed, Y-up, CCW front faces, which is also
// assimp's convention.
[[nodiscard]] core::Result<Model, ImportError> ImportModel(
    const std::string& path, const ImportOptions& options = {});

}  // namespace blue::asset

#endif  // BLUE_ASSET_MODEL_IMPORTER_H_
