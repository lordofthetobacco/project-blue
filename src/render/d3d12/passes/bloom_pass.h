#ifndef BLUE_RENDER_D3D12_PASSES_BLOOM_PASS_H_
#define BLUE_RENDER_D3D12_PASSES_BLOOM_PASS_H_

#include "render/d3d12/fullscreen_pass.h"
#include "render/d3d12/passes/frame_context.h"
#include "render/d3d12/root_signature.h"
#include "render/d3d12/shader_programs.h"

namespace blue::render::d3d12 {

class D3d12Context;
class Samplers;

// The bloom chain (shaders/bloom_down.slang, bloom_up.slang), as the Vulkan
// BloomPass: hdr is downsampled into every bloom level (13-tap, Karis
// average for the first), then each level is upsampled (tent) and added onto
// the next larger one, so level 0 ends up holding the sum TonemapPass
// reads. Borrows everything it is given. Neither copyable nor movable.
class BloomPass {
 public:
  BloomPass(const D3d12Context& context, ShaderPrograms& programs,
            const RootSignature& fullscreen_root, const Samplers& samplers);

  BloomPass(const BloomPass&) = delete;
  BloomPass& operator=(const BloomPass&) = delete;

  // Expects hdr in PIXEL_SHADER_RESOURCE; leaves every bloom level in
  // PIXEL_SHADER_RESOURCE.
  void Record(const FrameContext& ctx) const;

 private:
  const Samplers& samplers_;
  FullscreenPass downsample_;
  FullscreenPass upsample_;  // Additive.
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_PASSES_BLOOM_PASS_H_
