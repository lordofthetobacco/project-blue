#include "world/prefab_cache.h"

#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"
#include "platform/file_system.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "render/scene_view.h"

namespace blue::world {
namespace {

using GetResult = core::Result<PrefabId, prefab::PrefabError>;

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

// Returns the bytes of mesh and texture data `prefab` uploads.
size_t UploadBytes(const prefab::Prefab& prefab) {
  size_t bytes = 0;
  for (const prefab::Mesh& mesh : prefab.meshes) {
    bytes += mesh.vertices.size_bytes() + mesh.indices.size_bytes();
  }
  for (const prefab::Texture& texture : prefab.textures) {
    for (const prefab::TextureMip& mip : texture.mips) {
      bytes += mip.data.size();
    }
  }
  return bytes;
}

// Returns the cache key of `path`: an absolute, normalised path.
std::string KeyOf(std::string_view path) {
  return platform::AbsolutePath(path);
}

}  // namespace

GetResult PrefabCache::Get(std::string_view path) {
  std::string key = KeyOf(path);
  if (auto found = by_key_.find(key); found != by_key_.end()) {
    return GetResult::Ok(found->second);
  }
  const auto start = std::chrono::steady_clock::now();
  auto loaded = prefab::LoadPrefab(key);
  if (!loaded.ok()) {
    core::log::Warn("Prefab {}: {}", key, prefab::ToString(loaded.error()));
    return GetResult::Error(loaded.error());
  }
  const double load_ms = MillisecondsSince(start);
  const PrefabId id = Upload(std::move(loaded).value(), key);
  core::log::Info(
      "Prefab {} ready in {:.1f} ms (load {:.1f} ms, upload "
      "{:.1f} ms)",
      key, MillisecondsSince(start), load_ms,
      MillisecondsSince(start) - load_ms);
  return GetResult::Ok(id);
}

std::optional<PrefabId> PrefabCache::Find(std::string_view path) const {
  if (auto found = by_key_.find(KeyOf(path)); found != by_key_.end()) {
    return found->second;
  }
  if (auto found = by_key_.find(std::string(path)); found != by_key_.end()) {
    return found->second;
  }
  return std::nullopt;
}

PrefabId PrefabCache::Upload(prefab::Prefab prefab, std::string_view key) {
  // Keys that look like paths are normalised so nested references match.
  std::string stored_key =
      key.empty() ? std::format("<memory {}>", entries_.size()) : KeyOf(key);
  if (auto found = by_key_.find(stored_key); found != by_key_.end()) {
    Replace(found->second, std::move(prefab));
    return found->second;
  }
  const auto id = static_cast<PrefabId>(entries_.size());
  Entry entry{.key = stored_key, .prefab = std::move(prefab)};
  entry.gpu = UploadGpu(entry.prefab);
  entries_.push_back(std::move(entry));
  by_key_.emplace(std::move(stored_key), id);
  return id;
}

void PrefabCache::Replace(PrefabId id, prefab::Prefab prefab) {
  Entry& entry = entries_[Index(id)];
  if (entry.alive) {
    ReleaseGpu(id, entry);
  } else {
    entry.alive = true;
    by_key_.emplace(entry.key, id);
  }
  entry.prefab = std::move(prefab);
  entry.gpu = UploadGpu(entry.prefab);
}

void PrefabCache::Release(PrefabId id) {
  Entry& entry = entries_[Index(id)];
  if (!entry.alive) {
    return;
  }
  ReleaseGpu(id, entry);
  by_key_.erase(entry.key);
  entry.prefab = prefab::Prefab();
  entry.alive = false;
}

void PrefabCache::UpdateMaterial(PrefabId id, int material,
                                 const prefab::Material& params) {
  Entry& entry = entries_[Index(id)];
  if (!entry.alive || material < 0 ||
      static_cast<size_t>(material) >= entry.prefab.materials.size()) {
    return;
  }
  const auto index = static_cast<size_t>(material);
  entry.prefab.materials[index] = params;
  gpu_.UpdateMaterial(
      entry.gpu.materials[index],
      material::Sanitize(ToMaterialDesc(params, entry.prefab.textures)));
}

std::string PrefabCache::DerivedKey(PrefabId source, int material,
                                    const std::string& edit_key) {
  return std::format("{}/{}/{}", static_cast<uint32_t>(source), material,
                     edit_key);
}

PrefabGpu PrefabCache::UploadGpu(const prefab::Prefab& prefab) {
  PrefabGpu gpu;
  gpu.textures = gpu_.UploadTextures(prefab.textures);
  gpu.materials.reserve(prefab.materials.size());
  for (const prefab::Material& material : prefab.materials) {
    gpu.materials.push_back(gpu_.CreateMaterial(
        material::Sanitize(ToMaterialDesc(material, prefab.textures)),
        ToMaterialTextures(material, gpu.textures)));
  }
  gpu.meshes.reserve(prefab.meshes.size());
  gpu.submeshes.reserve(prefab.meshes.size());
  for (const prefab::Mesh& mesh : prefab.meshes) {
    gpu.meshes.push_back(gpu_.UploadMesh(mesh));
    std::vector<render::SubmeshDraw>& draws = gpu.submeshes.emplace_back();
    draws.reserve(mesh.submeshes.size());
    for (const asset::Submesh& submesh : mesh.submeshes) {
      draws.push_back(
          {.first_index = submesh.first_index,
           .index_count = submesh.index_count,
           .material = gpu.materials[static_cast<size_t>(submesh.material)]});
    }
  }
  gpu_bytes_ += UploadBytes(prefab);
  return gpu;
}

void PrefabCache::ReleaseGpu(PrefabId id, Entry& entry) {
  // Derived materials sample this prefab's textures: release them first.
  const std::string prefix = std::format("{}/", static_cast<uint32_t>(id));
  for (auto it = derived_.lower_bound(prefix);
       it != derived_.end() && it->first.starts_with(prefix);) {
    gpu_.ReleaseMaterial(it->second);
    it = derived_.erase(it);
  }
  for (const render::MaterialId material : entry.gpu.materials) {
    gpu_.ReleaseMaterial(material);
  }
  for (const render::MeshId mesh : entry.gpu.meshes) {
    gpu_.ReleaseMesh(mesh);
  }
  gpu_.ReleaseTextures(entry.gpu.textures);
  gpu_bytes_ -= UploadBytes(entry.prefab);
  entry.gpu = PrefabGpu();
}

}  // namespace blue::world
