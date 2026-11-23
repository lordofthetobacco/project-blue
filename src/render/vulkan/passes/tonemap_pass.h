#ifndef BLUE_RENDER_VULKAN_PASSES_TONEMAP_PASS_H_
#define BLUE_RENDER_VULKAN_PASSES_TONEMAP_PASS_H_

#include <string>

#include "render/vulkan/fullscreen_pass.h"
#include "render/vulkan/passes/frame_context.h"
#include "render/vulkan/render_targets.h"
#include "render/vulkan/samplers.h"
#include "render/vulkan/shader_programs.h"
#include "render/vulkan/vulkan_context.h"

namespace blue::render::vulkan {

// Bloom mix, exposure and tonemapping (shaders/tonemap.frag): HDR target
// (+ bloom mip 0) -> LDR target (sRGB-encoded colour, luma in alpha), with
// SceneView::post. Borrows
// `context` and `programs`, which must outlive it. Neither copyable nor
// movable. Not thread-safe.
class TonemapPass {
 public:
  TonemapPass(const VulkanContext& context, ShaderPrograms& programs,
              const std::string& shader_dir);

  TonemapPass(const TonemapPass&) = delete;
  TonemapPass& operator=(const TonemapPass&) = delete;

  // Points the pass at the current targets; call after they were created or
  // resized, while the GPU is idle.
  void BindTargets(const RenderTargets& targets, const Samplers& samplers);

  // Expects the HDR and bloom targets in SHADER_READ_ONLY_OPTIMAL (the bloom
  // target is only read when bloom is on). Leaves the LDR target in
  // SHADER_READ_ONLY_OPTIMAL.
  void Record(const FrameContext& ctx) const;

 private:
  FullscreenPass pass_;
};

}  // namespace blue::render::vulkan

#endif  // BLUE_RENDER_VULKAN_PASSES_TONEMAP_PASS_H_
