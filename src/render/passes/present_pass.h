#ifndef BLUE_RENDER_PASSES_PRESENT_PASS_H_
#define BLUE_RENDER_PASSES_PRESENT_PASS_H_

#include <vulkan/vulkan.h>

#include <string>

#include "render/fullscreen_pass.h"
#include "render/passes/frame_context.h"
#include "render/render_targets.h"
#include "render/samplers.h"
#include "render/shader_programs.h"
#include "render/vulkan_context.h"

namespace blue::render {

// Writes the LDR target to the swapchain image (shaders/present.frag),
// through FXAA when SceneView::post.fxaa_enabled (using the luma the tonemap
// pass stored in alpha), else as a plain copy. Borrows `context` and
// `programs`, which must outlive it. Neither copyable nor movable. Not
// thread-safe.
class PresentPass {
 public:
  // `swapchain_format` is the format of every swapchain image this pass will
  // draw to; an _SRGB format makes the pass decode the LDR image first.
  PresentPass(const VulkanContext& context, ShaderPrograms& programs,
              const std::string& shader_dir, VkFormat swapchain_format);

  PresentPass(const PresentPass&) = delete;
  PresentPass& operator=(const PresentPass&) = delete;

  // Points the pass at the current targets; call after they were created or
  // resized, while the GPU is idle.
  void BindTargets(const RenderTargets& targets, const Samplers& samplers);

  // Expects the LDR target in SHADER_READ_ONLY_OPTIMAL. Leaves the swapchain
  // image in COLOR_ATTACHMENT_OPTIMAL for the UI pass.
  void Record(const FrameContext& ctx) const;

 private:
  bool srgb_swapchain_;
  FullscreenPass pass_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_PRESENT_PASS_H_
