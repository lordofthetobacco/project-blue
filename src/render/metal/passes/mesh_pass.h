#ifndef BLUE_RENDER_METAL_PASSES_MESH_PASS_H_
#define BLUE_RENDER_METAL_PASSES_MESH_PASS_H_

#include "render/metal/metal_types.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "render/metal/material_system.h"
#include "render/metal/mesh.h"
#include "render/metal/metal_context.h"
#include "render/metal/passes/frame_context.h"
#include "render/metal/pipeline.h"
#include "render/metal/shader_programs.h"

namespace blue::render::metal {

// Shades the frame's draw list with the PBR shaders
// (shaders/mesh.vert.metal / mesh.frag.metal). The depth pre-pass has
// already written the scene depth, so this pass tests EQUAL without writing
// depth: every pixel is shaded once. The fragment stage sees FrameGlobals'
// resources and the material's (MaterialSystem::Bind()); the model matrix
// is the per-draw vertex bytes (the normal matrix is derived in the vertex
// shader).
//
// Borrows everything it is given, which must outlive it. Neither copyable
// nor movable. Not thread-safe.
class MeshPass {
 public:
  MeshPass(const MetalContext& context, ShaderPrograms& programs,
           const std::string& shader_dir, const MaterialSystem& materials,
           const std::vector<Mesh>& meshes);

  MeshPass(const MeshPass&) = delete;
  MeshPass& operator=(const MeshPass&) = delete;

  // Records ctx.draw_list on `encoder`, the scene encoder (BeginScenePass());
  // leaves an arbitrary pipeline bound.
  void Draw(const FrameContext& ctx,
            id<MTL4RenderCommandEncoder> encoder) const;

 private:
  const MaterialSystem& materials_;
  const std::vector<Mesh>& meshes_;
  // Indexed by MaterialVariant; replaced when the shaders are reloaded,
  // never null after construction.
  std::array<std::unique_ptr<Pipeline>, kMaterialVariantCount> pipelines_;
};

}  // namespace blue::render::metal

#endif  // BLUE_RENDER_METAL_PASSES_MESH_PASS_H_
