#ifndef BLUE_RENDER_METAL_PASSES_BLOOM_PASS_H_
#define BLUE_RENDER_METAL_PASSES_BLOOM_PASS_H_

#include "render/metal/metal_types.h"

#include <string>

#include "render/metal/fullscreen_pass.h"
#include "render/metal/metal_context.h"
#include "render/metal/passes/frame_context.h"
#include "render/metal/render_targets.h"
#include "render/metal/samplers.h"
#include "render/metal/shader_programs.h"

namespace blue::render::metal {

// Threshold-free bloom (the "physically based" variant of the Call of Duty
// chain): the HDR image is downsampled into the bloom target's mips with a
// 13-tap filter (shaders/bloom_down.frag.metal; Karis average on the first
// step), then upsampled back with a tent filter, each level added onto the
// next larger one (shaders/bloom_up.frag.metal). Mip 0 then holds the sum of
// all levels; TonemapPass mixes it into the image (bloom_strength). Borrows
// `context` and `programs`, which must outlive it. Neither copyable nor
// movable. Not thread-safe.
class BloomPass {
 public:
  BloomPass(const MetalContext& context, ShaderPrograms& programs,
            const std::string& shader_dir);

  BloomPass(const BloomPass&) = delete;
  BloomPass& operator=(const BloomPass&) = delete;

  // Points the pass at the current targets; call after they were created or
  // resized.
  void BindTargets(const RenderTargets& targets, const Samplers& samplers);

  // Reads the HDR target and writes every bloom mip, one render encoder per
  // step.
  void Record(const FrameContext& ctx) const;

 private:
  FullscreenPass downsample_;  // Input set i: the source of mip i.
  FullscreenPass upsample_;    // Input set i: mip i, added onto mip i - 1.
};

}  // namespace blue::render::metal

#endif  // BLUE_RENDER_METAL_PASSES_BLOOM_PASS_H_
