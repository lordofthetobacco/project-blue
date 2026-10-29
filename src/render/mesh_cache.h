#ifndef BLUE_RENDER_MESH_CACHE_H_
#define BLUE_RENDER_MESH_CACHE_H_

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "asset/mesh_data.h"
#include "render/renderer.h"

namespace blue::render {

struct CachedMesh {
  MeshId id;
  asset::Bounds bounds;
};

// Resolves scene::Entity::mesh_path to an uploaded mesh, importing each
// source once. Failures are remembered so a broken path is reported once
// rather than re-parsed every frame. Borrows `renderer`, which must outlive
// the cache. Not thread-safe.
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

  using Entries = std::unordered_map<std::string, std::optional<CachedMesh>,
                                     StringHash, std::equal_to<>>;

  [[nodiscard]] std::optional<CachedMesh> Load(std::string_view mesh_path,
                                               const std::string& key);

  Renderer& renderer_;
  // In both maps nullopt marks a failed load.
  // Normalized key: one entry per distinct source.
  Entries by_key_;
  // Paths exactly as entities spell them: allocation- and filesystem-free once
  // warm.
  Entries by_path_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_MESH_CACHE_H_
