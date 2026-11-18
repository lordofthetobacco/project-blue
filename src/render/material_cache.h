#ifndef BLUE_RENDER_MATERIAL_CACHE_H_
#define BLUE_RENDER_MATERIAL_CACHE_H_

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "material/material.h"
#include "render/renderer.h"
#include "render/scene_view.h"

namespace blue::render {

// Resolves .bmat material files (scene::Entity::material_path) to renderer
// materials, loading each file once. A file that is missing or broken is
// logged once and remembered as failed, so per-frame lookups of a bad path
// stay cheap and quiet; Reload() retries it. Lookups by an already seen
// spelling allocate nothing. Borrows `renderer`, which must outlive the
// cache. Neither copyable nor movable. Not thread-safe.
class MaterialCache {
 public:
  explicit MaterialCache(Renderer& renderer) : renderer_(renderer) {}

  MaterialCache(const MaterialCache&) = delete;
  MaterialCache& operator=(const MaterialCache&) = delete;

  // Returns the material of the .bmat file `path`, loading it on first use,
  // or nullopt if it cannot be loaded (warned once).
  [[nodiscard]] std::optional<MaterialId> Get(std::string_view path);

  // Reads `path` again and applies it to its material from the next frame
  // on (Renderer::UpdateMaterial), or creates it if it was never loaded or
  // had failed. Returns the material, or nullopt if the file cannot be
  // loaded (logged; a material loaded before keeps its last version).
  std::optional<MaterialId> Reload(std::string_view path);

 private:
  struct StringHash {
    using is_transparent = void;
    [[nodiscard]] size_t operator()(std::string_view text) const {
      return std::hash<std::string_view>{}(text);
    }
  };

  // Returns the by_key_ entry of `path`, loading the file on first use.
  [[nodiscard]] std::optional<MaterialId>* Entry(std::string_view path);
  // Loads the file `path` into material `existing` when given, else into a
  // new one. Returns nullopt on failure (logged).
  [[nodiscard]] std::optional<MaterialId> Load(
      std::string_view path, std::optional<MaterialId> existing);

  Renderer& renderer_;
  // Normalized absolute path -> material; nullopt marks a failed load.
  std::unordered_map<std::string, std::optional<MaterialId>, StringHash,
                     std::equal_to<>>
      by_key_;
  // Paths exactly as callers spell them -> their by_key_ entry (node
  // addresses of an unordered_map are stable).
  std::unordered_map<std::string, std::optional<MaterialId>*, StringHash,
                     std::equal_to<>>
      by_path_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_MATERIAL_CACHE_H_
