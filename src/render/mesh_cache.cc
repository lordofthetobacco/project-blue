#include "render/mesh_cache.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "asset/model_importer.h"
#include "core/log.h"
#include "material/material.h"
#include "platform/file_system.h"
#include "scene/scene.h"

namespace blue::render {

namespace {

constexpr float kBuiltinPlaneSize = 12.0f;
constexpr int kBuiltinSphereSegments = 64;

// Returns a plain dielectric material named `name`.
material::MaterialDesc SolidMaterial(const char* name, glm::vec3 base_color,
                                     float roughness) {
  material::MaterialDesc desc;
  desc.name = name;
  desc.base_color = glm::vec4(base_color, 1.0f);
  desc.roughness = roughness;
  return desc;
}

// The colours the builtins had as vertex colours before materials existed.
material::MaterialDesc CubeMaterial() {
  return SolidMaterial("builtin cube", {0.42f, 0.45f, 0.30f}, 0.8f);  // Olive.
}
material::MaterialDesc PlaneMaterial() {
  return SolidMaterial("builtin plane", {0.10f, 0.13f, 0.10f}, 0.9f);
}
material::MaterialDesc SphereMaterial() {
  return SolidMaterial("builtin sphere", {0.5f, 0.5f, 0.5f}, 0.5f);
}

// Returns an absolute, normalized, '/'-separated path: spellings of one file
// share a cache entry. The key is lexical, so symlinks are not resolved and
// two links to one file get separate entries.
[[nodiscard]] std::string FileKey(std::string_view mesh_path) {
  return platform::AbsolutePath(mesh_path);
}

}  // namespace

std::vector<SubmeshDraw> ResolveSubmeshes(
    std::span<const asset::Submesh> submeshes,
    std::span<const MaterialId> materials) {
  std::vector<SubmeshDraw> draws;
  draws.reserve(submeshes.size());
  for (const asset::Submesh& submesh : submeshes) {
    const bool known = submesh.material >= 0 &&
                       static_cast<size_t>(submesh.material) < materials.size();
    draws.push_back({
        .first_index = submesh.first_index,
        .index_count = submesh.index_count,
        .material = known ? materials[static_cast<size_t>(submesh.material)]
                          : kDefaultMaterial,
    });
  }
  return draws;
}

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
      const material::MaterialDesc material = CubeMaterial();
      return Store(asset::MakeCube(), {&material, 1});
    }
    if (mesh_path == "builtin:plane") {
      const material::MaterialDesc material = PlaneMaterial();
      return Store(asset::MakePlane(kBuiltinPlaneSize), {&material, 1});
    }
    if (mesh_path == "builtin:sphere") {
      const material::MaterialDesc material = SphereMaterial();
      return Store(asset::MakeSphere(kBuiltinSphereSegments), {&material, 1});
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
  asset::Model model = std::move(imported).value();
  return Store(std::move(model.mesh), model.materials);
}

CachedMesh MeshCache::Store(asset::MeshData data,
                            std::span<const material::MaterialDesc> materials) {
  auto stored = std::make_unique<Stored>();
  stored->data = std::move(data);
  renderer_.PreloadTextures(materials);
  for (const material::MaterialDesc& material : materials) {
    stored->materials.push_back(renderer_.CreateMaterial(material));
  }
  stored->submeshes =
      ResolveSubmeshes(stored->data.submeshes, stored->materials);
  stored_.push_back(std::move(stored));

  const Stored& kept = *stored_.back();
  return {.id = renderer_.UploadMesh(kept.data),
          .bounds = kept.data.bounds,
          .cpu_mesh = &kept.data,
          .submeshes = kept.submeshes,
          .materials = kept.materials};
}

}  // namespace blue::render
