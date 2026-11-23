#ifndef BLUE_RENDER_VULKAN_PASSES_SCOPE_LENS_PASS_H_
#define BLUE_RENDER_VULKAN_PASSES_SCOPE_LENS_PASS_H_

#include <vulkan/vulkan.h>

#include <array>
#include <memory>
#include <string>

#include "render/frame_config.h"
#include "render/vulkan/buffer.h"
#include "render/vulkan/passes/frame_context.h"
#include "render/vulkan/pipeline.h"
#include "render/vulkan/shader_programs.h"
#include "render/vulkan/vulkan_context.h"

namespace blue::render::vulkan {

// Draws the eyepiece glass of SceneView::scope (shaders/scope_lens.*): a
// depth-tested disc on the weapon, alpha-blended by its antialiased rim,
// showing the scope's picture through the eyepiece's optics and reticle.
// Set 0 is FrameGlobals' (the main view's), set 1 the pass's own: the lens
// uniforms (one buffer per frame in flight) and the picture.
//
// Borrows `context` and `programs`, which must outlive it. Neither copyable
// nor movable. Not thread-safe.
class ScopeLensPass {
 public:
  ScopeLensPass(const VulkanContext& context, ShaderPrograms& programs,
                VkDescriptorSetLayout frame_layout);
  ~ScopeLensPass();

  ScopeLensPass(const ScopeLensPass&) = delete;
  ScopeLensPass& operator=(const ScopeLensPass&) = delete;

  // Points every frame's set at the scope picture `view` (sampled with
  // `sampler` in SHADER_READ_ONLY_OPTIMAL). No frame may be in flight that
  // uses the sets. Draw() records nothing before the first call.
  void BindPicture(VkImageView view, VkSampler sampler);

  // Records the eyepiece, if the view has a scope. Call inside the main
  // scene pass's rendering after the opaque meshes, so their depth hides
  // the glass; the picture must be in SHADER_READ_ONLY_OPTIMAL.
  void Draw(const FrameContext& ctx) const;

 private:
  struct Frame {
    Buffer uniforms;
    VkDescriptorSet set = VK_NULL_HANDLE;
  };

  VkDevice device_;
  VkDescriptorSetLayout frame_layout_;
  VkDescriptorSetLayout lens_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  std::array<Frame, kFramesInFlight> frames_;
  bool picture_bound_ = false;
  // Replaced when its shaders are reloaded; never null after construction.
  std::unique_ptr<Pipeline> pipeline_;
};

}  // namespace blue::render::vulkan

#endif  // BLUE_RENDER_VULKAN_PASSES_SCOPE_LENS_PASS_H_
