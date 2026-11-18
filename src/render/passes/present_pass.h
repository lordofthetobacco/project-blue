#ifndef BLUE_RENDER_PASSES_PRESENT_PASS_H_
#define BLUE_RENDER_PASSES_PRESENT_PASS_H_

#include <vulkan/vulkan.h>

#include <string>

#include "render/barriers.h"
#include "render/fullscreen_pass.h"
#include "render/passes/frame_context.h"
#include "render/render_targets.h"
#include "render/samplers.h"
#include "render/shader_programs.h"
#include "render/vulkan_context.h"

namespace blue::render {

// The image a PresentPass writes: the swapchain image or the editor
// viewport image. Its previous contents are discarded.
struct PresentTarget {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkExtent2D extent{};
  // The last access to the image before this pass, which the pass's layout
  // transition waits for (nothing for a freshly acquired swapchain image
  // beyond the acquire semaphore; the previous frame's UI sampling for the
  // viewport image).
  BarrierScope previous_use;
};

// Writes the LDR target to an output image (shaders/present.frag),
// through FXAA when SceneView::post.fxaa_enabled (using the luma the tonemap
// pass stored in alpha), else as a plain copy. Borrows `context` and
// `programs`, which must outlive it. Neither copyable nor movable. Not
// thread-safe.
class PresentPass {
 public:
  // `output_format` is the format of every image this pass will draw to;
  // an _SRGB format makes the pass decode the LDR image first.
  PresentPass(const VulkanContext& context, ShaderPrograms& programs,
              const std::string& shader_dir, VkFormat output_format);

  PresentPass(const PresentPass&) = delete;
  PresentPass& operator=(const PresentPass&) = delete;

  // Points the pass at the current targets; call after they were created or
  // resized, while the GPU is idle.
  void BindTargets(const RenderTargets& targets, const Samplers& samplers);

  // Expects the LDR target in SHADER_READ_ONLY_OPTIMAL. Leaves `target` in
  // COLOR_ATTACHMENT_OPTIMAL.
  void Record(const FrameContext& ctx, const PresentTarget& target) const;

 private:
  bool srgb_output_;
  FullscreenPass pass_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_PRESENT_PASS_H_
