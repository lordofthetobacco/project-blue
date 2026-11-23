#ifndef BLUE_RENDER_D3D12_PASSES_SSAO_PASS_H_
#define BLUE_RENDER_D3D12_PASSES_SSAO_PASS_H_

#include "render/d3d12/fullscreen_pass.h"
#include "render/d3d12/passes/frame_context.h"
#include "render/d3d12/root_signature.h"
#include "render/d3d12/shader_programs.h"

namespace blue::render::d3d12 {

class D3d12Context;
class Samplers;

// Screen-space ambient occlusion at half resolution (shaders/ssao.slang)
// from the depth pre-pass result, then a depth-aware separable blur
// (shaders/ssao_blur.slang): depth -> ssao_raw -> ssao_blur -> ssao, the
// same chain and constants as the Vulkan SsaoPass. Borrows everything it is
// given. Neither copyable nor movable.
class SsaoPass {
 public:
  // Most of the screen height the sampling radius may cover.
  static constexpr float kMaxRadiusScreenFraction = 0.1f;

  SsaoPass(const D3d12Context& context, ShaderPrograms& programs,
           const RootSignature& fullscreen_root, const Samplers& samplers);

  SsaoPass(const SsaoPass&) = delete;
  SsaoPass& operator=(const SsaoPass&) = delete;

  // Call after the targets were recreated (which leaves ssao white).
  void TargetsRecreated() { target_white_ = true; }
  // With ctx.plan.ssao records the occlusion and the blur; otherwise
  // nothing, except one clear of ssao to white right after SSAO was
  // switched off. Expects the depth in kDepthReadState; leaves every SSAO
  // target in PIXEL_SHADER_RESOURCE.
  void Record(const FrameContext& ctx);

 private:
  const Samplers& samplers_;
  FullscreenPass occlusion_;
  FullscreenPass blur_horizontal_;  // ssao_raw -> ssao_blur (RG16F)
  FullscreenPass blur_vertical_;    // ssao_blur -> ssao (R8)
  bool target_white_ = true;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_PASSES_SSAO_PASS_H_
