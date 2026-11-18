#ifndef BLUE_RENDER_PASSES_BLOOM_PASS_H_
#define BLUE_RENDER_PASSES_BLOOM_PASS_H_

#include <string>

#include "render/fullscreen_pass.h"
#include "render/passes/frame_context.h"
#include "render/render_targets.h"
#include "render/samplers.h"
#include "render/shader_programs.h"
#include "render/vulkan_context.h"

namespace blue::render {

// Threshold-free bloom (the "physically based" variant of the Call of Duty
// chain): the HDR image is downsampled into the bloom target's mips with a
// 13-tap filter (shaders/bloom_down.frag; Karis average on the first step),
// then upsampled back with a tent filter, each level added onto the next
// larger one (shaders/bloom_up.frag). Mip 0 then holds the sum of all
// levels; TonemapPass mixes it into the image (bloom_strength). Borrows
// `context` and `programs`, which must outlive it. Neither copyable nor
// movable. Not thread-safe.
class BloomPass {
 public:
  BloomPass(const VulkanContext& context, ShaderPrograms& programs,
            const std::string& shader_dir);

  BloomPass(const BloomPass&) = delete;
  BloomPass& operator=(const BloomPass&) = delete;

  // Points the pass at the current targets; call after they were created or
  // resized, while the GPU is idle.
  void BindTargets(const RenderTargets& targets, const Samplers& samplers);

  // Expects the HDR target in SHADER_READ_ONLY_OPTIMAL. Leaves every bloom
  // mip in SHADER_READ_ONLY_OPTIMAL. Must be recorded outside of a
  // rendering pass.
  void Record(const FrameContext& ctx) const;

 private:
  FullscreenPass downsample_;  // Input set i: the source of mip i.
  FullscreenPass upsample_;    // Input set i: mip i, added onto mip i - 1.
};

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_BLOOM_PASS_H_
