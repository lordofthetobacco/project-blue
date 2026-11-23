#ifndef BLUE_RENDER_D3D12_PASSES_TONEMAP_PASS_H_
#define BLUE_RENDER_D3D12_PASSES_TONEMAP_PASS_H_

#include "render/d3d12/fullscreen_pass.h"
#include "render/d3d12/passes/frame_context.h"
#include "render/d3d12/root_signature.h"
#include "render/d3d12/shader_programs.h"

namespace blue::render::d3d12 {

class D3d12Context;
class Samplers;

// Bloom mix, exposure and tonemapping (shaders/tonemap.slang): HDR target
// (+ bloom, from package D3 on; a null view until then, which the shader
// does not read with bloom off) -> LDR target (sRGB-encoded colour, luma in
// alpha), with SceneView::post. Borrows everything it is given. Neither
// copyable nor movable.
class TonemapPass {
 public:
  TonemapPass(const D3d12Context& context, ShaderPrograms& programs,
              const RootSignature& fullscreen_root, const Samplers& samplers);

  TonemapPass(const TonemapPass&) = delete;
  TonemapPass& operator=(const TonemapPass&) = delete;

  // Expects the HDR target in PIXEL_SHADER_RESOURCE; leaves the LDR target
  // in PIXEL_SHADER_RESOURCE.
  void Record(const FrameContext& ctx) const;

 private:
  const Samplers& samplers_;
  FullscreenPass pass_;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_PASSES_TONEMAP_PASS_H_
