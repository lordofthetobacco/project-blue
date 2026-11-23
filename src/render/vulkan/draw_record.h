#ifndef BLUE_RENDER_VULKAN_DRAW_RECORD_H_
#define BLUE_RENDER_VULKAN_DRAW_RECORD_H_

#include <vulkan/vulkan.h>

#include <array>
#include <span>
#include <vector>

#include "render/draw_list.h"
#include "render/scene_view.h"
#include "render/vulkan/material_system.h"
#include "render/vulkan/mesh.h"
#include "render/vulkan/pipeline.h"

namespace blue::render::vulkan {

// The renderer's meshes and materials as DrawList::Build() reads them.
// Borrows both, which must outlive it; its resources() point at this object,
// so it is neither copyable nor movable.
class DrawListSource {
 public:
  DrawListSource(const MaterialSystem& materials,
                 const std::vector<Mesh>& meshes)
      : materials_(materials), meshes_(meshes) {}

  DrawListSource(const DrawListSource&) = delete;
  DrawListSource& operator=(const DrawListSource&) = delete;

  [[nodiscard]] DrawListResources resources() const;

 private:
  const MaterialSystem& materials_;
  const std::vector<Mesh>& meshes_;
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
// matrix, read by the vertex stage (shaders/mesh.slang, shaders/depth.slang).
inline constexpr VkPushConstantRange kDrawConstantsRange{
    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    .offset = 0,
    .size = 64,
};

}  // namespace blue::render::vulkan

#endif  // BLUE_RENDER_VULKAN_DRAW_RECORD_H_
