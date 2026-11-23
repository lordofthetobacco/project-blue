#ifndef BLUE_RENDER_D3D12_PASSES_MESH_PASS_H_
#define BLUE_RENDER_D3D12_PASSES_MESH_PASS_H_

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <vector>

#include "render/d3d12/frame_globals.h"
#include "render/d3d12/material_system.h"
#include "render/d3d12/mesh.h"
#include "render/d3d12/passes/frame_context.h"
#include "render/d3d12/root_signature.h"
#include "render/d3d12/shader_programs.h"
#include "render/draw_list.h"

namespace blue::render::d3d12 {

class D3d12Context;

// Shades the frame's draw list with the PBR shaders (shaders/mesh.slang, one
// program per MaterialVariant) into the HDR target. The depth pre-pass has
// written the scene depth, so this pass tests EQUAL against the read-only
// depth: every pixel is shaded once (both vertex stages compute the
// position with the `precise` ClipPosition()). Uses the scene + material
// root signature. Borrows everything it is given. Neither copyable nor
// movable.
class MeshPass {
 public:
  MeshPass(const D3d12Context& context, ShaderPrograms& programs,
           const RootSignature& root_signature,
           const FrameGlobals& frame_globals, const MaterialSystem& materials,
           const std::vector<Mesh>& meshes);

  MeshPass(const MeshPass&) = delete;
  MeshPass& operator=(const MeshPass&) = delete;

  // Records ctx.draw_list. Call inside the scene pass (BeginScenePass());
  // sets its own root signature.
  void Draw(const FrameContext& ctx) const;

 private:
  const D3d12Context& context_;
  const RootSignature& root_signature_;
  SceneRootParams params_;
  const FrameGlobals& frame_globals_;
  const MaterialSystem& materials_;
  const std::vector<Mesh>& meshes_;
  // Indexed by MaterialVariant; replaced on a shader reload.
  std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, kMaterialVariantCount>
      pipelines_;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_PASSES_MESH_PASS_H_
