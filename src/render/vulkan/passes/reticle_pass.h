#ifndef BLUE_RENDER_VULKAN_PASSES_RETICLE_PASS_H_
#define BLUE_RENDER_VULKAN_PASSES_RETICLE_PASS_H_

#include <vulkan/vulkan.h>

#include <memory>
#include <string>

#include "render/vulkan/passes/frame_context.h"
#include "render/vulkan/pipeline.h"
#include "render/vulkan/shader_programs.h"
#include "render/vulkan/vulkan_context.h"

namespace blue::render::vulkan {

// Draws the reflex sight of SceneView::sight (shaders/reticle.*): an
// additive, depth-tested quad without a vertex buffer. Borrows `context`
// and `programs`, which must outlive it. Neither copyable nor movable. Not
// thread-safe.
class ReticlePass {
 public:
  ReticlePass(const VulkanContext& context, ShaderPrograms& programs,
              const std::string& shader_dir,
              VkDescriptorSetLayout frame_layout);

  ReticlePass(const ReticlePass&) = delete;
  ReticlePass& operator=(const ReticlePass&) = delete;

  // Records the sight, if the view has one. Call inside the scene pass's
  // rendering after the opaque meshes, so their depth hides the window.
  void Draw(const FrameContext& ctx) const;

 private:
  VkDevice device_;
  VkDescriptorSetLayout frame_layout_;
  // Replaced when its shaders are reloaded; never null after construction.
  std::unique_ptr<Pipeline> pipeline_;
};

}  // namespace blue::render::vulkan

#endif  // BLUE_RENDER_VULKAN_PASSES_RETICLE_PASS_H_
