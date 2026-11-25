#ifndef BLUE_RENDER_DRAW_LIST_H_
#define BLUE_RENDER_DRAW_LIST_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "camera/frustum.h"
#include "material/material.h"
#include "render/scene_view.h"

namespace blue::render {

// The pipeline state a material needs, one mesh pipeline each.
enum class MaterialVariant : uint32_t {
  kOpaque = 0,
  kMasked = 1,             // Alpha-tested: fragments below the cutoff go.
  kDoubleSided = 2,        // No culling; back faces get a flipped normal.
  kMaskedDoubleSided = 3,  // Both.
};
inline constexpr size_t kMaterialVariantCount = 4;

[[nodiscard]] constexpr bool IsMasked(MaterialVariant variant) {
  return variant == MaterialVariant::kMasked ||
         variant == MaterialVariant::kMaskedDoubleSided;
}
[[nodiscard]] constexpr bool IsDoubleSided(MaterialVariant variant) {
  return variant == MaterialVariant::kDoubleSided ||
         variant == MaterialVariant::kMaskedDoubleSided;
}

// Returns whether `item` is drawn into the shadow maps: the one rule every
// backend's shadow pass applies before culling against a cascade.
[[nodiscard]] constexpr bool IsShadowCaster(const DrawItem& item) {
  return item.casts_shadow;
}

// Returns the variant `desc` is drawn with.
[[nodiscard]] MaterialVariant VariantOf(const material::MaterialDesc& desc);

// One submesh to draw in the camera passes (depth pre-pass and mesh pass).
struct DrawEntry {
  uint64_t key = 0;   // Sort key: variant, then material, then mesh.
  uint32_t item = 0;  // Index into SceneView::draws.
  uint32_t first_index = 0;
  uint32_t index_count = 0;
  MaterialId material{};
  MaterialVariant variant = MaterialVariant::kOpaque;
};

// What DrawList::Build() asks the backend about its meshes and materials:
// plain function pointers with one context (no std::function, nothing
// allocated).
struct DrawListResources {
  // The backend object both functions are called with.
  const void* context = nullptr;
  // Returns false when mesh `mesh` is unknown or released (its items are
  // skipped); otherwise sets `index_count` to the mesh's index count.
  bool (*mesh_index_count)(const void* context, MeshId mesh,
                           uint32_t& index_count) = nullptr;
  // Returns `material` resolved (unknown or released: kDefaultMaterial) and
  // sets `variant` to the resolved material's variant.
  MaterialId (*resolve_material)(const void* context, MaterialId material,
                                 MaterialVariant& variant) = nullptr;
};

// The visible submeshes of a frame, sorted so the passes that walk it change
// pipeline, material and vertex buffers as rarely as possible. Built once per
// frame and shared by every camera pass. Shared by the backends; each records
// it with its own API. Keeps its allocation between frames. Not thread-safe.
class DrawList {
 public:
  // Rebuilds the list from the items of `view` whose `world_bounds` (parallel
  // to view.draws) intersect `frustum`; counts the others in
  // stats.culled_draws. Items whose mesh is unknown or released are
  // skipped; unknown or released materials draw as kDefaultMaterial. Items
  // shadow_only are never listed (the shadow passes walk view.draws
  // themselves), and for a scope's picture (`scope_view`) neither are the
  // items hidden_in_scope; neither counts as culled. Allocates nothing once
  // the list has grown to its steady size.
  void Build(const SceneView& view, std::span<const camera::Aabb> world_bounds,
             const camera::Frustum& frustum, const DrawListResources& resources,
             RenderStats& stats, bool scope_view = false);

  [[nodiscard]] std::span<const DrawEntry> entries() const { return entries_; }

 private:
  std::vector<DrawEntry> entries_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_DRAW_LIST_H_
