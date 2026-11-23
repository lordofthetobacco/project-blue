#ifndef BLUE_RENDER_DRAW_LIST_H_
#define BLUE_RENDER_DRAW_LIST_H_

#include "render/metal_types.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "camera/frustum.h"
#include "render/material_system.h"
#include "render/mesh.h"
#include "render/pipeline.h"
#include "render/recording.h"
#include "render/scene_view.h"

namespace blue::render {

// One submesh to draw in the camera passes (depth pre-pass and mesh pass).
struct DrawEntry {
  uint64_t key = 0;   // Sort key: variant, then material, then mesh.
  uint32_t item = 0;  // Index into SceneView::draws.
  uint32_t first_index = 0;
  uint32_t index_count = 0;
  MaterialId material{};
  MaterialVariant variant = MaterialVariant::kOpaque;
};

// The visible submeshes of a frame, sorted so the passes that walk it change
// pipeline, material and vertex buffers as rarely as possible. Built once per
// frame and shared by every camera pass. Keeps its allocation between
// frames. Not thread-safe.
class DrawList {
 public:
  // Rebuilds the list from the items of `view` whose `world_bounds` (parallel
  // to view.draws) intersect `frustum`; counts the others in
  // stats.culled_draws. Items whose mesh is unknown or released are
  // skipped; unknown or released materials draw as kDefaultMaterial. For a
  // scope's picture (`scope_view`) the items hidden_in_scope are skipped
  // too (not counted as culled).
  void Build(const SceneView& view, std::span<const camera::Aabb> world_bounds,
             const camera::Frustum& frustum, const MaterialSystem& materials,
             const std::vector<Mesh>& meshes, RenderStats& stats,
             bool scope_view = false);

  [[nodiscard]] std::span<const DrawEntry> entries() const { return entries_; }

 private:
  std::vector<DrawEntry> entries_;
};

// How RecordDrawList() draws each MaterialVariant.
struct DrawListPipelines {
  // Null skips the variant's entries.
  std::array<const Pipeline*, kMaterialVariantCount> pipelines{};
  // Binds the entry's material (MaterialSystem::Bind()) for the variant's
  // entries.
  std::array<bool, kMaterialVariantCount> bind_material{};
};

// Records `entries` (sorted by DrawList) on `encoder`, opened on `rec`, with
// the pipeline of each entry's variant: binds the pipeline, the material
// where requested, the mesh's vertex buffer, and the item's model matrix
// (DrawConstants, 64 bytes) as constants at kBufferDraw before each item's
// first draw.
// The caller has set the frame's globals on `encoder`
// (FrameGlobals::Bind()). Adds to `stats` (draw calls and triangles) when
// non-null.
void RecordDrawList(const Recording& rec,
                    id<MTL4RenderCommandEncoder> encoder,
                    std::span<const DrawEntry> entries,
                    const DrawListPipelines& pipelines, const SceneView& view,
                    const MaterialSystem& materials,
                    const std::vector<Mesh>& meshes, RenderStats* stats);

}  // namespace blue::render

#endif  // BLUE_RENDER_DRAW_LIST_H_
