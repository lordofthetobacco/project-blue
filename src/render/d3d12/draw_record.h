#ifndef BLUE_RENDER_D3D12_DRAW_RECORD_H_
#define BLUE_RENDER_D3D12_DRAW_RECORD_H_

#include <d3d12.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "render/d3d12/material_system.h"
#include "render/d3d12/mesh.h"
#include "render/draw_list.h"
#include "render/scene_view.h"

namespace blue::render::d3d12 {

// The backend's meshes and materials as the shared DrawList::Build() reads
// them. Borrows both, which must outlive it; its resources() point at this
// object, so it is neither copyable nor movable.
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
  std::array<ID3D12PipelineState*, kMaterialVariantCount> pipelines{};
  // Binds the material's table (root parameter `material_param`) for the
  // variant's entries.
  std::array<bool, kMaterialVariantCount> bind_material{};
  uint32_t material_param = UINT32_MAX;
};

// The model matrix of a draw: the first 16 root constants (DrawConstants of
// shaders/mesh.slang and shaders/depth.slang).
inline constexpr uint32_t kDrawConstantCount = 16;

// Records `entries` (sorted by DrawList) with the pipeline of each entry's
// variant: binds the material table where requested and sets the item's
// model matrix (root constants) before each item's first draw. The root
// signature (one of the scene ones), the frame set, the render targets, the
// viewport and the scissor must be set. Adds to `stats` (draw calls and
// triangles) when non-null.
void RecordDrawList(ID3D12GraphicsCommandList* cmd,
                    std::span<const DrawEntry> entries,
                    const DrawListPipelines& pipelines, const SceneView& view,
                    const MaterialSystem& materials,
                    const std::vector<Mesh>& meshes, RenderStats* stats);

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_DRAW_RECORD_H_
