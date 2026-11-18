#ifndef BLUE_RENDER_PASSES_SKYBOX_PASS_H_
#define BLUE_RENDER_PASSES_SKYBOX_PASS_H_

#include <vulkan/vulkan.h>

#include <memory>
#include <string>

#include "render/environment.h"
#include "render/passes/frame_context.h"
#include "render/pipeline.h"
#include "render/shader_programs.h"
#include "render/vulkan_context.h"

namespace blue::render {

// Draws the environment's sky cube behind the scene (shaders/skybox.*): a
// full-screen triangle at reversed-Z depth 0 that only covers pixels the
// opaque meshes left at the cleared depth (GREATER_OR_EQUAL, no depth
// write). Borrows `context`, `programs` and `environment`, which must
// outlive it; the environment's sky view never changes, so the descriptor
// is written once. Neither copyable nor movable. Not thread-safe.
class SkyboxPass {
 public:
  SkyboxPass(const VulkanContext& context, ShaderPrograms& programs,
             const std::string& shader_dir, VkDescriptorSetLayout frame_layout,
             const Environment& environment);
  ~SkyboxPass();

  SkyboxPass(const SkyboxPass&) = delete;
  SkyboxPass& operator=(const SkyboxPass&) = delete;

  // Records the sky. Call inside the scene pass's rendering after the opaque
  // meshes.
  void Draw(const FrameContext& ctx) const;

 private:
  VkDevice device_;
  VkDescriptorSetLayout frame_layout_;
  VkDescriptorSetLayout sky_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet sky_set_ = VK_NULL_HANDLE;
  // Replaced when its shaders are reloaded; never null after construction.
  std::unique_ptr<Pipeline> pipeline_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_SKYBOX_PASS_H_
