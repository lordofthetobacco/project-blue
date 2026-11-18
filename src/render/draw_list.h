#ifndef BLUE_RENDER_DRAW_LIST_H_
#define BLUE_RENDER_DRAW_LIST_H_

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "camera/frustum.h"
#include "render/material_system.h"
#include "render/mesh.h"
#include "render/pipeline.h"
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
  // skipped; unknown or released materials draw as kDefaultMaterial.
  void Build(const SceneView& view, std::span<const camera::Aabb> world_bounds,
             const camera::Frustum& frustum, const MaterialSystem& materials,
             const std::vector<Mesh>& meshes, RenderStats& stats);

  [[nodiscard]] std::span<const DrawEntry> entries() const { return entries_; }

 private:
  std::vector<DrawEntry> entries_;
};

// How RecordDrawList() draws each MaterialVariant.
struct DrawListPipelines {
  // Null skips the variant's entries.
  std::array<const Pipeline*, kMaterialVariantCount> pipelines{};
  // Binds the material's set as set 1 for the variant's entries.
  std::array<bool, kMaterialVariantCount> bind_material{};
};

// Records `entries` (sorted by DrawList) with the pipeline of each entry's
// variant: binds `frame_set` as set 0, the material set as set 1 where
// requested, and pushes the item's model matrix (DrawConstants, 64 bytes,
// vertex stage) before each item's first draw. Every pipeline must use the
// DrawConstants push range. Adds to `stats` (draw calls and triangles) when
// non-null. Must be recorded inside a rendering scope.
void RecordDrawList(VkCommandBuffer cmd, std::span<const DrawEntry> entries,
                    const DrawListPipelines& pipelines,
                    VkDescriptorSet frame_set, const SceneView& view,
                    const MaterialSystem& materials,
                    const std::vector<Mesh>& meshes, RenderStats* stats);

// The push constant range every DrawList pipeline declares: the model
// matrix, read by the vertex stage (shaders/mesh.vert, shaders/depth.vert).
inline constexpr VkPushConstantRange kDrawConstantsRange{
    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    .offset = 0,
    .size = 64,
};

}  // namespace blue::render

#endif  // BLUE_RENDER_DRAW_LIST_H_
