#include "render/mesh_cache.h"

#include <string>
#include <utility>

#include <glm/vec3.hpp>

#include "asset/model_importer.h"
#include "core/log.h"
#include "platform/file_system.h"
#include "scene/scene.h"

namespace blue::render {

namespace {

constexpr glm::vec3 kCubeColor{0.42f, 0.45f, 0.30f};   // Olive drab, linear.
constexpr glm::vec3 kPlaneColor{0.10f, 0.13f, 0.10f};  // Dark grey-green.
constexpr float kBuiltinPlaneSize = 12.0f;

// Returns an absolute, normalized, '/'-separated path: spellings of one file
// share a cache entry. The key is lexical, so symlinks are not resolved and
// two links to one file get separate entries.
[[nodiscard]] std::string FileKey(std::string_view mesh_path) {
  return platform::AbsolutePath(mesh_path);
}

}  // namespace

std::optional<CachedMesh> MeshCache::Get(std::string_view mesh_path) {
  if (const auto found = by_path_.find(mesh_path); found != by_path_.end()) {
    return found->second;
  }

  const bool builtin = mesh_path.starts_with(scene::kBuiltinMeshPrefix);
  std::string key = builtin ? std::string(mesh_path) : FileKey(mesh_path);

  auto entry = by_key_.find(key);
  if (entry == by_key_.end()) {
    std::optional<CachedMesh> loaded = Load(mesh_path, key);
    entry = by_key_.emplace(std::move(key), loaded).first;
  }
  return by_path_.emplace(std::string(mesh_path), entry->second).first->second;
}

std::optional<CachedMesh> MeshCache::Load(std::string_view mesh_path,
                                          const std::string& key) {
  if (mesh_path.starts_with(scene::kBuiltinMeshPrefix)) {
    if (mesh_path == "builtin:cube") {
      return Store(asset::MakeCube(kCubeColor));
    }
    if (mesh_path == "builtin:plane") {
      return Store(asset::MakePlane(kBuiltinPlaneSize, kPlaneColor));
    }
    core::log::Error("Unknown builtin mesh '{}'", mesh_path);
    return std::nullopt;
  }

  auto imported = asset::ImportModel(key);
  if (!imported.ok()) {
    core::log::Error("Failed to import '{}': {}", mesh_path,
                     asset::ToString(imported.error()));
    return std::nullopt;
  }
  return Store(std::move(imported).value());
}

CachedMesh MeshCache::Store(asset::MeshData data) {
  cpu_meshes_.push_back(std::make_unique<asset::MeshData>(std::move(data)));
  const asset::MeshData& stored = *cpu_meshes_.back();
  return {.id = renderer_.UploadMesh(stored),
          .bounds = stored.bounds,
          .cpu_mesh = &stored};
}

}  // namespace blue::render
