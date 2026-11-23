#ifndef BLUE_WORLD_PREFAB_CACHE_H_
#define BLUE_WORLD_PREFAB_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/result.h"
#include "material/material.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "render/scene_view.h"
#include "render/texture_types.h"
#include "world/gpu_backend.h"

namespace blue::world {

// Handle of a prefab held by a PrefabCache. Stays valid for the cache's
// lifetime; Replace() keeps the id and swaps what it refers to.
enum class PrefabId : uint32_t {};

// GPU resources of one prefab, parallel to its vectors.
struct PrefabGpu {
  std::vector<render::MeshId> meshes;         // per prefab::Prefab::meshes
  std::vector<render::TextureId> textures;    // per textures
  std::vector<render::MaterialId> materials;  // per materials
  // Per mesh: its submeshes with the prefab materials resolved, ready for
  // render::DrawItem::submeshes.
  std::vector<std::vector<render::SubmeshDraw>> submeshes;
};

// Loads .pbo files once and keeps their CPU data (hierarchy, clips, sockets,
// lights, physics, children, CPU mesh spans for picking) together with their
// GPU resources. Paths are keyed after platform::AbsolutePath(), so the same
// file reached through different relative paths loads once. Borrows `gpu`,
// which must outlive the cache. Not thread-safe; neither copyable nor
// movable (World and PrefabPose keep references to it).
//
// Replace() and Release() hand the previous GPU resources back to the
// backend (freed once no frame in flight uses them). Re-uploading a document
// still costs a full upload, so parameter-only material changes go through
// UpdateMaterial(), which re-uploads nothing.
class PrefabCache {
 public:
  explicit PrefabCache(GpuBackend& gpu) : gpu_(gpu) {}
  PrefabCache(const PrefabCache&) = delete;
  PrefabCache& operator=(const PrefabCache&) = delete;

  // Returns the prefab at `path`, loading and uploading it on first use (the
  // load and upload times are logged).
  [[nodiscard]] core::Result<PrefabId, prefab::PrefabError> Get(
      std::string_view path);

  // Returns the id `path` is cached under, without loading.
  [[nodiscard]] std::optional<PrefabId> Find(std::string_view path) const;

  // Uploads an in-memory prefab (the editor's document) under `key` (a path,
  // or any name; nested prefabs that reference it by that path then use this
  // copy). `prefab` must satisfy prefab::ValidatePrefab(). If `key` is
  // already cached, that entry is replaced (see Replace()).
  PrefabId Upload(prefab::Prefab prefab, std::string_view key);

  // Replaces the prefab behind `id` and uploads it again. The previous GPU
  // resources and CPU copy are freed first (GpuBackend::FlushReleases()), so
  // two versions never take memory at once. Worlds holding `id` must be
  // rebuilt (World::Build()) before their next frame.
  void Replace(PrefabId id, prefab::Prefab prefab);

  // Points the CPU copy of `id` at the bulk data of `source`, an equal prefab
  // stored elsewhere (e.g. the editor document after saving, now a mapped
  // file; see prefab::RebaseBulk). Nothing is uploaded. Returns false and
  // changes nothing when `id` is released or the bulk arrays differ.
  bool RebaseCpu(PrefabId id, const prefab::Prefab& source);

  // Frees the GPU resources of `id` (and the override materials derived
  // from it) and forgets its key; the CPU prefab is dropped too. `id` must
  // not be used afterwards (World must be rebuilt without it); its slot is
  // not reused. No-op for an already released id.
  void Release(PrefabId id);
  // Returns true unless `id` was released.
  [[nodiscard]] bool alive(PrefabId id) const {
    return entries_[Index(id)].alive;
  }

  // Changes material `material` of `id` to `params` (its parameters only:
  // texture indices and UV sets are taken from `params` too, but textures
  // are not re-uploaded) and updates the GPU material in place. Derived
  // override materials of it are not updated (rebuild the world for that).
  void UpdateMaterial(PrefabId id, int material,
                      const prefab::Material& params);

  // The references returned by prefab() and gpu() stay valid while other
  // prefabs load; Replace() and Release() of the same id invalidate them.
  [[nodiscard]] const prefab::Prefab& prefab(PrefabId id) const {
    return entries_[Index(id)].prefab;
  }
  [[nodiscard]] const PrefabGpu& gpu(PrefabId id) const {
    return entries_[Index(id)].gpu;
  }
  // Returns the key (absolute path, or the Upload() key) of `id`.
  [[nodiscard]] const std::string& key(PrefabId id) const {
    return entries_[Index(id)].key;
  }

  // Returns a GPU material equal to material `material` of prefab `source`
  // with `desc_edit` applied to its description, created on first request
  // and shared by every equal request afterwards (scene overrides).
  // `edit_key` identifies the edit: equal keys must mean equal edits.
  template <typename Edit>
  render::MaterialId DerivedMaterial(PrefabId source, int material,
                                     const std::string& edit_key,
                                     const Edit& desc_edit) {
    const std::string full_key = DerivedKey(source, material, edit_key);
    if (auto found = derived_.find(full_key); found != derived_.end()) {
      return found->second;
    }
    const Entry& entry = entries_[Index(source)];
    const prefab::Material& base =
        entry.prefab.materials[static_cast<size_t>(material)];
    material::MaterialDesc desc = ToMaterialDesc(base, entry.prefab.textures);
    desc_edit(desc);
    const render::MaterialId id = gpu_.CreateMaterial(
        material::Sanitize(desc), ToMaterialTextures(base, entry.gpu.textures));
    derived_.emplace(full_key, id);
    return id;
  }

  [[nodiscard]] size_t size() const { return entries_.size(); }
  // Bytes of mesh and texture data uploaded and not released yet.
  [[nodiscard]] size_t gpu_bytes() const { return gpu_bytes_; }

 private:
  struct Entry {
    std::string key;
    prefab::Prefab prefab;
    PrefabGpu gpu;
    bool alive = true;
  };

  [[nodiscard]] static size_t Index(PrefabId id) {
    return static_cast<size_t>(id);
  }
  [[nodiscard]] static std::string DerivedKey(PrefabId source, int material,
                                              const std::string& edit_key);
  // Uploads `prefab`'s meshes, textures and materials.
  [[nodiscard]] PrefabGpu UploadGpu(const prefab::Prefab& prefab);
  // Releases `entry`'s GPU resources and the materials derived from `id`.
  void ReleaseGpu(PrefabId id, Entry& entry);

  GpuBackend& gpu_;
  // A deque: references to entries stay valid while new prefabs load.
  std::deque<Entry> entries_;
  std::unordered_map<std::string, PrefabId> by_key_;
  std::map<std::string, render::MaterialId> derived_;
  size_t gpu_bytes_ = 0;
};

}  // namespace blue::world

#endif  // BLUE_WORLD_PREFAB_CACHE_H_
