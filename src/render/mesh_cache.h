#ifndef BLUE_RENDER_MESH_CACHE_H_
#define BLUE_RENDER_MESH_CACHE_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "asset/mesh_data.h"
#include "render/renderer.h"
#include "render/scene_view.h"

namespace blue::render {

struct CachedMesh {
  MeshId id;
  asset::Bounds bounds;
  // The CPU copy of the mesh, e.g. for building physics shapes. Owned by the
  // MeshCache that returned it and valid until that cache is destroyed.
  const asset::MeshData* cpu_mesh = nullptr;
  // One entry per submesh of `cpu_mesh`, for DrawItem::submeshes. Owned by
  // the cache, like `cpu_mesh`.
  std::span<const SubmeshDraw> submeshes;
  // The mesh's own materials (its model's material table, indexed like
  // asset::Submesh::material). Owned by the cache.
  std::span<const MaterialId> materials;
};

// Returns one SubmeshDraw per element of `submeshes`, with each model-local
// material index resolved through `materials` (kDefaultMaterial when out of
// range).
[[nodiscard]] std::vector<SubmeshDraw> ResolveSubmeshes(
    std::span<const asset::Submesh> submeshes,
    std::span<const MaterialId> materials);

// Resolves scene::Entity::mesh_path to an uploaded mesh with its materials,
// importing each source once (the model's materials are created once, its
// textures loaded in one batch). Builtin meshes ("builtin:cube", "plane",
// "sphere") get a material of their own. Failures are remembered so a broken
// path is reported once rather than re-parsed every frame. The CPU mesh data
// of every loaded mesh is kept for the cache's lifetime at a stable address
// (CachedMesh::cpu_mesh). Borrows `renderer`, which must outlive the cache.
// Not thread-safe.
class MeshCache {
 public:
  explicit MeshCache(Renderer& renderer) : renderer_(renderer) {}

  MeshCache(const MeshCache&) = delete;
  MeshCache& operator=(const MeshCache&) = delete;

  // Returns the mesh for `mesh_path`, loading it on first use, or nullopt if
  // loading failed (the failure was logged).
  [[nodiscard]] std::optional<CachedMesh> Get(std::string_view mesh_path);

 private:
  struct StringHash {
    using is_transparent = void;
    [[nodiscard]] size_t operator()(std::string_view text) const {
      return std::hash<std::string_view>{}(text);
    }
  };

  // Everything the cache keeps of one loaded mesh.
  struct Stored {
    asset::MeshData data;
    std::vector<MaterialId> materials;
    std::vector<SubmeshDraw> submeshes;
  };

  using Entries = std::unordered_map<std::string, std::optional<CachedMesh>,
                                     StringHash, std::equal_to<>>;

  // Creates `materials`, keeps `data` for the cache's lifetime and uploads
  // it.
  [[nodiscard]] CachedMesh Store(
      asset::MeshData data, std::span<const material::MaterialDesc> materials);
  [[nodiscard]] std::optional<CachedMesh> Load(std::string_view mesh_path,
                                               const std::string& key);

  Renderer& renderer_;
  // Owners of the CachedMesh pointers and spans; declared before the maps
  // that point into them.
  std::vector<std::unique_ptr<Stored>> stored_;
  // In both maps nullopt marks a failed load.
  // Normalized key: one entry per distinct source.
  Entries by_key_;
  // Paths exactly as entities spell them: allocation- and filesystem-free once
  // warm.
  Entries by_path_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_MESH_CACHE_H_
