#ifndef BLUE_RENDER_D3D12_PASSES_DEPTH_PREPASS_H_
#define BLUE_RENDER_D3D12_PASSES_DEPTH_PREPASS_H_

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <span>
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

// Writes the scene depth of the camera view before anything is shaded
// (shaders/depth.slang; alpha-masked materials add its masked fragment stage
// and bind their material table). The mesh pass tests EQUAL against it, so
// every visible pixel is shaded exactly once. Uses the scene + material root
// signature. Borrows everything it is given. Neither copyable nor movable.
class DepthPrepass {
 public:
  DepthPrepass(const D3d12Context& context, ShaderPrograms& programs,
               const RootSignature& root_signature,
               const FrameGlobals& frame_globals,
               const MaterialSystem& materials,
               const std::vector<Mesh>& meshes);

  DepthPrepass(const DepthPrepass&) = delete;
  DepthPrepass& operator=(const DepthPrepass&) = delete;

  // Clears the depth target and draws ctx.draw_list into it. Leaves the
  // depth target in RenderTargets::kDepthReadState.
  void Record(const FrameContext& ctx) const;

 private:
  // Creates the pipelines of the variants selected by `masked` from one
  // compiled program.
  void CreatePipelines(bool masked, std::span<const std::byte> vertex,
                       std::span<const std::byte> pixel);

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

#endif  // BLUE_RENDER_D3D12_PASSES_DEPTH_PREPASS_H_
